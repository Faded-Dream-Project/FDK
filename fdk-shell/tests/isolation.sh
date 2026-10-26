#!/usr/bin/env bash
# isolation.sh — the prefix-isolation build proof (fdk-shell's HARD
# REQUIREMENT, verbatim from the add-on contract):
#
#   "fdk-shell may include and link ONLY FDK's installed public
#    headers and library. No private includes, no reaching into
#    internals. To prove it, install FDK to a temporary prefix and
#    build fdk-shell against that prefix in the test script."
#
# What this script does, and what each step PROVES:
#
#   1. Installs FDK into build/isolation-prefix (headers + shared
#      library + pkg-config file). Nothing from FDK's build tree is
#      referenced afterwards.
#   2. Rebuilds fdk-shell from scratch with FDK_CFLAGS pointing at
#      the prefix's INCLUDE directory and FDK_LIBS at its shared
#      library — and, decisively, with -I flags that DO NOT contain
#      FDK's source tree: if any fdk-shell file reached for an FDK
#      internal header, COMPILATION WOULD FAIL (the include path
#      simply is not there).
#   3. Greps the package's sources for every shape of private reach
#      (relative FDK-tree includes, src/ paths, internal headers) —
#      none may exist.
#   4. Links the demo against the prefix's libfdk.so.1 and RUNS it
#      under a throwaway Xvfb (3 seconds, auto-quit) — the whole
#      stack works from the installed prefix alone.
#   5. Inspects the dynamic dependency: libfdk-shell.so's undefined
#      fdk_* symbols all resolve from libfdk.so.1, and ldd shows the
#      library bound to the prefix's copy.
#   6. Builds the package copied ALONE into a scratch directory —
#      no sibling FDK tree anywhere (the packaging families' pristine
#      tarball condition): the make graph itself must not reach for
#      ../build when the link inputs point at the prefix.
#
# Environment: needs the FDK build present (sibling tree — the
# script builds it if needed), pkg-config, Xvfb for the run leg.
set -u

cd "$(dirname "$0")/.." # fdk-shell/
FDK_ROOT=${FDK_ROOT:-..}
PREFIX="$(pwd)/build-isolation"
export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"
HAVE_WL_PC=0
if pkg-config --exists wayland-client 2>/dev/null; then
    HAVE_WL_PC=1
fi

echo "== 1. installing FDK into a throwaway prefix =="
rm -rf "$PREFIX"
mkdir -p "$(pwd)/build"
make -C "$FDK_ROOT" install PREFIX="$PREFIX" >/tmp/fdkshell-isolation-fdk.log 2>&1 || {
    echo "FAIL: FDK install into the prefix failed (see /tmp/fdkshell-isolation-fdk.log)"
    exit 1
}
test -f "$PREFIX/include/fdk/fdk_extend.h" || { echo "FAIL: fdk_extend.h not installed"; exit 1; }
test -f "$PREFIX/lib/libfdk.so.1" || { echo "FAIL: libfdk.so.1 not installed"; exit 1; }
echo "   prefix: $PREFIX (fdk $(sed -n 's/^Version: //p' "$PREFIX/lib/pkgconfig/fdk.pc"))"

echo "== 2. rebuilding fdk-shell against the prefix ALONE =="
# The decisive flags: the ONLY fdk include path is the prefix's;
# FDK's source tree (../src, ../include) is nowhere on the line.
make clean >/dev/null
FDK_CFLAGS="-I$PREFIX/include" \
FDK_LIBS="-L$PREFIX/lib -lfdk" \
FDK_SO_LIBS="-L$PREFIX/lib -lfdk" \
make static shared examples >/tmp/fdkshell-isolation-build.log 2>&1 || {
    echo "FAIL: fdk-shell does not compile against the installed public headers alone"
    echo "     (this is exactly the private-include bug the proof exists to catch)"
    cat /tmp/fdkshell-isolation-build.log | tail -20
    exit 1
}

echo "== 3. source audit: no private reaches in the package =="
BAD=0
# (a) no include may leave the package or the public API surface:
#     anything starting with ../ or containing FDK's src/ path.
if grep -rn --include='*.c' --include='*.h' -E '#include[[:space:]]+"(\.\./|.*FDK/src)' \
        src include examples tests 2>/dev/null | grep -v 'shell_internal.h' | grep -vE '#include[[:space:]]+"\.\./src/shell_internal.h"'; then
    # ../src/shell_internal.h is the package's OWN internal header
    # (the tests' verification channel) — allowed; anything else
    # reaching out is a violation.
    echo "   (note: only the package's own ../src/shell_internal.h is permitted)"
    BAD=1
fi
# (b) no FDK internal header names anywhere.
if grep -rn --include='*.c' --include='*.h' -E 'fdk.*(_internal\.h|/src/)' \
        src include examples tests 2>/dev/null; then
    echo "FAIL: fdk-shell references an FDK internal header"
    BAD=1
fi
[ $BAD -eq 0 ] || exit 1
echo "   clean: only <fdk/*.h> public includes + the package's own headers"

echo "== 4. linking + RUNNING the demo against the prefix =="
LD_LIBRARY_PATH="$PREFIX/lib" ldd build/examples/bar | grep -q "libfdk.so.1 => $PREFIX/lib/libfdk.so.1" || {
    echo "FAIL: the demo is not bound to the prefix's libfdk.so.1:"
    LD_LIBRARY_PATH="$PREFIX/lib" ldd build/examples/bar | grep libfdk
    exit 1
}

