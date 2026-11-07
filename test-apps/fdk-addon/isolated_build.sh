#!/bin/sh
# isolated_build.sh — the add-on isolation proof, as a script.
#
# Builds libfdk-hud.so against an INSTALLED FDK and nothing else:
# the only FDK include path on the compile line is the installed
# prefix's, so any reach into FDK's private headers (src/, the
# in-tree include/, test-apps/common/) is a compile error, not a
# review finding. Then it audits the dynamic boundary: every
# undefined fdk_* symbol the add-on imports must be EXPORTED by
# libfdk.so.1 — the same audit fdk-layer-shell's isolation.sh runs,
# which is what guarantees the add-on binds to the ONE libfdk the
# host process already loaded (soname dedup), never a second copy.
#
# Usage: sh isolated_build.sh [stage-dir]   (default /tmp/fdk-stage)
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
STAGE=${1:-/tmp/fdk-stage}
OUT=$(dirname -- "$0")/build

echo "== install FDK into the throwaway prefix: $STAGE"
# the sandbox's X11 dev tree (where FDK's own build finds Xrandr);
# the isolation claim is about the ADD-ON's include path below
[ -f "$ROOT/scripts/env.sh" ] && . "$ROOT/scripts/env.sh"
make -C "$ROOT" shared >/dev/null
rm -rf "$STAGE"
make -C "$ROOT" install DESTDIR="$STAGE" PREFIX=/usr >/dev/null

INC="$STAGE/usr/include"
LIB="$STAGE/usr/lib"

echo "== compile the add-on against the INSTALLED headers alone"
rm -rf "$OUT"
mkdir -p "$OUT"
CC=${CC:-gcc}
# NOTE: -I$INC is the ONLY FDK include path. No -I$ROOT/include, no
# -I../common. X11 headers are the add-on's own declared dependency
# (it speaks the X11 role itself).
"$CC" -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
    -Wmissing-prototypes -Wconversion -Wsign-conversion -Wcast-qual \
    -Wpointer-arith -Wundef -Wwrite-strings -Werror \
    -D_POSIX_C_SOURCE=200809L -O2 -fPIC -shared \
    -I"$INC" \
    -o "$OUT/libfdk-hud.so" \
    "$(dirname -- "$0")/hud.c" \
    -L"$LIB" -lfdk -lX11 -lm \
    -Wl,--version-script="$(dirname -- "$0")/fdk-hud.exports.map" \
    -Wl,-rpath,"$LIB"
echo "   ok: $OUT/libfdk-hud.so"

echo "== the exported namespace (must be fdk_hud_* only)"
nm -D --defined-only "$OUT/libfdk-hud.so" | awk '{print $3}' |
    grep -v '^$' | sort -u | sed 's/^/   /'

echo "== the dynamic boundary: every imported fdk_* symbol resolves"
echo "   from libfdk.so.1"
SONAME=$(readlink "$LIB"/libfdk.so)
IMPORTS=$(nm -D --undefined-only "$OUT/libfdk-hud.so" | awk '{print $2}' |
    grep '^fdk_' | sort -u || true)
if [ -z "$IMPORTS" ]; then
    echo "   (none imported — wrong build?)"
    exit 1
fi
FAIL=0
for sym in $IMPORTS; do
    if nm -D --defined-only "$LIB/$SONAME" | awk '{print $3}' |
        grep -qx "$sym"; then
        echo "   ok: $sym"
    else
        echo "   FAIL: $sym does not come from libfdk.so.1"
        FAIL=1
    fi
done
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

echo "== ldd (the add-on's own deps: libfdk + libX11 + libc/libm)"
ldd "$OUT/libfdk-hud.so" | sed 's/^/   /'
echo "ISOLATION PROOF OK"
