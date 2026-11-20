#!/bin/sh
# ABI check (docs/quality-gates.md): abidiff the BUILT libfdk.so
# against the committed DWARF baseline (tools/abi/libfdk.so.1.baseline.xml,
# abidw dump built from a -g release config — struct layouts and
# bit offsets included, so a field reorder is caught, not just a
# symbol add/remove).
#
# Update procedure for an INTENDED append-only change:
#   1. make FDK_CONFIG=release CFLAGS="-g -O2 -DNDEBUG" shared
#   2. abidw --out-file tools/abi/libfdk.so.1.baseline.xml build/libfdk.so
#   3. git diff the XML: it must only ADD (functions, types). A
#      removal or layout change is an ABI break by definition
#      (docs/abi-policy.md) and needs the SONAME major moved.
# NOTE: the baseline is built WITHOUT the hardening/FORTIFY flags;
# those change codegen, not the ABI, and FORTIFY=3 needs -O2 only.
set -e
ABIDIFF="${ABIDIFF:-abidiff}"
SO="${1:-build/libfdk.so}"
BASE="$(dirname "$0")/libfdk.so.1.baseline.xml"
exec "$ABIDIFF" "$BASE" "$SO"
