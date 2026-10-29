#!/bin/sh
# fdk-shell/packaging/debian/build-deb.sh — build the fdk-shell .deb
# packages.
#
# Runs INSIDE the same Debian-family environment (and the same CI
# container run) as FDK's packaging/debian/build-deb.sh, AFTER it:
# dist/ carries FDK's built debs, which this script INSTALLS first —
# fdk-shell compiles against the INSTALLED public FDK (pkg-config,
# system headers, system libfdk.so.1), never the sibling tree. That
# is the add-on isolation contract, proven inside every packaging
# pipeline.
#
# Produces in dist/:
#   libfdk-shell0_<ver>-1_<arch>.deb  — runtime: libfdk-shell.so.0
#   fdk-shell-dev_<ver>-1_<arch>.deb  — header + pkg-config metadata
#
# The static archive is deliberately NOT packaged (FDK Proprietary
# License 4(d), same posture as FDK's own packages).

set -eu
. "$(dirname "$0")/../ci/common.sh"
fdk_require_release_posture

VERSION="$(fdkshell_version)"
# fdk-shell's own version is plain numeric (0.1.0) — legal in every
# package grammar; the deb appends the -1 debian_revision.
PKGVER="$(fdk_packver deb "$VERSION")-1"
# The FDK floor for the dependency metadata: extension API v1 first
# shipped in FDK 1.1.0-rc1 (fdkshell_min_fdk_ver spells it per
# family: 1.1.0~rc1 here).
MINVER="$(fdkshell_min_fdk_ver deb)"
FDK_VERSION_RAW="$(fdk_expected_version)"
ARCH="$(dpkg --print-architecture 2>/dev/null || \
        uname -m | sed 's/^x86_64$/amd64/; s/^aarch64$/arm64/')"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)"
[ -n "$MULTIARCH" ] || MULTIARCH="$(cc -dumpmachine 2>/dev/null || true)"
[ -n "$MULTIARCH" ] || MULTIARCH="x86_64-linux-gnu"
LIBDIR="/usr/lib/$MULTIARCH"

echo "== building fdk-shell $VERSION debs (arch $ARCH, libdir $LIBDIR) =="
echo "   FDK dependency floor: $(fdkshell_fdk_runtime_pkg deb) (>= $MINVER)"

# --- install FDK's packages (built by FDK's build-deb.sh earlier) ------
FDK_DEB_RT="dist/libfdk1_$(fdk_packver deb "$FDK_VERSION_RAW")-1_${ARCH}.deb"
FDK_DEB_DV="dist/fdk-dev_$(fdk_packver deb "$FDK_VERSION_RAW")-1_${ARCH}.deb"
[ -f "$FDK_DEB_RT" ] || { echo "BUILD FAIL: missing $FDK_DEB_RT (run FDK's build-deb.sh first)"; exit 1; }
[ -f "$FDK_DEB_DV" ] || { echo "BUILD FAIL: missing $FDK_DEB_DV (run FDK's build-deb.sh first)"; exit 1; }
dpkg -i "$FDK_DEB_RT" "$FDK_DEB_DV"
ldconfig 2>/dev/null || true

echo "== FDK installed: pkg-config fdk = $(pkg-config --modversion fdk) =="

# --- the boundary audit (isolation.sh's source proof, inline) ----------
echo "== boundary audit: no private FDK reaches in the package sources =="
if grep -rn --include='*.c' --include='*.h' -E '#include[[:space:]]+"(\.\./|.*FDK/src)' \
        fdk-shell/src fdk-shell/include 2>/dev/null | \
        grep -v 'shell_internal.h'; then
    echo "BUILD FAIL: fdk-shell source reaches outside its package"
    exit 1
fi
if grep -rn --include='*.c' --include='*.h' -E 'fdk.*(_internal\.h|/src/)' \
        fdk-shell/src fdk-shell/include 2>/dev/null; then
    echo "BUILD FAIL: fdk-shell references an FDK internal header"
    exit 1
fi
echo "   clean: only <fdk/*.h> public includes"

