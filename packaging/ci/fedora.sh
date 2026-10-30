#!/bin/sh
# packaging/ci/fedora.sh — host-side orchestrator for the FDK RPM
# pipeline. Runs on a GitHub-hosted Ubuntu runner (docker available)
# or any docker-capable Linux box.
#
#  1. BUILD inside a pinned almalinux:10 container (glibc 2.38): the
#     full test battery, then a native rpmbuild from a pristine
#     source tarball.
#  2. INSTALL-TESTS in CLEAN containers: AlmaLinux 10 and Fedora 42.
#
# Building on the OLDEST supported glibc (Alma 10) makes the artifacts
# forward-compatible with newer Fedora releases; the reverse (build on
# Fedora, run on RHEL 10) would not hold. Artifacts land in dist/.

set -eu
cd "$(dirname "$0")/../.."
. "$(dirname "$0")/common.sh"
fdk_require_release_posture

RUN() { # RUN <image> <script> [args...]
    img="$1"; shift
    echo "== docker run $img: $* =="
    docker run --rm -v "$PWD":/work -w /work -e HOME=/tmp \
        --entrypoint sh "$img" -c "$*"
}

echo "===== FDK rpm pipeline ====="
docker pull -q almalinux:10

RUN almalinux:10 '
    set -eu
    # libXtst-devel: the fdk-shell X11 suite drives its passive-grab
    # passthrough test with XTEST-synthesized keys — test
    # infrastructure, the same class as the X11 dev headers. (On
    # this el10 builder the suite still self-skips without Xvfb;
    # the package keeps the builder honest the day it does not.)
    # The base image drifts: a stale container plus a fresh repo makes
    # any NEW package pull a partial toolchain upgrade (the glibc
    # cascade that broke this builder on 2026-10-28: the transaction
    # upgraded glibc mid-session and the libX11-devel files never
    # landed). One coherent full update first, then the build set.
    dnf -y -q update || \
        echo "NOTE: full container update failed — continuing with the image state (disclosed)"
    dnf install -y -q \
        gcc make pkgconf-pkg-config rpm-build cpio \
        libasan libubsan \
        libX11-devel libXext-devel libXtst-devel zlib-devel \
        libXrandr-devel \
        wayland-devel libxkbcommon-devel \
        dejavu-sans-fonts dejavu-sans-mono-fonts \
        fontconfig \
        harfbuzz fribidi
    # fontconfig: the el10 image does NOT pull it in with the font
    # packages — without libfontconfig.so.1 the whole dlopen fallback
    # channel is absent and the shaping matrix liveness floor skips
    # every leg (found live on CI: the WARN said it plainly).
    # RHEL 10 ships no standalone Xorg server: Xvfb is absent even
    # from CRB. The build container then skips the X11 suite
    # (disclosed by the build script); the Fedora 42 install-test
    # still runs the full graphical validation.
    dnf install -y -q --enablerepo=crb xorg-x11-server-Xvfb 2>/dev/null || \
        echo "XVFB-SKIP (container-level): no Xvfb in the el10 repositories — the X11 suite is skipped on this builder and disclosed"
    # A CJK font for the shaping suites per-script fallback leg. Any
    # flavor works (the resolver accepts TrueType-flavored faces); a
    # repo without one is DISCLOSED — the shaping fallback leg skips
    # honestly (primary .notdef) rather than failing.
    dnf install -y -q google-noto-sans-cjk-ttc-fonts 2>/dev/null || \
        dnf install -y -q wqy-zenhei-fonts 2>/dev/null || \
        echo "CJK-FONT-SKIP: no CJK font package in this repo — the shaping fallback leg skips honestly (disclosed)"
    # The per-script Noto faces (Arabic, Devanagari, Hebrew, Thai,
    # Bengali, ...) for the shaping suite six-script fallback
    # matrix — the unsorted-table root cause behind the §10 desktop
    # report was invisible on machines that only had the one CJK
    # font. The per-script packages are the split Fedora ships;
    # a repo without them falls back to the meta and, failing that,
    # is DISCLOSED (the matrix legs skip honestly).
    dnf install -y -q google-noto-sans-arabic-fonts \
        google-noto-sans-hebrew-fonts \
        google-noto-sans-devanagari-fonts \
        google-noto-sans-thai-fonts \
        google-noto-sans-bengali-fonts 2>/dev/null || \
        dnf install -y -q google-noto-fonts 2>/dev/null || \
        echo "SCRIPT-FONT-SKIP: no per-script Noto packages in this repo — the six-script matrix legs skip honestly (disclosed)"
    # The default sans-serif stays DejaVu Sans (the fontconfig pin
    # below): the GUI geometry tests are calibrated against its
    # metrics, and a base Noto face would otherwise win the default
    # ranking; the per-script faces serve the FALLBACK channel,
    # which is queried by language, not by the default family.
    # (Heredoc delimiter deliberately UNQUOTED and the block free of
    # apostrophes: this whole RUN argument is single-quoted shell —
    # the quoting bug class commit 2537955 fixed once already.)
    cat > /etc/fonts/local.conf <<XMLCONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <match target="pattern">
    <test qual="any" name="family"><string>sans-serif</string></test>
    <edit name="family" mode="prepend" binding="strong"><string>DejaVu Sans</string></edit>
  </match>
</fontconfig>
XMLCONF
    # A name-keyed CFF font for the CFF suites real-font legs (the
    # loader now takes OTTO faces; the vertex-agreement sweep and the
    # fuzz battery want real Type 2 data). Optional and disclosed: the
    # crafted-font legs always run, the real-font legs skip without it.
    dnf install -y -q urw-base35-fonts 2>/dev/null || \
        echo "URW-FONT-SKIP: no urw-base35-fonts in this repo — the CFF real-font legs skip honestly (disclosed)"
    sh packaging/fedora/build-rpm.sh
    sh fdk-shell/packaging/fedora/build-rpm.sh
'

for img in almalinux:10 fedora:42; do
    docker pull -q "$img"
    RUN "$img" 'sh packaging/fedora/install-test.sh'
    RUN "$img" 'sh fdk-shell/packaging/fedora/install-test.sh'
done

echo "===== FDK rpm pipeline OK (FDK + fdk-shell) ====="
ls -la dist/
