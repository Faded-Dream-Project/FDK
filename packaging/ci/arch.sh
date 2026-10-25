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
    pacman -Sy --noconfirm --needed \
        base-devel pkgconf libx11 libxext zlib wayland libxkbcommon \
        xorg-server-xvfb ttf-dejavu tar
    sh packaging/arch/build-pkg.sh
    sh fdk-shell/packaging/arch/build-pkg.sh
'

RUN archlinux:base 'sh packaging/arch/install-test.sh'
RUN archlinux:base 'sh fdk-shell/packaging/arch/install-test.sh'

echo "===== FDK arch pipeline OK (FDK + fdk-shell) ====="
ls -la dist/
