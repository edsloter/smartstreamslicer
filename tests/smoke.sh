#!/usr/bin/env bash
# Smoke + determinism + cross-platform CI test for sss.
#
#   usage: smoke.sh <path-to-sss> [workdir]
#
# Runs (on whatever platform it is invoked on):
#   1. encode a fixed, generated dataset (deterministic: identical bytes on
#      both Windows and POSIX) -> out1
#   2. --dry-run plan written to two files and diffed (plan determinism)
#   3. decode with --verify and byte-compare every restored file (round trip)
#   4. encode again -> out2 and diff the per-part SHA-256 manifest
#      (determinism of splits across runs)
#   5. encode/decode hundreds of small files (open-handle budget)
#   6. write <workdir>/manifest.sha256 (sorted part hashes). CI compares the
#      manifests produced by different operating systems to verify that the
#      same input yields byte-identical splits on every build flavor.
#   7. flip one byte in the second part, decode --verify, expect exit code 4
#
# Exits 0 on success. Prints SMOKE_OK on success.
set -euo pipefail

S="${1:?usage: $0 <path-to-sss> [workdir]}"
W="${2:-}"
if [ -z "$W" ]; then W="$(mktemp -d)"; fi
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$(cd "$(dirname "$S")" && pwd)"
S="$BIN/$(basename "$S")"

# Shared-build support: put Vectorscan DLLs / libhs.so on the search path.
for d in "$BIN" \
         "$BIN/../thirdparty/vectorscan/bin" \
         "$BIN/../thirdparty/vectorscan/lib" \
         "$BIN/thirdparty/vectorscan/bin" \
         "$BIN/thirdparty/vectorscan/lib"; do
  if ls "$d"/hs.dll "$d"/hs_runtime.dll "$d"/libhs.so.* >/dev/null 2>&1; then
    PATH="$d:$PATH"
    export PATH
    export LD_LIBRARY_PATH="$d:${LD_LIBRARY_PATH:-}"
  fi
done

echo "smoke: binary=$S workdir=$W"

FILLER='0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!@#._-~:;'

gen_file() { # path lines
  awk -v n="$2" -v f="$FILLER" \
    'BEGIN{ for(i=0;i<n;i++) printf "SSSLIN%06d %s\n", i, f }' > "$1"
}

mkdir -p "$W/data/sub" "$W/data/upper"
# Deliberately created in a NON-sorted order, with names that would diverge
# if the encoder used filesystem enumeration order instead of sorting.
gen_file "$W/data/0numeric.txt" 20
gen_file "$W/data/Zulu.txt" 5
gen_file "$W/data/_underscore.txt" 3
gen_file "$W/data/big.bin" 72000
gen_file "$W/data/alpha1.txt" 4
gen_file "$W/data/small.txt" 1
printf 'a\nb\nc\n' > "$W/data/sub/Ced.bin"
gen_file "$W/data/sub/nested.txt" 8
gen_file "$W/data/upper/CASEFILE.TXT" 2
gen_file "$W/data/aLPha2.txt" 6
gen_file "$W/data/Alpha0.txt" 7
gen_file "$W/data/B_kiwi.bin" 9
gen_file "$W/data/b_banana.bin" 11

cd "$W"
mkdir -p out1 out2
A=( -e -f -v 0 -j 0 --target 1m --min 750k --max 1300k )

# 1 + 2: encode, and plan determinism via two --dry-run captures
"$S" "${A[@]}" data "$W/out1/part" >/dev/null
"$S" -e --dry-run -f -v 0 -j 0 --target 1m --min 750k --max 1300k data dryA >"$W/planA.txt" 2>&1
"$S" -e --dry-run -f -v 0 -j 0 --target 1m --min 750k --max 1300k data dryB >"$W/planB.txt" 2>&1
diff -q "$W/planA.txt" "$W/planB.txt" >/dev/null
echo "smoke: dry-run plan deterministic"

# 3: decode + verify, byte-compare restored tree
rm -rf "$W/rt1"
"$S" -d --verify -f -v 0 "$W/out1/part.sss001" "$W/rt1"
diff -r "$W/data" "$W/rt1"
echo "smoke: round-trip byte-equal"

# 4: encode again -> same split manifest
"$S" "${A[@]}" data "$W/out2/part" >/dev/null
( cd "$W/out1" && sha256sum part.sss* | sort ) > "$W/manifest1.txt"
( cd "$W/out2" && sha256sum part.sss* | sort ) > "$W/manifest2.txt"
diff -q "$W/manifest1.txt" "$W/manifest2.txt" >/dev/null
echo "smoke: splits deterministic across runs"

# 5: decoding many small files must not exhaust OS open-handle limits
#    (regression: decode held every output open until the end, so >~500 files
#     failed with "cannot create output"; now bounded by LRU eviction at 400)
mkdir -p "$W/many"
for i in $(seq -w 0 639); do
  dd if=/dev/zero of="$W/many/t$i.bin" bs=8192 count=1 status=none
done
"$S" -e -f -v 0 -j 0 "$W/many" "$W/manyout" >/dev/null
rm -rf "$W/rt-many"
"$S" -d -f -v 0 "$W/manyout.sss001" "$W/rt-many" >/dev/null 2>&1
if [ "$(find "$W/rt-many" -type f | wc -l)" -ne 640 ]; then
  echo "error: many-files restored count mismatch"; exit 1
fi
echo "smoke: many-files decode under handle budget"

# 6: cross-platform manifest for CI comparison (before corruption below).
# Written to ${SMOKE_MANIFEST_DIR:-workdir} so CI can upload/hash it.
EXPORT_MANIFEST="${SMOKE_MANIFEST_DIR:-$W}"
mkdir -p "$EXPORT_MANIFEST"
( cd "$W/out1" && sha256sum part.sss* | sort ) > "$EXPORT_MANIFEST/manifest.sha256"
echo "manifest=$EXPORT_MANIFEST/manifest.sha256"

# 7: corruption is detected, exit code 4
P2="$(find "$W/out1" -name 'part.sss*' | sort | sed -n '2p')"
printf '\xFF' | dd of="$P2" bs=1 seek=300 count=1 conv=notrunc status=none
rm -rf "$W/rc"
set +e
"$S" -d --verify -f -v 0 "$W/out1/part.sss001" "$W/rc" >/dev/null 2>&1
ec=$?
set -e
if [ "$ec" -ne 4 ]; then echo "error: corruption expected exit 4, got $ec"; exit 1; fi
echo "smoke: corruption -> exit 4"

echo "SMOKE_OK"