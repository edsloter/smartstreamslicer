/*
 * SmartStreamSlicer (sss) - smart streaming splitter/joiner
 * Copyright (C) 2026 Edward Sloter
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "chunker.h"

#include "cli.h"
#include "matcher.h"
#include "progress.h"
#include "sss_io.h"

#include "fastcdc/fastcdc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sss {

SplitEnv make_split_env(const Options& o) {
    const uint64_t base = o.target != 0 ? o.target : 1024ULL * 1024 * 1024;

    SplitEnv e;
    e.target = base;
    e.min_sz = o.min_sz != 0 ? o.min_sz : base / 4 * 3;
    e.max_sz = o.max_sz != 0 ? o.max_sz : base / 4 * 5;

    e.min_sz = std::max<uint64_t>(e.min_sz, 1);
    e.max_sz = std::max<uint64_t>(e.max_sz, base);
    e.target = std::clamp(e.target, e.min_sz, e.max_sz);
    return e;
}

namespace {

constexpr size_t kWin = 256;
constexpr size_t kRunLen = 16;
constexpr uint64_t kFloor = 64;
// Feed fastcdc in large blocks: its Process() memmoves the unconsumed
// trailing buffer on every call, so small reads make large-target chunking
// superlinear (e.g. ~1024 * ~256 MiB self-memmoves per 1 GiB chunk at a
// 1 GiB average). 64 MiB reads keep that overhead negligible for real
// workloads while bounding memory use.
constexpr uint64_t kBuf = 64ULL << 20;

using cdc_ft::fastcdc::Chunker;
using cdc_ft::fastcdc::Config;

bool read_window(FILE* f, uint64_t size, uint64_t cut, std::vector<uint8_t>& buf, uint64_t& base) {
    const uint64_t lo = cut >= kWin ? cut - kWin : 0;
    const uint64_t hi = std::min<uint64_t>(size, cut + kWin + 1);
    buf.resize(static_cast<size_t>(hi - lo));
    if (buf.empty()) {
        base = lo;
        return true;
    }
    if (!file_seek(f, static_cast<int64_t>(lo), SEEK_SET)) {
        buf.clear();
        return false;
    }
    if (!safe_read(f, buf.data(), buf.size())) {
        buf.clear();
        return false;
    }
    base = lo;
    return true;
}

uint64_t apply_rules(const uint8_t* w, size_t wlen, uint64_t base, uint64_t prev, uint64_t cut,
                     uint64_t next, const BoundaryMatcher& m) {
    const size_t idx = static_cast<size_t>(cut - base);
    uint64_t c = cut;

    if (idx < wlen && idx > 0 && w[idx - 1] == w[idx]) {
        size_t right = idx;
        while (right < wlen && w[right] == w[idx]) ++right;
        size_t left = idx;
        while (left > 0 && w[left - 1] == w[idx]) --left;
        if (right - left >= kRunLen) c = base + right;
    }
    if (c > next) c = next;
    if (c <= prev) c = cut;

    size_t best = 0;
    if (m.nearest_delimiter(w, wlen, static_cast<size_t>(c - base), best)) {
        const uint64_t d = base + best;
        if (d != c && d <= next && d > prev && d - prev >= kFloor && next - d >= kFloor) {
            c = d;
        }
    }
    return c;
}

} // namespace

bool content_boundaries(FILE* f, uint64_t size, const SplitEnv& env,
                        const BoundaryMatcher& matcher, std::vector<uint64_t>& out,
                        Progress* prog) {
    out.clear();
    if (size <= env.max_sz) return true;
    if (env.min_sz == 0 || env.target < env.min_sz || env.max_sz < env.target) return true;

    std::vector<uint8_t> buf(kBuf);
    std::vector<uint64_t> raw;
    Config cfg(static_cast<size_t>(env.min_sz), static_cast<size_t>(env.target),
               static_cast<size_t>(env.max_sz));
    uint64_t pos = 0;
    Chunker chunker(cfg, [&](const uint8_t*, size_t n) {
        pos += n;
        raw.push_back(pos);
    });
    for (;;) {
        const size_t n = std::fread(buf.data(), 1, kBuf, f);
        if (n == 0) {
            if (std::ferror(f) != 0) return false;
            break;
        }
        chunker.Process(buf.data(), n);
        if (prog) prog->add(n);
    }
    chunker.Finalize();
    if (raw.empty() || raw.back() != size) return true;

    std::vector<uint8_t> win;
    uint64_t prev = 0;
    out.reserve(raw.size());
    for (size_t i = 0; i + 1 < raw.size(); ++i) {
        uint64_t base = 0;
        if (!read_window(f, size, raw[i], win, base)) return true;
        const uint64_t c = apply_rules(win.data(), win.size(), base, prev, raw[i], raw[i + 1],
                                       matcher);
        if (c > prev) {
            out.push_back(c - prev);
            prev = c;
        }
    }
    if (prev < size) out.push_back(size - prev);
    return true;
}

} // namespace sss