# --- build + test against the INSTALLED FDK ----------------------------
# FDK_CFLAGS/FDK_LIBS come from pkg-config — the sibling checkout is
# NOWHERE on the compile line (the Makefile's default -I../include is
# overridden). If any fdk-shell source reached for an FDK internal,
# compilation would fail right here.
make -C fdk-shell clean >/dev/null
make -C fdk-shell static shared \
    FDK_CFLAGS="$(pkg-config --cflags fdk)" \
    FDK_LIBS="$(pkg-config --libs fdk)" \
    FDK_SO_LIBS="$(pkg-config --libs fdk)"
if command -v Xvfb >/dev/null 2>&1; then
# The X11 suite in distro containers runs with LSan exitcode=0: every
# test must still pass, but exit-time leak REPORTS from third-party
# libraries do not fail the container run (this bookworm container:
# FDK's own suite reports 1696 bytes in 20 allocations, fdk-shell's
# suite one 128-byte object — every stack carries only stripped
# distro-library frames, zero first-party frames; not reproducible
# outside the container, absent on newer libX11). The STRICT leak gate
# (exit-on-any-leak) runs in CI's ubuntu job and on every development
# machine — the same documented posture as FDK's own build-deb.sh.
    LSAN_OPTIONS=exitcode=0 make -C fdk-shell test-x11 \
        FDK_CFLAGS="$(pkg-config --cflags fdk)" \
        FDK_LIBS="$(pkg-config --libs fdk)" \
        FDK_SO_LIBS="$(pkg-config --libs fdk)"
else
    echo "XVFB-SKIP: Xvfb not present; fdk-shell's X11 suite not run for this build (disclosed)"
fi

# --- stage the install --------------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
make -C fdk-shell CONFIG=release install DESTDIR="$STAGE" \
    PREFIX=/usr LIBDIR="$LIBDIR" \
    FDK_CFLAGS="$(pkg-config --cflags fdk)" \
    FDK_LIBS="$(pkg-config --libs fdk)" \
    FDK_SO_LIBS="$(pkg-config --libs fdk)"
# The static archive is NOT shipped.
rm -f "$STAGE$LIBDIR/libfdk-shell.a"

# --- derive the runtime Depends from the .so's NEEDED + the FDK floor --
SO_REAL="libfdk-shell.so.$(echo "$VERSION" | cut -d. -f1,2,3)"
[ -f "$STAGE$LIBDIR/$SO_REAL" ] || {
    echo "BUILD FAIL: expected $LIBDIR/$SO_REAL in the staged tree:"; ls "$STAGE$LIBDIR" 2>/dev/null; exit 1;
}
NEEDED="$(objdump -p "$STAGE$LIBDIR/$SO_REAL" | awk '/NEEDED/ {print $NF}' | sort -u)"
RUNTIME_DEPS=""
for soname in $NEEDED; do
    case "$soname" in
        libfdk.so.1)  RUNTIME_DEPS="$RUNTIME_DEPS, $(fdkshell_fdk_runtime_pkg deb) (>= $MINVER)" ;;
        libX11.so.6)  RUNTIME_DEPS="$RUNTIME_DEPS, libx11-6" ;;
        libXrandr.so.2) RUNTIME_DEPS="$RUNTIME_DEPS, libxrandr2" ;;
        libwayland-client.so.0) RUNTIME_DEPS="$RUNTIME_DEPS, libwayland-client0" ;;
        libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|ld-linux-*.so.*) ;; # libc6, implicit
        libgcc_s.so.1) RUNTIME_DEPS="$RUNTIME_DEPS, libgcc-s1" ;;
        *) echo "BUILD FAIL: unmapped NEEDED soname '$soname' — extend the map in fdk-shell/packaging/debian/build-deb.sh" >&2; exit 1 ;;
    esac
done
RUNTIME_DEPS="$(printf '%s' "$RUNTIME_DEPS" | sed 's/^, //')"
echo "== .so NEEDED -> Depends: $RUNTIME_DEPS"

# --- split into the two package trees ------------------------------------
RT="$STAGE/pkg-libfdk-shell0"
DV="$STAGE/pkg-fdk-shell-dev"
mkdir -p "$RT/DEBIAN" "$DV/DEBIAN" \
         "$RT/usr/share/doc/libfdk-shell0" "$DV/usr/share/doc/fdk-shell-dev"

