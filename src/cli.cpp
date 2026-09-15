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
#include "cli.h"

#include "sss_io.h"
#include "version.h"

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace sss {

namespace {

bool apply_size_flag(const std::string& value,
                     const char* flag,
                     uint64_t& out,
                     Mode& mode,
                     std::string& err) {
    if (!parse_size(value, out)) {
        err = std::string("invalid size for ") + flag + ": '" + value +
                  "' (expected e.g. 500, 512m, 1g)";
        mode = Mode::Error;
        return false;
    }
    if (out == 0) {
        err = std::string("size for ") + flag + " must be > 0";
        mode = Mode::Error;
        return false;
    }
    return true;
}

bool parse_uint(const std::string& value, uint64_t& out) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    try {
        out = std::stoull(value);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

} // namespace

ParseResult parse_args(int argc, char** argv) {
    ParseResult r;
    Options& o = r.opts;
    std::vector<std::string> positional;
    std::string want_mode;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-") {
            positional.push_back(arg);
            continue;
        }
        if (arg == "--") {
            for (++i; i < argc; ++i) positional.push_back(argv[i]);
            break;
        }

        std::string value;
        bool has_value = false;
        std::string name = arg;
        const size_t eq = arg.find('=');
        if (eq != std::string::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
            has_value = true;
        }

        auto take_value = [&](std::string& out_val) -> bool {
            if (has_value) {
                out_val = value;
                return true;
            }
            if (i + 1 < argc) {
                out_val = argv[++i];
                return true;
            }
            r.mode = Mode::Error;
            r.error = "missing value for " + name;
            return false;
        };

        auto set_mode = [&](const std::string& m) -> bool {
            if (!want_mode.empty() && want_mode != m) {
                r.mode = Mode::Error;
                r.error = "cannot combine -e and -d";
                return false;
            }
            want_mode = m;
            return true;
        };

        if (name == "-e" || name == "--encode") {
            if (!set_mode("e")) return r;
        } else if (name == "-d" || name == "--decode") {
            if (!set_mode("d")) return r;
        } else if (name == "-h" || name == "--help") {
            r.mode = Mode::Help;
            return r;
        } else if (name == "-H" || name == "--long-help") {
            r.mode = Mode::LongHelp;
            return r;
        } else if (name == "-V" || name == "--version") {
            r.mode = Mode::Version;
            return r;
        } else if (name == "-f" || name == "--force") {
            o.force = true;
        } else if (name == "-k" || name == "--keep") {
            o.keep = true;
        } else if (name == "--no-recursion") {
            o.no_recursion = true;
        } else if (name == "--ignore-dot") {
            o.ignore_dot = true;
        } else if (name == "--dry-run") {
            o.dry_run = true;
        } else if (name == "--verify") {
            o.verify = true;
        } else if (name == "-j" || name == "--jobs") {
            std::string v;
            if (!take_value(v)) return r;
            uint64_t n = 0;
            if (!parse_uint(v, n)) {
                r.mode = Mode::Error;
                r.error = "invalid jobs count: '" + v + "'";
                return r;
            }
            o.jobs = static_cast<int>(n);
        } else if (name == "-v" || name == "--verbose") {
            std::string v;
            if (!take_value(v)) return r;
            uint64_t n = 0;
            if (!parse_uint(v, n)) {
                r.mode = Mode::Error;
                r.error = "invalid verbose level: '" + v + "'";
                return r;
            }
            o.verbose = static_cast<int>(n);
        } else if (name == "-n" || name == "--num-chunks") {
            std::string v;
            if (!take_value(v)) return r;
            if (!parse_uint(v, o.num_chunks) || o.num_chunks == 0) {
                r.mode = Mode::Error;
                r.error = "invalid chunk count: '" + v + "'";
                return r;
            }
            o.num_chunks_set = true;
        } else if (name == "--target") {
            std::string v;
            if (!take_value(v)) return r;
            if (!apply_size_flag(v, "--target", o.target, r.mode, r.error)) return r;
        } else if (name == "--min") {
            std::string v;
            if (!take_value(v)) return r;
            if (!apply_size_flag(v, "--min", o.min_sz, r.mode, r.error)) return r;
        } else if (name == "--max") {
            std::string v;
            if (!take_value(v)) return r;
            if (!apply_size_flag(v, "--max", o.max_sz, r.mode, r.error)) return r;
        } else if (name == "-i" || name == "--input") {
            if (!take_value(o.input)) return r;
        } else if (name == "-o" || name == "--output") {
            if (!take_value(o.output)) return r;
        } else if (name == "--file-list") {
            if (!take_value(o.file_list)) return r;
        } else if (name == "--ignore") {
            std::string v;
            if (!take_value(v)) return r;
            size_t start = 0;
            while (true) {
                const size_t comma = v.find(',', start);
                const std::string part = comma == std::string::npos
                                             ? v.substr(start)
                                             : v.substr(start, comma - start);
                std::string ext = part;
                for (auto& c : ext) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
                if (!ext.empty()) o.ignore_ext.insert(ext);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (arg.size() > 1 && arg.front() == '-') {
            if (arg[1] == '-') {
                r.mode = Mode::Error;
                r.error = "unknown option: " + arg;
                return r;
            }
            bool ok = true;
            for (size_t ci = 1; ci < arg.size() && ok; ++ci) {
                const char c = arg[ci];
                switch (c) {
                    case 'f': o.force = true; break;
                    case 'k': o.keep = true; break;
                    case 'e': ok = set_mode("e"); break;
                    case 'd': ok = set_mode("d"); break;
                    case 'h': r.mode = Mode::Help; return r;
                    case 'H': r.mode = Mode::LongHelp; return r;
                    case 'V': r.mode = Mode::Version; return r;
                    default:
                        ok = false;
                        break;
                }
            }
            if (!ok) {
                r.mode = Mode::Error;
                r.error = "unknown or conflicting option: " + arg;
                return r;
            }
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() > 2) {
        r.mode = Mode::Error;
        r.error = "too many positional arguments (expected input and output at most)";
        return r;
    }
    if (o.input.empty() && positional.size() >= 1) o.input = positional[0];
    if (o.output.empty() && positional.size() >= 2) o.output = positional[1];

    if (want_mode.empty()) {
        r.mode = Mode::Error;
        r.error = "no mode given; use -e (encode) or -d (decode)";
        return r;
    }
    r.mode = want_mode == "e" ? Mode::Encode : Mode::Decode;

    if (o.input.empty()) {
        r.mode = Mode::Error;
        r.error = "no input; use -i or pass it positionally";
        return r;
    }
    if (o.output.empty()) {
        r.mode = Mode::Error;
        r.error = "no output; use -o or pass it positionally";
        return r;
    }
    if (o.num_chunks_set && o.target != 0) {
        r.mode = Mode::Error;
        r.error = "--target and --num-chunks cannot be used together";
        return r;
    }

    return r;
}

void print_banner(FILE* f) {
    const bool col = !std::getenv("NO_COLOR") &&
                     fileno(f) >= 0 && isatty(fileno(f)) != 0;
    if (col) std::fputs("\033[36m", f);
    std::fprintf(f, "SmartStreamSlicer (sss) v%s\n", SSS_VERSION_STRING);
    std::fputs("Copyright (C) 2026 Edward Sloter\n", f);
    std::fputc('\n', f);
    std::fputs("A streaming splitter/joiner that combines files and directories into a\n", f);
    std::fputs("custom container (CSTR magic, versioned headers, XXH3 payload checksums,\n", f);
    std::fputs("per-file chunk offset maps) and slices it into .sssNNN parts at\n", f);
    std::fputs("smart file-aligned boundaries.\n", f);
    std::fputc('\n', f);
    if (col) std::fputs("\033[0m", f);
    std::fflush(f);
}

void print_short_help(FILE* f) {
    std::fputs(
                  "Usage: sss -e|-d [options] -i INPUT -o OUTPUT\n"
                  "  (positional form: sss [-e|-d] [options] INPUT [OUTPUT])\n"
                  "\n"
                  "Modes:\n"
                  "  -e, --encode      Split input into .sssNNN chunks\n"
                  "  -d, --decode      Reassemble .sss chunks into files\n"
                  "\n"
                  "I/O:\n"
                  "  -i, --input PATH     File, dir, file-list, or '-' for stdin\n"
                  "  -o, --output PATH    File, dir, or '-' for stdout\n"
                  "  -f, --force          Overwrite existing output\n"
                  "  -k, --keep           Keep incomplete parts after an error\n"
                  "\n"
                  "Splitting:\n"
                  "  --target SZ          Target chunk size (k/m/g/t suffixes; default 1g)\n"
                  "  --min SZ             Hard minimum chunk size (default 75%% of target)\n"
                  "  --max SZ             Hard maximum chunk size (default 125%% of target)\n"
                  "  -n, --num-chunks N   Split into exactly N chunks\n"
                  "\n"
                  "Scheduling:\n"
                  "  -j, --jobs N         Worker threads (0 = system max)\n"
                  "  -v, --verbose N      Verbosity 0(quiet)..3; auto-quiet when streaming;\n"
                  "                      level 1 shows a progress bar on a terminal\n"
                  "  --dry-run            Print the split plan, write nothing\n"
                  "  --verify             Verify payload checksums on decode\n"
                  "\n"
                  "Other:\n"
                  "  -h                   Short help\n"
                  "  -H, --long-help      Full help\n"
                  "  -V, --version        Print version\n",
        f);
}

void print_long_help(FILE* f) {
    std::fputs(
                  "Usage: sss -e|-d [options] -i INPUT -o OUTPUT\n"
                  "\n"
                  "MODES\n"
                  "  -e, --encode\n"
                  "      Combine input file(s)/dirs into a stream and split into .sssNNN chunks.\n"
                  "      Directories are traversed recursively by default.\n"
                  "  -d, --decode\n"
                  "      Read .sssNNN chunks and reconstruct the original files with the\n"
                  "      original directory structure.\n"
                  "  -h, --help          Short help.\n"
                  "  -H, --long-help     This text.\n"
                  "  -V, --version       Print program version.\n"
                  "\n"
                  "INPUT/OUTPUT\n"
                  "  -i, --input PATH    File, directory, '-' for stdin, or a manifest of\n"
                  "                      inputs given with --file-list.\n"
                  "  -o, --output PATH   Output file, directory, or '-' for stdout. The name you\n"
                  "                      give is used verbatim as the base, and .sssNNN is always\n"
                  "                      appended (never substituted): -o N.xz -> N.xz.sss001\n"
                  "                      (digits widen past 1000 parts). Ending the name in .sss\n"
                  "                      is optional; if typed, it stays part of the base, e.g.\n"
                  "                      -o N.sss -> N.sss.sss001.\n"
                  "  -f, --force         Overwrite existing outputs. Without it, existing\n"
                  "                      outputs cause an error.\n"
                  "  -k, --keep          On a failed encode/decode, keep the incomplete split\n"
                  "                      files for inspection instead of deleting them.\n"
                  "                      (Combined with --dry-run, nothing is written either way.)\n"
                  "  --file-list=FILE    Read newline-delimited paths (files or dirs) from FILE.\n"
                  "  --no-recursion      Do not descend into subdirectories of input dirs.\n"
                  "  --ignore=exts       Comma-separated file extensions to skip (mp3,mp4,avi).\n"
                  "                      Case-insensitive; a leading dot is optional.\n"
                  "  --ignore-dot        Skip dot files and dot directories (.git-*, etc.).\n"
                  "\n"
                  "SPLITTING\n"
                  "  --target SZ         Soft target chunk size. Suffixes: k, m, g, t.\n"
                  "                      Default 1g. Chunks land at file boundaries near this\n"
                  "                      target (slightly under or over is expected).\n"
                  "  --min SZ            Hard minimum before a chunk can be finalized.\n"
                  "                      Default 75%% of target.\n"
                  "  --max SZ            Hard maximum; a single file larger than this is\n"
                  "                      sub-split. Default 125%% of target.\n"
                  "  -n, --num-chunks N  Instead of a target size, split the total size by N\n"
                  "                      and use that as the target (implies --max).\n"
                  "  --dry-run           Walk inputs, compute the plan, report chunk sizes and\n"
                  "                      counts, and exit without writing anything.\n"
                  "                      Entries are always processed in sorted (path) order so\n"
                  "                      identical inputs produce identical streams.\n"
                  "\n"
                  "WORKERS\n"
                  "  -j, --jobs N        Number of writer threads. 0 (default) uses the system\n"
                  "                      maximum. Encode and decode are fully streamable and\n"
                  "                      never load a whole chunk into RAM.\n"
                  "  -v, --verbose N     Verbosity: 0 quiet, 1 normal (default), 2 per-file,\n"
                  "                      3 debug. Always quiet when either side is a stream.\n"
                  "                      Level 1 shows a progress bar (MiB/s, %, ETA) when a\n"
                  "                      terminal is attached; set SSS_FORCE_PROGRESS=1 to show\n"
                  "                      it even when stderr is redirected.\n"
                  "  --verify            After decode, recompute XXH3 checksums and compare\n"
                  "                      against payload checksums stored in the headers.\n"
                  "\n"
                  "STREAMING\n"
                  "  Any of the four combinations work for both modes:\n"
                  "    file->file      sss -e -i in.bin -o out\n"
                  "    file->stdout    sss -e -i in.bin -o -\n"
                  "    stdin->file     sss -e -i - -o out\n"
                  "    stdin->stdout   sss -e -i - -o -\n"
                  "  Note that .sssNNN is always appended to the -o name: -o out\n"
                  "  writes out.sss001, and -o out.sss writes out.sss.sss001. Typing\n"
                  "  the .sss base yourself is never required.\n"
                  "\n"
                  "EXIT STATUS\n"
                  "  0 success; 1 CLI error; 2 I/O error; 3 format error; 4 data (checksum)\n"
                  "  mismatch; 5 not implemented yet.\n",
        f);
}

} // namespace sss
