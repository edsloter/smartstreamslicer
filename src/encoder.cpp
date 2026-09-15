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
#include "encoder.h"

#include "chunker.h"
#include "cli.h"
#include "matcher.h"
#include "format.h"
#include "fsutil.h"
#include "hash.h"
#include "progress.h"
#include "sss_io.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace sss {
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kCopyBuf = 1 << 20;
constexpr uint64_t kMinPart = kStreamHeaderLen + kChunkHeaderLen + kChunkFooterLen + 256;

struct PendingFile {
    std::string name;
    fs::path abs_path;
    uint64_t size = 0;
    uint64_t payload_xxh3 = 0;
    std::vector<Segment> segments;
    std::vector<uint64_t> boundaries;
};

struct ChunkEntry {
    uint32_t file_id = 0;
    uint64_t payload_len = 0;
    uint32_t seg_index = 0;
};

struct ChunkPlan {
    uint64_t part_size = 0;
    std::vector<ChunkEntry> entries;
};

struct SimResult {
    std::vector<PendingFile> files;
    std::vector<ChunkPlan> chunks;
    uint64_t total_payload = 0;
    uint64_t total_bytes = 0;
};

uint64_t entry_len(const std::string& name, uint64_t segments) {
    return 2 + name.size() + 8 + 8 + 4 + segments * 20;
}