XVFB_NUM=$((300 + RANDOM % 200)); XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
Xvfb "$XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdkshell-isolation-xvfb.log 2>&1 &
XVFB_PID=$!
sleep 2
DISPLAY="$XVFB_DISP" LD_LIBRARY_PATH="$PREFIX/lib" \
    LSAN_OPTIONS="suppressions=$(cd "$FDK_ROOT" && pwd)/tests/data/lsan-gl.supp" \
    timeout 8 ./build/examples/bar 3
RC=$?
kill "$XVFB_PID" 2>/dev/null; wait "$XVFB_PID" 2>/dev/null
rm -f "/tmp/.X11-unix/X$XVFB_NUM"
[ $RC -eq 0 ] || { echo "FAIL: the demo did not run cleanly from the prefix (exit $RC)"; exit 1; }
echo "   demo ran 3s against the installed-prefix stack, exit 0"

echo "== 5. the dynamic boundary of libfdk-shell.so =="
# The .so carries a real NEEDED entry on libfdk.so.1 (the loader
# resolves the whole chain; the distro package tools derive the
# runtime dependency from the ELF).
if ! objdump -p build/libfdk-shell.so | grep -q "NEEDED.*libfdk.so.1"; then
    echo "FAIL: libfdk-shell.so has no NEEDED entry on libfdk.so.1"
    objdump -p build/libfdk-shell.so | grep NEEDED
    exit 1
fi
echo "   NEEDED: libfdk.so.1 (the dependency chain is in the ELF)"
# Every undefined fdk_* symbol the package imports must resolve
# against libfdk.so.1 (never another provider).
MISSING=$(LD_LIBRARY_PATH="$PREFIX/lib" \
    nm -D --undefined-only build/libfdk-shell.so | awk '{print $2}' | \
    sed -n 's/^\(fdk_[a-z0-9_]*\)@.*/\1/p; s/^\(fdk_[a-z0-9_]*\)$/\1/p' | sort -u)
UNRESOLVED=0
for sym in $MISSING; do
    if ! nm -D --defined-only "$PREFIX/lib/libfdk.so.1" | \
            awk '{print $3}' | grep -qx "$sym"; then
        echo "   UNRESOLVED by libfdk: $sym"
        UNRESOLVED=1
    fi
done
[ $UNRESOLVED -eq 0 ] || { echo "FAIL: fdk_* symbols not satisfied by libfdk.so.1"; exit 1; }
COUNT=$(printf '%s\n' "$MISSING" | grep -c '^fdk_' || true)
echo "   $COUNT fdk_* symbols imported; all resolve from libfdk.so.1"

# The package exports only its own namespace.
LEAKED=$(nm -D --defined-only build/libfdk-shell.so | awk '$2 ~ /^[TDBRW]$/ {print $3}' | \
    grep -v '^fdk_shell' | grep -v '^_fini$' | grep -v '^_init$' | \
    grep -v '__bss_start\|_edata\|_end' || true)
if [ -n "$LEAKED" ]; then
    echo "FAIL: libfdk-shell.so exports symbols outside its namespace:"
    printf '%s\n' "$LEAKED"
    exit 1
fi
echo "   exports: fdk_shell_* only"

echo "== 6. the pristine-tree build (no sibling FDK anywhere) =="
# The leg the rpm/alpine CI failure (run 40298760190) proved was
# missing: every OTHER leg runs inside the sibling layout, where
# ../build/libfdk.so physically exists even while the compile and
# link lines point at the prefix — make's graph is satisfied by a
# file the link never uses. The packaging families build from
# PRISTINE fdk-shell-only tarballs: no sibling tree at all.
SCRATCH=$(mktemp -d)
tar -C .. --exclude=fdk-shell/build --exclude=fdk-shell/build-isolation \
    -cf - fdk-shell | tar -C "$SCRATCH" -xf -
if (cd "$SCRATCH/fdk-shell" && \
    make FDK_CFLAGS="-I$PREFIX/include" \
          FDK_LIBS="-L$PREFIX/lib -lfdk" \
          FDK_SO_LIBS="-L$PREFIX/lib -lfdk" \
          static shared examples) >/tmp/fdkshell-isolation-pristine.log 2>&1
then
    if [ ! -f "$SCRATCH/fdk-shell/build/libfdk-shell.so" ] || \
       ! objdump -p "$SCRATCH/fdk-shell/build/libfdk-shell.so" \
            | grep -q "NEEDED.*libfdk.so.1"; then
        echo "FAIL: the pristine-tree library is missing or unbound"
        rm -rf "$SCRATCH"; exit 1
    fi
    echo "   the package builds green from a tree with no sibling FDK"
else
    echo "FAIL: the pristine-tree build failed (the packaging condition):"
    tail -5 /tmp/fdkshell-isolation-pristine.log
    rm -rf "$SCRATCH"; exit 1
fi
rm -rf "$SCRATCH"

# Restore the default (sibling-tree) build so later make targets
# link the static archive again.
make clean >/dev/null
make static shared examples >/tmp/fdkshell-isolation-restore.log 2>&1 || {
    echo "note: restore build failed (see /tmp/fdkshell-isolation-restore.log)"
}

echo "ISOLATION PROOF PASSED: fdk-shell builds, links, and runs against FDK's installed public surface only."
