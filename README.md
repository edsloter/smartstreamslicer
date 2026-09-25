# SmartStreamSlicer (sss)

A high-performance C++ CLI tool for combining single files or entire directories into a custom streaming container and splitting them into intelligently-sized chunks. Unlike classic tar or raw CDC splitting, `sss` uses pattern-aware splitting that respects file boundaries so chunk contents stay extractable and inspectable.

**Version 0.1.1** — cross-platform (Windows / POSIX), C++17.

**Status**: encode/decode pipelines are fully working (file/dir → `.sssNNN` parts and back, with XXH3 integrity and all four stdio combinations). Oversized files are sub-split across parts on content-defined boundaries (FastCDC with Vectorscan-backed run/delimiter refinement), layered onto the same segment map format without breaking compatibility.

## Features

- **Format-aware splitting**: A custom stream layout (`CSTR` magic + metadata headers) concatenates files sequentially; chunk boundaries align cleanly with file headers whenever possible.
- **Target-size chunks**: Define a soft target size (`--target 1G`, suffixes `k`/`m`/`g` accepted) with hard min/max windows — chunks hover tightly around your goal.
- **Oversized-file fallback**: Files larger than the max chunk are sub-split on content-defined boundaries: a FastCDC scan (using `--min/--target/--max`) finds cut points, and a Vectorscan-backed refinement refuses cuts that split runs of identical bytes and snaps cuts onto the nearest record/line delimiter (NUL/LF/CR). When content chunking is unusable the encoder falls back to fixed-size slicing with the same guarantees.
- **Integrity checksums**: Every payload carries an XXH3-64 checksum, stored in its header; `--verify` checks reconstructed files against originals.
- **Chunk offset map**: Each file header embeds a minimal per-file chunk map (chunk index + offset + length), enabling direct single-file extraction with no manifest.
- **stdin/stdout streaming**: All four combinations are supported for both encode (`-e`) and decode (`-d`): file/file, file/stdout, stdin/file, stdin/stdout.
- **Multithreading**: Split files are produced in parallel (`-j N`, `0` = system max).
- **Deterministic output**: Entries are always sorted by path, so identical inputs always yield identical streams.
- **Bounded scan memory**: `--max-mem SZ` caps the RAM used by the content-defined boundary scan (k/m/g/t suffixes). It only sizes the read/feed buffer handed to the chunker — it never changes chunking parameters — so output stays byte-identical with or without it.
- **File lists**: Pass a `.txt` manifest with `--file-list=` for batch operations, with optional filters via `--ignore` and `--ignore-dot`.
- **Smart chunk counts**: `-n N` splits into `N` chunks by size, instead of using a target size.
- **Versioned format**: Format versioning baked into headers so old encodings stay decodable across versions.
- **Dry-run & inspect safety**: `--dry-run` previews the plan without writing; `-k/--keep` preserves partial parts when an error occurs.
- **AGPL-3.0 licensed**: Copyleft license that keeps network-served modified versions open.

## Installation

The only supported build path is CMake: a feature-full `sss` depends on FastCDC,
Vectorscan and Boost, all wired together by the CMake project. A bare `g++ src/*.cpp`
one-liner would silently produce a degraded binary and is not supported.

**Tested toolchain** (use these; they are what the binaries in a release are built with):

- **Windows**: CMake 3.18+ with the **MinGW-w64** generator — GCC **g++ 13** (MSYS2
  `mingw-w64-ucrt-x86_64-gcc`, MinGW Makefiles or Ninja). MSVC is **not** supported: the
  Vectorscan pattern engine (GCC/Clang only) will not build with it.
- **POSIX**: CMake 3.18+ with a GCC **g++** (tested on Ubuntu 24.04 / WSL2 g++ 13.3),
  Makefiles or Ninja.
- **`ragel`** — state-machine compiler required by Vectorscan. Install it so the `ragel`
  command is on `PATH` (Ubuntu: `apt install ragel`; MSYS2:
  `pacman -S mingw-w64-ucrt-x86_64-ragel`), or pass `-DRAGEL=/path/to/ragel` to CMake.
- **Boost headers** — Vectorscan needs only Boost headers. If `BOOST_ROOT` does not point at
  an existing Boost (i.e. `$BOOST_ROOT/boost/version.hpp` is present), CMake downloads the
  Boost 1.84.0 release headers from `archives.boost.io` and extracts them into the build tree
  automatically, printing a distinct status line for each step (download vs. extract).
- First configure needs network access to fetch dependencies (xxHash via FetchContent, Boost
  headers unless `BOOST_ROOT` is supplied). `ninja` is recommended for fast parallel builds.

### Clone (with submodules)

```bash
git clone --recurse-submodules --shallow-submodules https://github.com/edsloter/smartstreamslicer
```

If you already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

### Build

