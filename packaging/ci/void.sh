#!/bin/sh
# packaging/ci/void.sh — host-side orchestrator for the FDK Void
# Linux pipeline. Runs on a GitHub-hosted Ubuntu runner (docker
# available) or any docker-capable Linux box.
#
# BUILD and INSTALL-TEST both happen inside the pinned Void glibc
# container (ghcr.io/void-linux/void-glibc — Void's official image).
# Artifacts land in dist/.

set -eu
cd "$(dirname "$0")/../.."

RUN() { # RUN <image> <script> [args...]
    img="$1"; shift
    echo "== docker run $img: $* =="
    docker run --rm -v "$PWD":/work -w /work -e HOME=/tmp \
        --entrypoint sh "$img" -c "$*"
}

VOID_IMAGE="ghcr.io/void-linux/void-glibc:latest"

echo "===== FDK void pipeline ====="
docker pull -q "$VOID_IMAGE"

RUN "$VOID_IMAGE" '
    set -eu
    xbps-install -Sy base-devel pkgconf findutils \
        libX11-devel libXext-devel zlib-devel \
        wayland-devel libxkbcommon-devel \
        dejavu-fonts-ttf tar
    # GL runtime for the X11-suite forced-lane groups (llvmpipe),
    # layered over the known package-name combinations:
    xbps-install -Sy mesa mesa-dri 2>/dev/null || \
        xbps-install -Sy libglvnd mesa-dri 2>/dev/null || \
        xbps-install -Sy mesa 2>/dev/null || \
        echo "NOTE: no GL runtime package combination available; the forced-lane suite groups would fail (disclosed)"
    xbps-install -Sy xorg-server-xvfb 2>/dev/null || \
        xbps-install -Sy xvfb 2>/dev/null || \
        echo "XVFB-SKIP (container-level): no Xvfb package found; the X11 suite and graphical smoke will be skipped and disclosed"
    sh packaging/void/build-xbps.sh
'

RUN "$VOID_IMAGE" 'sh packaging/void/install-test.sh'

echo "===== FDK void pipeline OK ====="
ls -la dist/
