#!/bin/sh
# packaging/ci/arch.sh — host-side orchestrator for the FDK Arch
# package pipeline. Runs on a GitHub-hosted Ubuntu runner (docker
# available) or any docker-capable Linux box.
#
# BUILD and INSTALL-TEST both happen inside a pinned archlinux:base
# container (rolling release; the release notes record the snapshot
# date). Artifacts land in dist/.

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

echo "===== FDK arch pipeline ====="
docker pull -q archlinux:base

RUN archlinux:base '
    set -eu
    # libxtst: the fdk-shell X11 suite drives its passive-grab
    # passthrough test with XTEST-synthesized keys — test
    # infrastructure, the same class as the X11 headers (the arch
    # libx11 is unsplit; libxtst carries both header and library).
    pacman -Sy --noconfirm --needed \
        base-devel pkgconf libx11 libxext libxtst zlib wayland libxkbcommon \
        xorg-server-xvfb ttf-dejavu tar \
        harfbuzz fribidi wqy-zenhei \
        libxrandr \
        noto-fonts
    # noto-fonts (the base set): the per-script faces (Arabic,
    # Devanagari, Hebrew, Thai, Bengali, ...) the shaping suite
    # six-script fallback matrix needs — the unsorted-table root
    # cause behind the §10 desktop report was invisible on machines
    # that only had the one CJK font.
    # The default sans-serif stays DejaVu Sans (the fontconfig pin
    # below): the GUI geometry tests are calibrated against its
    # metrics, and the noto-fonts base face would otherwise win the
    # default ranking; the per-script faces serve the FALLBACK
    # channel, which is queried by language, not by the default
    # family. (Heredoc delimiter deliberately UNQUOTED and the block
    # free of apostrophes: this whole RUN argument is single-quoted
    # shell — the quoting bug class commit 2537955 fixed once
    # already.)
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
    # A name-keyed CFF font for the CFF suites real-font legs.
    # Optional and disclosed: the crafted-font legs always run.
    pacman -Sy --noconfirm --needed gsfonts 2>/dev/null || \
        echo "URW-FONT-SKIP: no gsfonts in this repo — the CFF real-font legs skip honestly (disclosed)"
    sh packaging/arch/build-pkg.sh
    sh fdk-shell/packaging/arch/build-pkg.sh
'

RUN archlinux:base 'sh packaging/arch/install-test.sh'
RUN archlinux:base 'sh fdk-shell/packaging/arch/install-test.sh'

echo "===== FDK arch pipeline OK (FDK + fdk-shell) ====="
ls -la dist/