POSIX (Makefiles or Ninja):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Windows (MinGW-w64; MSYS2 UCRT64/MINGW64 or WSL2):

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

(`-G "MinGW Makefiles"` works too when Ninja is not installed.) The binary is `sss`
(`sss.exe` on Windows).

#### Quick start (all-in-one scripts)

`build.sh` (POSIX) and `build.ps1` (Windows) wrap the whole flow — shallow submodule
init, dependency checks, auto-installing/downloading anything missing (ragel via the
system package manager or winget, Boost 1.84.0 headers from `archives.boost.io`), then
CMake configure/build — and re-verify cached downloads by SHA-256 every run. Boost
headers download once to the OS temp dir (`%TEMP%\SmartStreamSlicer\cache` on
Windows, `${TMPDIR:-/tmp}/smartstreamslicer` on POSIX; override with `SSS_CACHE`)
and are extracted there exactly once. The OS clears these temp folders on reboot
or periodic temp cleanup, so after a fresh boot the next build simply re-fetches
once; until then repeat builds pass the extracted tree to CMake via `BOOST_ROOT`,
with no copying or re-extracting:

```bash
# POSIX (native or WSL)
./build.sh                     # shared build, binary at build_linux_shared/sss
./build.sh -static             # fully self-contained, binary at build_linux/sss
./build.sh --smoke             # also run tests/smoke.sh against the build

# Windows (PowerShell)
.\build.ps1                    # shared build, binary at build_shared\sss.exe
.\build.ps1 -Static            # fully self-contained, binary at build\sss.exe
.\build.ps1 -Smoke             # also run tests/smoke.ps1 against the build
```

Use `--ragel <path>` / `-Ragel <path>` to point at an existing ragel binary, or
`--boost-root <dir>` / `-BoostRoot <dir>` (checked for `boost/version.hpp` with
`BOOST_VERSION 108400`) to use an existing install instead of the cached one. The
scripts are optional — direct CMake usage above (with `-DBOOST_ROOT` or an empty build
dir that can download Boost) works exactly the same.

### CMake options

| Option                 | Default | Meaning |
| ---------------------- | ------- | ------- |
| `SSS_STATIC`           | `ON`    | Link bundled libraries statically into one self-contained executable (on Windows this also links the MinGW runtime in). `OFF` produces a shared build: on Windows CMake stages `hs.dll` + `hs_runtime.dll` next to the executable; on Linux `libhs.so.5` must be on the loader path (see `tests/smoke.sh` for the run commands). |
| `SSS_ENABLE_VECTORSCAN`| `ON`    | Pattern-aware boundary refinement. Turning it `OFF` yields a degraded binary without Vectorscan support — keep `ON`. |
| `RAGEL`                | `ragel` | Path to the ragel binary when it is not on `PATH`. |
| `BOOST_ROOT`           | (auto)  | Leave unset and CMake downloads/extracts Boost 1.84 headers into the build tree; or point it at an existing Boost to skip that step (the build scripts do this automatically with their cache). |

All build flavors — `SSS_STATIC` ON or OFF, Windows or POSIX — produce **format-compatible
output**: the stream format is versioned, encoding is deterministic, and any build can
decode parts produced by any other build (verified by the CI test suite).

## Usage

For encode, `-o NAME` is used verbatim as the *base* of the output name: files are always
written as `NAME.sss001`, `NAME.sss002`, ... — `.sssNNN` is appended, never substituted.
Typing the `.sss` yourself is optional (`-o out` → `out.sss001`, while `-o out.sss` →
`out.sss.sss001`).

```bash
# Encode a directory into 1GB chunks (recursive by default)
sss -e -i "folder" -o "archive"

# Encode a single file and pipe to stdout
sss -e -i "file.xz" -o -

# Decode stdin back into the original files (restored tree goes into the -o directory)
sss -d -i - -o "extracted/"

# Split by number of chunks
sss -e -i "bigfile" -o "part" -n 8

# Split on a low-RAM machine; --max-mem only tunes the scan buffer, so the
# parts are byte-identical to an unconstrained encode
sss -e -i "bigfile" -o "part" -n 16 --max-mem 512m

# Encode using a list of files/dirs, with multithreading
sss -e -i file_list.txt -o "batch" -j 4

# Deterministic encode (entries always processed in sorted path order)
sss -e -i "folder" -o "sorted"

# Preview the split plan without writing anything
sss -e --dry-run -i "folder" -o "dry"

# Encode, then verify reconstructed payloads match the originals
sss -e -i "folder" -o "v" --verify

# Quiet mode (no output; also forced automatically for stdin/stdout)
sss -e -i "folder" -o - -v 0
```

### CLI Flags