std::string human_size(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ULL * 1024 * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f TiB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f GiB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

// Packs whole content-defined chunks (cumulative offsets, strictly inner
// increasing, sourced from the FastCDC run/delimiter-aware pass) into parts.
// `try_leading` copies part hosting the file in the current part; `has_leading`
// reports whether out[0] is that leading take (false when the first chunk does
// not fit the remaining headroom and a fresh part must be started instead).
// Every take keeps its part within --max; the whole-file packing is only
// possible when every chunk individually fits a fresh part (pc_cap), otherwise
// false is returned and the caller falls back to fixed-size slicing.
bool pack_content(const std::vector<uint64_t>& bounds, uint64_t size, uint64_t p0_cap,
                  uint64_t pc_cap, bool try_leading, bool& has_leading,
                  std::vector<uint64_t>& out) {
    out.clear();
    has_leading = false;
    if (bounds.empty() || bounds.back() != size || pc_cap == 0) return false;

    auto chunk_size = [&](size_t k) -> uint64_t {
        return k == 0 ? bounds[0] : bounds[k] - bounds[k - 1];
    };
    for (size_t k = 0; k < bounds.size(); ++k) {
        if (chunk_size(k) > pc_cap) return false;
    }

    size_t cur = 0;       // index of next unconsumed chunk end
    uint64_t consumed = 0;
    if (try_leading) {
        while (cur < bounds.size() && bounds[cur] <= p0_cap) ++cur;
        if (cur == 0) {
            try_leading = false;  // first chunk cannot share the current part
        } else {
            consumed = bounds[cur - 1];
            out.push_back(consumed);
            has_leading = true;
        }
    }

    uint64_t seg_start = consumed;
    while (consumed < size) {
        const uint64_t target = consumed + pc_cap;
        while (cur < bounds.size() && bounds[cur] <= target) ++cur;
        if (cur == 0 || bounds[cur - 1] == consumed) return false;  // chunk too wide
        consumed = bounds[cur - 1];
        out.push_back(consumed - seg_start);
        seg_start = consumed;
    }
    return true;
}

// Builds the exact per-file segment maps and per-part byte layout without
// touching any I/O. The result drives --dry-run, the StreamHeader bookkeeping,
// and the byte-for-byte script the write pass follows.
SimResult simulate(const std::vector<PathEntry>& entries, const SplitEnv& env,
                   const std::vector<std::vector<uint64_t>>& boundaries) {
    constexpr uint64_t sh = kStreamHeaderLen;
    constexpr uint64_t ch = kChunkHeaderLen;
    constexpr uint64_t ft = kChunkFooterLen;

    SimResult r;
    r.files.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        PendingFile pf;
        pf.name = e.rel_name;
        pf.abs_path = e.abs_path;
        pf.size = e.size;
        pf.boundaries = boundaries[i];
        r.files.push_back(std::move(pf));
        r.total_payload += e.size;
    }

    uint64_t part_bytes = sh + ch;  // current part, footer not yet counted
    uint32_t chunk_index = 0;
    uint64_t cur_entries = 0;
    std::vector<ChunkEntry> cur_list;
    std::vector<uint64_t> file_done(r.files.size(), 0);

    auto finalize_chunk = [&]() {
        r.chunks.push_back(ChunkPlan{part_bytes + ft, std::move(cur_list)});
        cur_list = {};
        part_bytes = sh + ch;
        chunk_index++;
        cur_entries = 0;
    };

    const auto cap_after = [&](uint64_t used) -> uint64_t {
        return env.max_sz > used ? env.max_sz - used : 0;
    };

    for (uint32_t fid = 0; fid < r.files.size(); ++fid) {
        PendingFile& pf = r.files[fid];

        const uint64_t L1 = entry_len(pf.name, 1);
        if (L1 + pf.size + part_bytes + ft <= env.max_sz) {
            // Fits entirely in the current part.
            if (cur_entries > 0 && part_bytes + ft >= env.min_sz &&
                part_bytes + ft + L1 + pf.size > env.target) {
                finalize_chunk();
            }
            Segment seg;
            seg.chunk_index = chunk_index;
            seg.offset_in_chunk = part_bytes + L1;
            seg.length = pf.size;
            pf.segments.push_back(seg);
            part_bytes += L1 + pf.size;
            cur_entries++;
            cur_list.push_back(ChunkEntry{fid, pf.size, static_cast<uint32_t>(file_done[fid]++)});
            continue;
        }

        // Oversized relative to the current part headroom: sub-split across
        // part boundaries. Every part hosting the file carries a full copy of
        // the entry (including the complete segment map) so each part stays
        // self describing. Preferred cut points are whole content-defined
        // chunks (FastCDC + run/delimiter rules); the fixed-size layout is the
        // fallback when content chunking is unusable or a chunk cannot fit a
        // single part.
        uint64_t c = 1;
        uint64_t L = L1;
        uint64_t p0_cap = 0;
        uint64_t pc_cap = 0;
        std::vector<uint64_t> takes;
        bool has_leading = false;
        bool packed = false;
        if (!pf.boundaries.empty()) {
            for (int iter = 0; iter < 12; ++iter) {
                L = entry_len(pf.name, c);
                p0_cap = cap_after(part_bytes + ft + L);
                pc_cap = cap_after(sh + ch + ft + L);
                bool has_leading_next = false;
                const bool try_lead = cur_entries > 0 && p0_cap > 0;
                if (!pack_content(pf.boundaries, pf.size, p0_cap, pc_cap, try_lead,
                                  has_leading_next, takes)) {
                    break;
                }
                const uint64_t cnew = static_cast<uint64_t>(takes.size());
                if (cnew == c) {
                    has_leading = has_leading_next;
                    packed = true;
                    break;
                }
                c = cnew;
            }
        }

        if (!packed) {
            c = 1;
            L = L1;
            p0_cap = 0;
            pc_cap = 0;
            for (int iter = 0; iter < 16; ++iter) {
                L = entry_len(pf.name, c);
                p0_cap = cap_after(part_bytes + ft + L);
                pc_cap = cap_after(sh + ch + ft + L);
                uint64_t needed = 0;
                if (cur_entries > 0 && pf.size > p0_cap) {
                    needed = (pf.size - p0_cap + pc_cap - 1) / pc_cap;
                } else if (cur_entries == 0 && pf.size > pc_cap) {
                    needed = (pf.size - pc_cap - 1) / pc_cap + 1;
                }
                const uint64_t cnew = needed + 1;
                if (cnew == c) break;
                c = cnew;
            }

            uint64_t rem = pf.size;
            takes.clear();
            if (cur_entries > 0 && p0_cap > 0) {
                const uint64_t take = std::min(rem, p0_cap);
                takes.push_back(take);
                rem -= take;
                has_leading = true;
            }
            while (rem > 0) {
                const uint64_t take = std::min(rem, pc_cap);
                takes.push_back(take);
                rem -= take;
            }
        }

        size_t ti = 0;
        if (has_leading) {
            const uint64_t take = takes[0];
            Segment seg;
            seg.chunk_index = chunk_index;
            seg.offset_in_chunk = part_bytes + L;
            seg.length = take;
            pf.segments.push_back(seg);
            part_bytes += L + take;
            cur_entries++;
            cur_list.push_back(ChunkEntry{fid, take, static_cast<uint32_t>(file_done[fid]++)});
            ++ti;
            finalize_chunk();
        } else if (cur_entries > 0) {
            finalize_chunk();
        }

        for (; ti < takes.size(); ++ti) {
            part_bytes = sh + ch;
            cur_entries = 0;
            const uint64_t take = takes[ti];
            Segment seg;
            seg.chunk_index = chunk_index;
            seg.offset_in_chunk = part_bytes + L;
            seg.length = take;
            pf.segments.push_back(seg);
            part_bytes += L + take;
            cur_entries = 1;
            cur_list.push_back(ChunkEntry{fid, take, static_cast<uint32_t>(file_done[fid]++)});
            if (ti + 1 < takes.size()) finalize_chunk();
        }
    }

    if (cur_entries > 0 || r.chunks.empty()) {
        r.chunks.push_back(ChunkPlan{part_bytes + ft, std::move(cur_list)});
    }

    for (const auto& plan : r.chunks) r.total_bytes += plan.part_size;
    return r;
}

