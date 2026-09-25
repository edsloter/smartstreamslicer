# Changelog

All notable changes to SmartStreamSlicer are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.1] - 2026-09-24

### Added

- `--max-mem SZ` flag (k/m/g/t suffixes) for encode: bounds the RAM used by the
  content-defined boundary scan on low-memory machines. It only sizes the
  read/feed block handed to the FastCDC chunker; it never changes chunk sizing,
  so produced `.sssNNN` parts are **byte-identical** with or without it (and
  regardless of the value given). Set on a constrained box, e.g.
  `sss -e -i big -o part -n 16 --max-mem 512m`.
  - A budget smaller than a single chunk's working set cannot be honored: the
    block stays at the default and a warning is printed.
  - Accepting the option on decode. Decode's memory use is already small and
    flat, so it has no effect there.
- Documented usage in `README.md`, the `-h` short help, and the `-H` long help.

### Changed

- Program version bumped to `0.1.1`. The stream header's packed SemVer therefore
  changes from `v0.0.1` to `v0.1.1`; the on-disk layout is unchanged, so older
  builds still decode newer streams (they print a "newer version" notice and
  continue).

## [0.0.1]

- Initial release baseline (encode/decode, content-defined oversized-file
  sub-splitting, deterministic output, XXH3 integrity, stdin/stdout streaming).
- Fully working progress bar for both encode/decode paths. 