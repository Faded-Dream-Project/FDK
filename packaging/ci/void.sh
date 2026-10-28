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
. "$(dirname "$0")/common.sh"
fdk_require_release_posture

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
    # libXtst-devel: the fdk-shell X11 suite drives its passive-grab
    # passthrough test with XTEST-synthesized keys — test
    # infrastructure, the same class as the X11 dev headers.
    # The round-robin mirror (repo-default.voidlinux.org) went
    # unreachable for hours on 2026-10-26 (runner + two other
    # networks) — the old single retry of the same dead host could
    # not recover. Rotate the real mirror list FIRST (the
    # fdk_void_mirror_rotate helper in common.sh); every xbps line
    # below then runs
    # against the first mirror that answered.
    . packaging/ci/common.sh
    fdk_void_mirror_rotate
    xbps-install -Sy base-devel pkgconf findutils zstd \
        libX11-devel libXext-devel libXtst-devel zlib-devel \
        wayland-devel libxkbcommon-devel \
        dejavu-fonts-ttf tar \
        harfbuzz fribidi
    # GL runtime for the X11-suite forced-lane groups (llvmpipe),
    # layered over the known package-name combinations:
    xbps-install -Sy mesa mesa-dri 2>/dev/null || \
        xbps-install -Sy libglvnd mesa-dri 2>/dev/null || \
        xbps-install -Sy mesa 2>/dev/null || \
        echo "NOTE: no GL runtime package combination available; the forced-lane suite groups would fail (disclosed)"
    # A CJK font for the shaping suite's per-script fallback leg —
    # non-fatal: a repo without one is disclosed and the suite's
    # fallback leg skips honestly (primary .notdef).
    xbps-install -Sy noto-fonts-cjk 2>/dev/null || \
        xbps-install -Sy wqy-zenhei-font 2>/dev/null || \
        echo "CJK-FONT-SKIP: no CJK font package in this repo — the shaping suite's fallback leg skips honestly (disclosed)"
    xbps-install -Sy xorg-server-xvfb 2>/dev/null || \
        xbps-install -Sy xvfb 2>/dev/null || \
        echo "XVFB-SKIP (container-level): no Xvfb package found; the X11 suite and graphical smoke will be skipped and disclosed"
    sh packaging/void/build-xbps.sh
    sh fdk-shell/packaging/void/build-xbps.sh
'

RUN "$VOID_IMAGE" 'sh packaging/void/install-test.sh'
RUN "$VOID_IMAGE" 'sh fdk-shell/packaging/void/install-test.sh'

echo "===== FDK void pipeline OK (FDK + fdk-shell) ====="
ls -la dist/