size_t encode_jobs(int requested, size_t parts, bool serialize) {
    if (serialize || parts <= 1) return 1;
    size_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    const size_t n = requested > 0 ? static_cast<size_t>(requested) : hw;
    return std::min(n, parts);
}

} // namespace

Rc encode(const Options& o) {
    const bool to_stdout = o.output == "-";
    const bool from_stdin = o.input == "-";

    std::vector<PathEntry> entries;

    if (from_stdin) {
        PathEntry e;
        e.rel_name = "stdin";
        e.abs_path = fs::path("<stdin>");
        e.size = 0;
        entries.push_back(std::move(e));
    } else {
        std::vector<std::string> roots;
        roots.push_back(o.input);
        if (!o.file_list.empty()) {
            roots = read_file_list(o.file_list);
            if (roots.empty()) {
                log_error("file list is empty or unreadable: " + o.file_list);
                return Rc::IoError;
            }
        }
        ScanOptions so;
        so.recursive = !o.no_recursion;
        so.ignore_dot = o.ignore_dot;
        so.ignore_ext.assign(o.ignore_ext.begin(), o.ignore_ext.end());
        const ScanResult sr = scan_inputs(roots, so);
        if (!sr.ok) {
            log_error(sr.error);
            return Rc::IoError;
        }
        if (sr.entries.empty()) {
            log_error("no input files matched");
            return Rc::IoError;
        }
        entries = sr.entries;
    }

    // stdin input is spooled so the entry's size is known before planning.
    FILE* spool = nullptr;
    uint64_t spool_hash = 0;
    if (from_stdin) {
        spool = std::tmpfile();
        if (spool == nullptr) {
            log_error("cannot create stdin spool file");
            return Rc::IoError;
        }
        std::vector<char> buf(kCopyBuf);
        size_t n = 0;
        uint64_t got = 0;
        Xxh3Stream h;
        while ((n = std::fread(buf.data(), 1, buf.size(), stdin)) > 0) {
            h.update(buf.data(), n);
            if (!safe_write(spool, buf.data(), n)) {
                std::fclose(spool);
                log_error("failed writing stdin spool");
                return Rc::IoError;
            }
            got += n;
        }
        entries[0].size = got;
        spool_hash = h.digest();
        std::fflush(spool);
        std::rewind(spool);
    }

    uint64_t total_payload = 0;
    for (const auto& e : entries) total_payload += e.size;

    SplitEnv env = make_split_env(o);
    if (o.num_chunks_set) {
        const uint64_t t = total_payload == 0 ? 1 : (total_payload + o.num_chunks - 1) / o.num_chunks;
        env.target = std::clamp<uint64_t>(t, 1, UINT64_MAX);
        const uint64_t dmin = o.min_sz != 0 ? o.min_sz : env.target / 4 * 3;
        const uint64_t dmax = o.max_sz != 0 ? o.max_sz : env.target / 4 * 5;
        env.min_sz = std::max<uint64_t>(dmin, 1);
        env.max_sz = std::max<uint64_t>(dmax, env.target);
        env.target = std::clamp(env.target, env.min_sz, env.max_sz);
    }
    if (env.max_sz < kMinPart) {
        log_error("--max (or derived chunk size) is too small to hold stream metadata");
        if (spool) std::fclose(spool);
        return Rc::CliError;
    }

    // Live progress bars for each encode pass. "Defining Content" counts the
    // bytes the content-detection scan actually reads (only files larger than
    // --max are scanned), next_phase() switches to "Hashing" for the pre-hash
    // pass and finally "Splitting" for the encode write pass. The size each
    // pass reports matches the bytes that pass reads. Disabled for --dry-run.
    uint64_t prepare_total = 0;
    for (const auto& e : entries)
        if (e.size > env.max_sz) prepare_total += e.size;
    const bool need_bar = g_verbose == 1 && want_progress(stderr) && !o.dry_run;
    Progress prog(stderr, prepare_total, "Defining Content", need_bar);

    BoundaryMatcher matcher;
    matcher.compile();

    // Content-defined boundaries for oversized files. Each entry gets its
    // cumulative cut offsets; empty means "keep the fixed-size slicing" (the
    // file fits a part, chunking was unusable, or the content pass failed).
    std::vector<std::vector<uint64_t>> bd(entries.size());
    {
        std::vector<uint64_t> deltas;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].size <= env.max_sz) continue;
            FILE* f = spool;
            bool owned = false;
            if (!from_stdin) {
                f = std::fopen(entries[i].abs_path.string().c_str(), "rb");
                if (f == nullptr) {
                    log_error("cannot open input: " + entries[i].rel_name);
                    if (spool) std::fclose(spool);
                    return Rc::IoError;
                }
                owned = true;
            }
            deltas.clear();
            if (content_boundaries(f, entries[i].size, env, matcher, deltas, &prog) &&
                !deltas.empty()) {
                bd[i].reserve(deltas.size());
                uint64_t acc = 0;
                for (const uint64_t d : deltas) {
                    acc += d;
                    bd[i].push_back(acc);
                }
            }
            if (owned) std::fclose(f);
        }
    }

    SimResult plan = simulate(entries, env, bd);

    if (o.dry_run) {
        std::fprintf(stdout, "input: %zu file(s), %s payload\n", plan.files.size(),
                     human_size(plan.total_payload).c_str());
        std::fprintf(stdout, "target: %s, min: %s, max: %s\n", human_size(env.target).c_str(),
                     human_size(env.min_sz).c_str(), human_size(env.max_sz).c_str());
        std::fprintf(stdout, "parts: %zu, total stream: %s\n", plan.chunks.size(),
                     human_size(plan.total_bytes).c_str());
        for (size_t i = 0; i < plan.chunks.size(); ++i) {
            const size_t n = plan.chunks[i].entries.size();
            std::fprintf(stdout, "  part %03zu: %s, %zu entr%s\n", i + 1,
                         human_size(plan.chunks[i].part_size).c_str(), n, n == 1 ? "y" : "ies");
        }
        std::fprintf(stdout, "would write %zu part file(s)%s\n", plan.chunks.size(),
                     to_stdout ? " to stdout" : "");
        if (spool) std::fclose(spool);
        return Rc::Ok;
    }

    if (from_stdin) {
        plan.files[0].size = entries[0].size;
        plan.files[0].payload_xxh3 = spool_hash;
    } else {
        // Pre-hash every payload so FileEntry metadata (written before the
        // bytes) carries the true checksum even on non-seekable output.
        prog.next_phase(plan.total_payload, "Hashing");
        logv(2, "hashing payloads...");
        std::vector<char> buf(kCopyBuf);
        for (auto& pf : plan.files) {
            FILE* in = std::fopen(pf.abs_path.string().c_str(), "rb");
            if (in == nullptr) {
                log_error("cannot open input: " + pf.name);
                if (spool) std::fclose(spool);
                return Rc::IoError;
            }
            Xxh3Stream h;
            size_t n = 0;
            while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
                h.update(buf.data(), n);
                prog.add(n);
            }
            const bool ok = std::ferror(in) == 0;
            std::fclose(in);
            if (!ok) {
                log_error("read error while hashing: " + pf.name);
                if (spool) std::fclose(spool);
                return Rc::IoError;
            }
            pf.payload_xxh3 = h.digest();
        }
    }

    const std::string base = to_stdout ? std::string() : o.output;
    if (!to_stdout) {
        if (!o.force) {
            for (size_t i = 0; i < plan.chunks.size(); ++i) {
                const std::string part = make_part_name(base, i, plan.chunks.size());
                std::error_code ec;
                if (fs::exists(part, ec) && !ec) {
                    log_error("output part already exists (use -f to overwrite): " + part);
                    if (spool) std::fclose(spool);
                    return Rc::IoError;
                }
            }
        }
    }

    const size_t njobs = encode_jobs(o.jobs, plan.chunks.size(), from_stdin || to_stdout);

    // Content detection and hashing are done: switch to the encode write pass.
    prog.next_phase(plan.total_payload, "Splitting");

    // Per-file cumulative source offsets of each segment, indexed by the
    // per-file segment ordinal stamped onto every ChunkEntry during planning.
    // This lets the parallel part writers read at deterministic offsets with
    // no shared mutable cursor between them.
    std::vector<std::vector<uint64_t>> seg_starts(plan.files.size());
    for (uint32_t fid = 0; fid < plan.files.size(); ++fid) {
        uint64_t o = 0;
        seg_starts[fid].reserve(plan.files[fid].segments.size());
        for (const auto& s : plan.files[fid].segments) {
            seg_starts[fid].push_back(o);
            o += s.length;
        }
    }

    StreamHeader sh_hdr;
    sh_hdr.total_chunks = static_cast<uint32_t>(plan.chunks.size());
    sh_hdr.total_stream_size = plan.total_bytes;
    sh_hdr.chunk_target = env.target;
    sh_hdr.num_chunks_requested = o.num_chunks_set ? static_cast<uint32_t>(o.num_chunks) : 0;
    sh_hdr.file_count = static_cast<uint64_t>(plan.files.size());

    std::atomic<size_t> next_part{0};
    std::atomic<bool> stop{false};
    std::vector<std::string> created;
    std::mutex created_mu;
    bool failed = false;
    Rc fail_rc = Rc::Ok;

    // Writes one complete part to its own file (or stdout, single job only)
    // and reports the part's filename so the caller can clean it up on error.
    const auto write_part = [&](size_t ci, std::vector<char>& buf,
                                std::string& created_name) -> Rc {
        const ChunkPlan& cp = plan.chunks[ci];
        FILE* f = to_stdout ? stdout : nullptr;
        if (!to_stdout) {
            const std::string part = make_part_name(base, ci, plan.chunks.size());
            f = std::fopen(part.c_str(), "wb");
            if (f == nullptr) {
                log_error("cannot create output part: " + part);
                return Rc::IoError;
            }
            created_name = part;
        }

        std::vector<uint8_t> ser;
        encode_stream_header(sh_hdr, ser);
        if (!safe_write(f, ser.data(), ser.size())) {
            log_error("write failed on stream header");
            if (!to_stdout) std::fclose(f);
            return Rc::IoError;
        }

        Xxh3Stream chunk_hash;  // covers everything after the StreamHeader

        ChunkHeader ch_hdr;
        ch_hdr.chunk_index = static_cast<uint32_t>(ci);
        ch_hdr.chunk_size = cp.part_size;
        ch_hdr.entry_count = static_cast<uint32_t>(cp.entries.size());
        encode_chunk_header(ch_hdr, ser);
        if (!safe_write(f, ser.data(), ser.size())) {
            log_error("write failed on chunk header");
            if (!to_stdout) std::fclose(f);
            return Rc::IoError;
        }
        chunk_hash.update(ser.data(), ser.size());

        for (const auto& ce : cp.entries) {
            const PendingFile& pf = plan.files[ce.file_id];
            FileEntry fe;
            fe.name = pf.name;
            fe.size = pf.size;
            fe.payload_xxh3 = pf.payload_xxh3;
            fe.segments = pf.segments;
            encode_file_entry(fe, ser);
            if (!safe_write(f, ser.data(), ser.size())) {
                log_error("write failed on file entry");
                if (!to_stdout) std::fclose(f);
                return Rc::IoError;
            }
            chunk_hash.update(ser.data(), ser.size());
        }

        for (const auto& ce : cp.entries) {
            const uint32_t fid = ce.file_id;
            const uint64_t nbytes = ce.payload_len;
            const uint64_t off = seg_starts[fid][ce.seg_index];

            if (from_stdin) {
                if (!file_seek(spool, static_cast<int64_t>(off), SEEK_SET)) {
                    log_error("spool seek failed");
                    if (!to_stdout) std::fclose(f);
                    return Rc::IoError;
                }
                uint64_t left = nbytes;
                while (left > 0) {
                    const size_t step = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
                    const size_t n = std::fread(buf.data(), 1, step, spool);
                    if (n == 0) {
                        log_error("spool read failed");
                        if (!to_stdout) std::fclose(f);
                        return Rc::IoError;
                    }
                    chunk_hash.update(buf.data(), n);
                    if (!safe_write(f, buf.data(), n)) {
                        log_error("write failed on payload");
                        if (!to_stdout) std::fclose(f);
                        return Rc::IoError;
                    }
                    prog.add(n);
                    left -= n;
                }
            } else {
                FILE* in = std::fopen(plan.files[fid].abs_path.string().c_str(), "rb");
                if (in == nullptr) {
                    log_error("cannot open input: " + plan.files[fid].name);
                    if (!to_stdout) std::fclose(f);
                    return Rc::IoError;
                }
                if (!file_seek(in, static_cast<int64_t>(off), SEEK_SET)) {
                    std::fclose(in);
                    log_error("input seek failed");
                    if (!to_stdout) std::fclose(f);
                    return Rc::IoError;
                }
                uint64_t left = nbytes;
                while (left > 0) {
                    const size_t step = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
                    const size_t n = std::fread(buf.data(), 1, step, in);
                    if (n == 0) {
                        std::fclose(in);
                        log_error("input read failed: " + plan.files[fid].name);
                        if (!to_stdout) std::fclose(f);
                        return Rc::IoError;
                    }
                    chunk_hash.update(buf.data(), n);
                    if (!safe_write(f, buf.data(), n)) {
                        std::fclose(in);
                        log_error("write failed on payload");
                        if (!to_stdout) std::fclose(f);
                        return Rc::IoError;
                    }
                    prog.add(n);
                    left -= n;
                }
                std::fclose(in);
            }
        }

        ChunkFooter ft;
        ft.stream_xxh3 = chunk_hash.digest();
        ft.chunk_size = cp.part_size;
        encode_chunk_footer(ft, ser);
        if (!safe_write(f, ser.data(), ser.size())) {
            log_error("write failed on chunk footer");
            if (!to_stdout) std::fclose(f);
            return Rc::IoError;
        }

        if (!to_stdout) std::fclose(f);
        return Rc::Ok;
    };

    const size_t total = plan.chunks.size();
    std::vector<std::thread> pool;
    pool.reserve(njobs);
    for (size_t w = 0; w < njobs; ++w) {
        pool.emplace_back([&]() {
            std::vector<char> buf(kCopyBuf);
            for (;;) {
                const size_t ci = next_part.fetch_add(1);
                if (ci >= total || stop.load(std::memory_order_relaxed)) return;
                std::string created_name;
                const Rc rc = write_part(ci, buf, created_name);
                std::lock_guard<std::mutex> g(created_mu);
                if (!to_stdout && !created_name.empty()) created.push_back(created_name);
                if (rc != Rc::Ok) {
                    if (!failed) {
                        failed = true;
                        fail_rc = rc;
                    }
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& th : pool) th.join();

    if (spool) std::fclose(spool);

    if (failed) {
        if (!to_stdout && !o.keep) {
            for (const auto& p : created) std::remove(p.c_str());
        }
        return fail_rc;
    }

    prog.finish();

    logv(1, std::to_string(plan.chunks.size()) + " part(s), " +
                 human_size(plan.total_payload) + " payload");
    return Rc::Ok;
}

} // namespace sss