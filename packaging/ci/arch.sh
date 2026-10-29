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
        libxrandr
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