# libfdk-shell0: versioned .so + SONAME symlink
mkdir -p "$RT$LIBDIR"
mv "$STAGE$LIBDIR/$SO_REAL" "$RT$LIBDIR/"
mv "$STAGE$LIBDIR/libfdk-shell.so.0" "$RT$LIBDIR/" 2>/dev/null || \
    ln -sf "$SO_REAL" "$RT$LIBDIR/libfdk-shell.so.0"

# fdk-shell-dev: header, dev symlink, pkgconfig
mkdir -p "$DV/usr/include"
mv "$STAGE/usr/include/fdk-shell" "$DV/usr/include/fdk-shell"
mkdir -p "$DV$LIBDIR/pkgconfig"
mv "$STAGE$LIBDIR/libfdk-shell.so" "$DV$LIBDIR/libfdk-shell.so"
mv "$STAGE$LIBDIR/pkgconfig/fdk-shell.pc" "$DV$LIBDIR/pkgconfig/"

# leftovers must be empty — anything else means the split is stale
if find "$STAGE" -path "$STAGE/pkg-*" -prune -o -type f -print | grep -q .; then
    echo "BUILD FAIL: unassigned files in the fdk-shell install tree:"
    find "$STAGE" -path "$STAGE/pkg-*" -prune -o -type f -print
    exit 1
fi

# --- docs ----------------------------------------------------------------
for pkgdir in "$RT/usr/share/doc/libfdk-shell0" "$DV/usr/share/doc/fdk-shell-dev"; do
    install -m 644 LICENSE "$pkgdir/LICENSE"
    install -m 644 THIRD-PARTY-NOTICES.md "$pkgdir/THIRD-PARTY-NOTICES.md"
done

# --- DEBIAN control files ------------------------------------------------
sed -e "s|@PKGVER@|$PKGVER|g" -e "s|@ARCH@|$ARCH|" -e "s|@RUNTIME_DEPS@|$RUNTIME_DEPS|" \
    fdk-shell/packaging/debian/control.libfdk-shell0 > "$RT/DEBIAN/control"
sed -e "s|@PKGVER@|$PKGVER|g" -e "s|@ARCH@|$ARCH|" -e "s|@MINVER@|$MINVER|" \
    fdk-shell/packaging/debian/control.fdk-shell-dev > "$DV/DEBIAN/control"
printf 'activate-noawait ldconfig\n' > "$RT/DEBIAN/triggers"
for pkgdir in "$RT" "$DV"; do
    (cd "$pkgdir" && find . -type f ! -path './DEBIAN/*' | sed 's|^\./||' | \
     while IFS= read -r f; do md5sum "$f"; done) > "$pkgdir/DEBIAN/md5sums"
done

# --- build the archives ---------------------------------------------------
mkdir -p dist
dpkg-deb --build --root-owner-group "$RT" "dist/libfdk-shell0_${PKGVER}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$DV" "dist/fdk-shell-dev_${PKGVER}_${ARCH}.deb"

echo "== built: =="
ls -la dist/*fdk-shell*

# --- metadata + payload verification --------------------------------------
echo "== extraction-based payload verification =="
dpkg-deb -I "dist/libfdk-shell0_${PKGVER}_${ARCH}.deb" | sed -n '1,12p'
dpkg-deb -I "dist/fdk-shell-dev_${PKGVER}_${ARCH}.deb" | sed -n '1,12p'
echo "--- libfdk-shell0 contents:"
dpkg-deb -c "dist/libfdk-shell0_${PKGVER}_${ARCH}.deb" | awk '{print $NF}' | grep -v '/$' | sort
echo "--- fdk-shell-dev contents:"
dpkg-deb -c "dist/fdk-shell-dev_${PKGVER}_${ARCH}.deb" | awk '{print $NF}' | grep -v '/$' | sort

# --- the installed-stack smoke (FDK + fdk-shell both via dpkg) ----------
dpkg -i "dist/libfdk-shell0_${PKGVER}_${ARCH}.deb" \
       "dist/fdk-shell-dev_${PKGVER}_${ARCH}.deb"
ldconfig 2>/dev/null || true
if fdk_start_xvfb; then
    fdkshell_run_smoke "$VERSION" || { fdk_stop_xvfb; echo "BUILD FAIL: fdk-shell graphical smoke test"; exit 1; }
    fdk_stop_xvfb
fi

echo "== FDK-SHELL DEB BUILD OK ($VERSION, arch $ARCH) =="