```
-h                Short help
-H, --long-help   Long help
-V, --version     Print program version
-e                Encode (split input into .sss chunks)
-d                Decode (assemble .sss chunks back into files)
-f, --force       Overwrite existing output
-k, --keep        Keep broken/incomplete split files when an error occurs
-j N --jobs=N     Threads (default 0 = system max)
--target SZ       Target chunk size, accepts k/m/g suffixes (e.g. 512m, 1g)
--min SZ          Hard minimum chunk size (default 75% of target)
--max SZ          Hard maximum chunk size (default 125% of target)
--max-mem SZ      Bound RAM for the content-scan buffer (k/m/g/t suffixes).
                  Buffer tuning only; output is byte-identical with or without
                  it. See "Determinism and --max-mem" below.
-n N --num-chunks=N  Split into N chunks instead of using a target size
-i INPUT --input=INPUT    Input file, dir, '-' for stdin
-o OUTPUT --output=OUTPUT Output base; encode appends .sssNNN (see Output Names)
--file-list=FILE  Read inputs from a newline-delimited .txt manifest
--no-recursion    Do not descend into subdirectories
--ignore=a,b,c    Skip file extensions (mp3,mp4,avi, etc.)
--ignore-dot      Skip dotfiles (.git-*, etc.)
--dry-run         Compute the split plan and report it, but write nothing
--verify          After decode, compare XXH3 payload checksums against headers
-v N --verbose=N  Verbosity: 0 quiet, 1 normal (default), 2+ per-file detail.
                  Always quiet for stdin/stdout streams. Verbosity 1 also draws a
                  progress bar (MiB/s, %, ETA) on stderr when a terminal is attached;
                  set SSS_FORCE_PROGRESS=1 to force the bar even when stderr is piped.
```

Entries are always processed in sorted path order (deterministic output); this is the default and is not configurable.

Positional compatibility: `sss -e "filein" "fileout"` is equivalent to `-i`/`-o` (up to two positionals are accepted when `-i`/`-o` are unset).

### Determinism and `--max-mem`

`--max-mem` is pure buffer tuning. It never alters the chunking parameters — `--target`, `--min`, `--max`, and `-n` are what drive chunk boundaries. It only changes how many bytes at a time the content-defined boundary scan reads and feeds to the chunker (larger feeds amortize FastCDC's internal buffering on large targets). Because content-defined boundaries depend solely on the chunker's `min/avg/max` configuration and the file contents, an encode with `--max-mem` produces **byte-identical `.sssNNN` parts** to an encode without it — on any machine, regardless of the value used. A budget smaller than a single chunk's working set cannot be honored and is reported with a warning.

### Output Names

The `-o` name is the base and is never modified: encode writes `NAME.sss001`, `NAME.sss002`, and so on. Whether the base already ends in `.sss` or not is irrelevant — `.sssNNN` is always appended — so `-o out` and `-o out.sss` both work and produce `out.sss001` vs `out.sss.sss001`. For more than 1000 parts the digits widen (`NAME.sss1001`); a minimum of two digits is always enforced.

## Stream Layout

A stream is a sequence of parts (`.sssNNN` files, or a single concatenated stream when piping). Every part begins with the replicated `StreamHeader`, then contains a `ChunkHeader`, the metadata for the files stored in that part, the raw payload bytes, and a trailing `ChunkFooter`:

- **StreamHeader** (40 bytes): magic `CSTR`, encoder SemVer version, total chunk count, total stream byte size, chunk target, requested chunk count, file count. Replicated verbatim in every part so a set stays self describing even if individual parts are lost.
- **ChunkHeader** (24 bytes): magic `CSTR`, encoder SemVer version, part index, part byte size, entry count.
- **FileEntry** per file stored in the part: name length + name, payload size, payload `XXH3` checksum, and a **file chunk offset map** — a list of `{part index, offset within part, length}` segments describing exactly where every byte of the file lives. Oversized files are sub-split across parts, and each part hosting the file repeats its entry with the complete offset map so every part is individually decodable.
- **ChunkFooter** (20 bytes): magic `CSTR`, streaming `XXH3` checksum of everything after the `StreamHeader` (chunk header + metadata + payload), and the part size — appended at the end so encode/decode can stream through non-seekable stdio without rewriting headers.

The version field carries the encoding app's SemVer (packed as `MAJOR*1e6 + MINOR*1e3 + PATCH`, e.g. `v0.1.1`). An old decoder is  assumed readable to new encode: if a decoder reads a stream whose version is larger than its own, it warns that a newer release may be available at <https://github.com/edsloter/smartstreamslicer> and still attempts the decode, repeating the warning if decoding fails.

### Streaming notes

All four combinations work for encode and decode: file/file, file/stdout, stdin/file, stdin/stdout. To keep the segment maps exact, encode spools stdin to a temporary file before processing; decode reads a stdin stream part-by-part using the part sizes stored in each header.

## License

AGPL-3.0-or-later — Copyright (C) 2026 Edward Sloter. See `LICENSE` for the full text.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.