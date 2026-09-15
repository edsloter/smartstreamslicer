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
#include "decoder.h"

#include "cli.h"
#include "format.h"
#include "fsutil.h"
#include "hash.h"
#include "progress.h"
#include "sss_io.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <vector>

namespace sss {
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kCopyBuf = 1 << 20;
constexpr size_t kMaxOpenOutputs = 400;

struct OutFile {
    std::string path;
    FILE* f = nullptr;
    std::unique_ptr<Xxh3Stream> verifier;
    uint64_t size = 0;
    uint64_t bytes = 0;
    uint64_t expected_hash = 0;
};

bool valid_magic(const std::array<uint8_t, 4>& m) { return m == kMagic; }

bool safe_rel_path(const std::string& name) {
    if (name.empty()) return false;
    if (name.front() == '/' || name.front() == '\\') return false;
    fs::path p(name);
    for (const auto& comp : p) {
        if (comp == "..") return false;
    }
    return true;
}

} // namespace

Rc decode(const Options& o) {
    const bool from_stdin = o.input == "-";
    const bool to_stdout = o.output == "-";

    std::string base;
    std::vector<std::string> part_paths;
    uint32_t total_chunks = 0;
    uint64_t file_count = 0;
    uint32_t stream_version = 0;
    bool newer_stream = false;

    if (!from_stdin) {
        std::string b;
        if (looks_like_part_name(o.input, b)) {
            base = b;
        } else {
            base = o.input;
        }
        const fs::path bp(base);
        const std::string stem = bp.filename().string();
        std::vector<std::pair<uint64_t, fs::path>> found;
        std::error_code ec;
        fs::path parent = bp.parent_path();
        if (parent.empty()) parent = ".";
        for (fs::directory_iterator it(parent, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const std::string fn = it->path().filename().string();
            const std::string prefix = stem + ".sss";
            if (fn.size() <= prefix.size() || fn.compare(0, prefix.size(), prefix) != 0) continue;
            const std::string digits = fn.substr(prefix.size());
            if (digits.find_first_not_of("0123456789") != std::string::npos) continue;
            uint64_t num = 0;
            bool ok = true;
            for (char c : digits) {
                num = num * 10 + static_cast<uint64_t>(c - '0');
                if (num > UINT32_MAX) {
                    ok = false;
                    break;
                }
            }
            if (!ok || num == 0) continue;
            found.emplace_back(num, it->path());
        }
        if (found.empty()) {
            log_error("no .sss part files found for base: " + base);
            return Rc::IoError;
        }
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& p : found) part_paths.push_back(p.second.string());
    }

    FILE* in = from_stdin ? stdin : std::fopen(part_paths[0].c_str(), "rb");
    if (!from_stdin && in == nullptr) {
        log_error("cannot open first part: " + part_paths[0]);
        return Rc::IoError;
    }

    std::map<std::string, OutFile> outputs;
    std::vector<std::string> created;
    std::vector<char> copybuf(kCopyBuf);
    std::vector<uint8_t> raw;
    Rc rc = Rc::Ok;
    std::unique_ptr<Progress> prog;

    std::list<std::string> lru;
    std::map<std::string, std::list<std::string>::iterator> lru_pos;
    size_t open_outputs = 0;

    const auto touch = [&](const std::string& name) {
        auto p = lru_pos.find(name);
        if (p != lru_pos.end()) {
            lru.splice(lru.end(), lru, p->second);
        } else {
            lru.push_back(name);
            lru_pos.emplace(name, std::prev(lru.end()));
        }
    };

    const auto evict_lru = [&]() {
        while (!lru.empty()) {
            const std::string name = lru.front();
            lru.pop_front();
            lru_pos.erase(name);
            auto p = outputs.find(name);
            if (p != outputs.end() && p->second.f != nullptr) {
                std::fclose(p->second.f);
                p->second.f = nullptr;
                --open_outputs;
            }
            break;
        }
    };

    const auto make_room = [&]() {
        while (open_outputs >= kMaxOpenOutputs) evict_lru();
    };

    const auto ensure_open = [&](OutFile& of, const std::string& name) -> bool {
        if (of.f != nullptr) {
            touch(name);
            return true;
        }
        make_room();
        FILE* f = std::fopen(of.path.c_str(), "ab");
        if (f == nullptr) {
            log_error("cannot reopen output: " + of.path);
            return false;
        }
        of.f = f;
        ++open_outputs;
        touch(name);
        return true;
    };

    const auto prog_add = [&](size_t n) {
        if (prog) prog->add(n);
    };

    uint64_t stdout_bytes = 0;
    uint64_t stdout_expected = 0;
    uint64_t stdout_expected_hash = 0;
    std::unique_ptr<Xxh3Stream> stdout_verifier;
    if (to_stdout && o.verify) stdout_verifier = std::make_unique<Xxh3Stream>();

    for (uint32_t part = 0; part < total_chunks || part == 0; ++part) {
        if (!from_stdin && part > 0) {
            const std::string pn = make_part_name(base, part, total_chunks);
            in = std::fopen(pn.c_str(), "rb");
            if (in == nullptr) {
                log_error("missing part file: " + pn);
                rc = Rc::IoError;
                break;
            }
        }

        raw.resize(kStreamHeaderLen);
        if (!safe_read(in, raw.data(), kStreamHeaderLen)) {
            log_error("unexpected end of stream in part " + std::to_string(part + 1));
            rc = Rc::IoError;
            if (!from_stdin && part > 0) std::fclose(in);
            break;
        }
        prog_add(raw.size());
        StreamHeader ph;
        if (!decode_stream_header(std::string_view(
                reinterpret_cast<const char*>(raw.data()), raw.size()), ph)) {
            log_error("bad stream header in part " + std::to_string(part));
            rc = Rc::FormatError;
            break;
        }
        if (!valid_magic(ph.magic)) {
            log_error("bad magic in part " + std::to_string(part));
            rc = Rc::FormatError;
            break;
        }
        if (!ver::valid(ph.format_version)) {
            log_error("unrecognized version marker in stream header in part " +
                      std::to_string(part + 1) + ": " +
                      std::to_string(ph.format_version));
            rc = Rc::FormatError;
            break;
        }
        if (ph.format_version > ver::current_packed()) {
            newer_stream = true;
            log_warn("stream created by " + ver::to_string(ph.format_version) +
                     " (newer than this build " +
                     ver::to_string(ver::current_packed()) +
                     "); a newer version may be available at " + kDownloadUrl);
        }
        if (part == 0) {
            stream_version = ph.format_version;
            total_chunks = ph.total_chunks;
            file_count = ph.file_count;
            if (!from_stdin && !to_stdout) {
                prog = std::make_unique<Progress>(stderr, ph.total_stream_size, "Restoring",
                                                  g_verbose == 1 && want_progress(stderr));
            }
            if (to_stdout && file_count != 1) {
                log_error("cannot decode to stdout: stream contains " + std::to_string(file_count) +
                          " files (need exactly 1)");
                rc = Rc::FormatError;
                break;
            }
            if (total_chunks == 0) {
                log_error("stream declares zero chunks");
                rc = Rc::FormatError;
                break;
            }
        } else if (ph.total_chunks != total_chunks || ph.file_count != file_count ||
                   ph.format_version != stream_version) {
            log_error("inconsistent stream headers between parts");
            rc = Rc::FormatError;
            break;
        }

        Xxh3Stream chunk_hash;
        raw.resize(kChunkHeaderLen);
        if (!safe_read(in, raw.data(), kChunkHeaderLen)) {
            log_error("truncated chunk header in part " + std::to_string(part));
            rc = Rc::IoError;
            break;
        }
        prog_add(raw.size());
        chunk_hash.update(raw.data(), raw.size());
        ChunkHeader ch;
        if (!decode_chunk_header(std::string_view(
                reinterpret_cast<const char*>(raw.data()), raw.size()), ch)) {
            log_error("bad chunk header in part " + std::to_string(part));
            rc = Rc::FormatError;
            break;
        }
        if (ch.chunk_index != part) {
            log_error("chunk index mismatch: expected " + std::to_string(part) + " got " +
                      std::to_string(ch.chunk_index));
            rc = Rc::FormatError;
            break;
        }

        struct Slot {
            std::string name;
            uint64_t size = 0;
            uint64_t payload_xxh3 = 0;
            uint64_t here = 0;
        };
        std::vector<Slot> slots;
        slots.reserve(ch.entry_count);

        for (uint32_t e = 0; e < ch.entry_count; ++e) {
            uint8_t lenb[2];
            if (!safe_read(in, lenb, 2)) {
                log_error("truncated entry in part " + std::to_string(part));
                rc = Rc::IoError;
                goto cleanup_part;
            }
            prog_add(2);
            chunk_hash.update(lenb, 2);
            const uint16_t namelen = static_cast<uint16_t>(lenb[0]) |
                                     static_cast<uint16_t>(lenb[1] << 8);
            std::string name;
            name.resize(namelen);
            if (namelen > 0) {
                if (!safe_read(in, name.data(), namelen)) {
                    log_error("truncated entry name in part " + std::to_string(part));
                    rc = Rc::IoError;
                    goto cleanup_part;
                }
                prog_add(name.size());
                chunk_hash.update(name.data(), name.size());
            }
            uint8_t fixed[20];
            if (!safe_read(in, fixed, 20)) {
                log_error("truncated entry in part " + std::to_string(part));
                rc = Rc::IoError;
                goto cleanup_part;
            }
            prog_add(20);
            chunk_hash.update(fixed, 20);
            uint64_t size = 0, phash = 0;
            for (int i = 0; i < 8; ++i)
                size |= static_cast<uint64_t>(fixed[i]) << (8 * i);
            for (int i = 0; i < 8; ++i)
                phash |= static_cast<uint64_t>(fixed[8 + i]) << (8 * i);
            const uint32_t segc = static_cast<uint32_t>(fixed[16]) |
                                  static_cast<uint32_t>(fixed[17] << 8) |
                                  static_cast<uint32_t>(fixed[18] << 16) |
                                  static_cast<uint32_t>(fixed[19] << 24);
            std::vector<uint8_t> segraw(static_cast<size_t>(segc) * 20);
            if (!segraw.empty() && !safe_read(in, segraw.data(), segraw.size())) {
                log_error("truncated segment map in part " + std::to_string(part));
                rc = Rc::IoError;
                goto cleanup_part;
            }
            prog_add(segraw.size());
            chunk_hash.update(segraw.data(), segraw.size());

            Slot sl;
            sl.name = std::move(name);
            sl.size = size;
            sl.payload_xxh3 = phash;
            uint64_t sum = 0;
            for (uint32_t s = 0; s < segc; ++s) {
                const size_t off = static_cast<size_t>(s) * 20;
                uint64_t cidx = 0, oin = 0, ln = 0;
                for (int i = 0; i < 4; ++i)
                    cidx |= static_cast<uint64_t>(segraw[off + i]) << (8 * i);
                for (int i = 0; i < 8; ++i)
                    oin |= static_cast<uint64_t>(segraw[off + 4 + i]) << (8 * i);
                for (int i = 0; i < 8; ++i)
                    ln |= static_cast<uint64_t>(segraw[off + 12 + i]) << (8 * i);
                sum += ln;
                if (cidx == part) sl.here += ln;
                (void)oin;
            }
            if (sum != size) {
                log_error("segment map size mismatch for " + sl.name);
                rc = Rc::FormatError;
                goto cleanup_part;
            }
            slots.push_back(std::move(sl));
        }

        for (const Slot& sl : slots) {
            if (to_stdout) {
                if (stdout_expected == 0) {
                    stdout_expected = sl.size;
                    stdout_expected_hash = sl.payload_xxh3;
                } else if (stdout_expected != sl.size) {
                    log_error("conflicting sizes for stdout stream");
                    rc = Rc::DataError;
                    goto cleanup_part;
                }
                std::vector<char>& buf = copybuf;
                uint64_t left = sl.here;
                while (left > 0) {
                    const size_t step = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
                    const size_t n = std::fread(buf.data(), 1, step, in);
                    if (n == 0) {
                        log_error("truncated payload in part " + std::to_string(part));
                        rc = Rc::IoError;
                        goto cleanup_part;
                    }
                    chunk_hash.update(buf.data(), n);
                    if (stdout_verifier) stdout_verifier->update(buf.data(), n);
                    if (!safe_write(stdout, buf.data(), n)) {
                        log_error("write failed to stdout");
                        rc = Rc::IoError;
                        goto cleanup_part;
                    }
                    prog_add(n);
                    stdout_bytes += n;
                    left -= n;
                }
                continue;
            }

            auto it = outputs.find(sl.name);
            if (it == outputs.end()) {
                if (!safe_rel_path(sl.name)) {
                    log_error("unsafe path in stream: " + sl.name);
                    rc = Rc::FormatError;
                    goto cleanup_part;
                }
                const fs::path dst_path = fs::path(o.output) / sl.name;
                std::error_code ec;
                if (!dst_path.parent_path().empty() &&
                    !fs::exists(dst_path.parent_path(), ec) &&
                    !fs::create_directories(dst_path.parent_path(), ec)) {
                    log_error("cannot create output directory: " +
                              dst_path.parent_path().string());
                    rc = Rc::IoError;
                    goto cleanup_part;
                }
                if (ec) {
                    log_error("cannot prepare output path: " + dst_path.string());
                    rc = Rc::IoError;
                    goto cleanup_part;
                }
                if (!o.force && fs::exists(dst_path, ec) && !ec) {
                    log_error("output exists (use -f to overwrite): " + dst_path.string());
                    rc = Rc::IoError;
                    goto cleanup_part;
                }
                make_room();
                FILE* f = std::fopen(dst_path.string().c_str(), "wb");
                if (f == nullptr) {
                    log_error("cannot create output: " + dst_path.string());
                    rc = Rc::IoError;
                    goto cleanup_part;
                }
                OutFile nf;
                nf.path = dst_path.string();
                nf.f = f;
                nf.size = sl.size;
                nf.expected_hash = sl.payload_xxh3;
                if (o.verify) nf.verifier = std::make_unique<Xxh3Stream>();
                created.push_back(dst_path.string());
                it = outputs.emplace(sl.name, std::move(nf)).first;
                ++open_outputs;
                touch(sl.name);
            }
            OutFile& of = it->second;
            if (of.f == nullptr && !ensure_open(of, sl.name)) {
                rc = Rc::IoError;
                goto cleanup_part;
            }
            touch(sl.name);
            if (of.size != sl.size) {
                log_error("conflicting sizes for " + sl.name);
                rc = Rc::DataError;
                goto cleanup_part;
            }
            uint64_t left = sl.here;
            while (left > 0) {
                const size_t step = static_cast<size_t>(std::min<uint64_t>(left, copybuf.size()));
                const size_t n = std::fread(copybuf.data(), 1, step, in);
                if (n == 0) {
                    log_error("truncated payload in part " + std::to_string(part));
                    rc = Rc::IoError;
                    goto cleanup_part;
                }
                chunk_hash.update(copybuf.data(), n);
                if (of.verifier) of.verifier->update(copybuf.data(), n);
                if (!safe_write(of.f, copybuf.data(), n)) {
                    log_error("write failed for " + sl.name);
                    rc = Rc::IoError;
                    goto cleanup_part;
                }
                prog_add(n);
                of.bytes += n;
                left -= n;
            }
        }

        {
            raw.resize(kChunkFooterLen);
            if (!safe_read(in, raw.data(), kChunkFooterLen)) {
                log_error("truncated footer in part " + std::to_string(part));
                rc = Rc::IoError;
                goto cleanup_part;
            }
            prog_add(raw.size());
            ChunkFooter ft;
            if (!decode_chunk_footer(std::string_view(
                    reinterpret_cast<const char*>(raw.data()), raw.size()), ft)) {
                log_error("bad footer in part " + std::to_string(part));
                rc = Rc::FormatError;
                goto cleanup_part;
            }
            if (!valid_magic(ft.magic) || ft.chunk_size != ch.chunk_size) {
                log_error("bad footer in part " + std::to_string(part));
                rc = Rc::DataError;
                goto cleanup_part;
            }
            if (chunk_hash.digest() != ft.stream_xxh3) {
                log_error("chunk checksum mismatch in part " + std::to_string(part));
                rc = Rc::DataError;
                goto cleanup_part;
            }
        }

        if (!from_stdin) {
            std::error_code ec;
            uintmax_t fsz = fs::file_size(fs::path(part_paths[part]), ec);
            if (!ec && fsz != ch.chunk_size) {
                log_error("part file size mismatch in part " + std::to_string(part));
                rc = Rc::DataError;
                goto cleanup_part;
            }
            std::fclose(in);
            in = nullptr;
        }

        if (part + 1 >= total_chunks) break;

    cleanup_part:
        if (rc != Rc::Ok) {
            if (!from_stdin && in != nullptr) {
                std::fclose(in);
                in = nullptr;
            }
            break;
        }
    }

    if (in != nullptr) {
        if (!from_stdin) {
            std::fclose(in);
        }
        in = nullptr;
    }

    if (prog) prog->finish();

    if (rc == Rc::Ok) {
        if (to_stdout) {
            if (stdout_bytes != stdout_expected) {
                log_error("stdout output size mismatch (" + std::to_string(stdout_bytes) +
                          " of " + std::to_string(stdout_expected) + " bytes)");
                rc = Rc::DataError;
            }
            if (rc == Rc::Ok && stdout_verifier &&
                stdout_verifier->digest() != stdout_expected_hash) {
                log_error("payload checksum mismatch on stdout");
                rc = Rc::DataError;
            }
        }
        for (auto& kv : outputs) {
            OutFile& of = kv.second;
            if (of.f != nullptr) {
                std::fclose(of.f);
                of.f = nullptr;
            }
            if (of.bytes != of.size) {
                log_error("output size mismatch for " + kv.first + " (" +
                          std::to_string(of.bytes) + " of " + std::to_string(of.size) + " bytes)");
                rc = Rc::DataError;
            }
            if (o.verify && of.verifier && of.verifier->digest() != of.expected_hash) {
                log_error("payload checksum mismatch for " + kv.first);
                rc = Rc::DataError;
            }
        }
    }

    if (rc != Rc::Ok) {
        if (newer_stream) {
            log_error("this stream was created by a newer version of sss (" +
                      ver::to_string(stream_version) + "); this build is " +
                      ver::to_string(ver::current_packed()) +
                      " and may not be able to decode it. A newer version may "
                      "be available at " + kDownloadUrl);
        }
        for (auto& kv : outputs) {
            if (kv.second.f != nullptr) {
                std::fclose(kv.second.f);
                kv.second.f = nullptr;
            }
        }
        if (!to_stdout && !o.keep) {
            for (const auto& p : created) std::remove(p.c_str());
        }
    }

    if (rc == Rc::Ok) {
        logv(1, "decoded " + std::to_string(outputs.size()) + " file(s) from " +
                     std::to_string(total_chunks) + " part(s)");
    }
    return rc;
}

} // namespace sss