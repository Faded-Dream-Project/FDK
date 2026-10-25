#!/bin/sh
# packaging/ci/alpine.sh — host-side orchestrator for the FDK Alpine
# pipeline. Runs on a GitHub-hosted Ubuntu runner (docker available)
# or any docker-capable Linux box.
#
# BUILD and INSTALL-TEST both happen inside a pinned alpine:3.22
# container — the MUSL target. musl is NOT interchangeable with the
# glibc environments of the other pipelines; these artifacts run only
# on musl systems. Artifacts land in dist/.

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

echo "===== FDK alpine pipeline (musl) ====="
docker pull -q alpine:3.22

# 'tar' (GNU) is required: busybox tar lacks --transform, which the
# pristine-tarball step uses. Xvfb comes from either of the two known
# package names depending on the release.
RUN alpine:3.22 '
    set -eu
    apk add --no-cache \
        build-base pkgconf tar coreutils alpine-sdk \
        libx11-dev libxext-dev zlib-dev \
        wayland-dev libxkbcommon-dev \
        font-dejavu
    # mesa GL runtime: the X11 suite forces the GL present lane in its
    # equivalence/downgrade groups — llvmpipe satisfies them, exactly
    # like the glibc containers get implicitly via their xvfb stack.
    # Driver package names moved across Alpine releases; try the
    # known combinations in order and disclose if none is available.
    apk add --no-cache mesa-gl mesa-dri-gallium 2>/dev/null || \
        apk add --no-cache mesa-gl 2>/dev/null || \
        apk add --no-cache mesa 2>/dev/null || \
        echo "NOTE: no mesa GL package combination available; the forced-lane suite groups would fail (disclosed)"
    apk add --no-cache xvfb 2>/dev/null || \
        apk add --no-cache xorg-server-xvfb 2>/dev/null || \
        echo "XVFB-SKIP (container-level): no Xvfb package found; the X11 suite and graphical smoke will be skipped and disclosed"
    sh packaging/alpine/build-apk.sh
    sh fdk-shell/packaging/alpine/build-apk.sh
'

RUN alpine:3.22 '
    set -eu
    apk add --no-cache tar coreutils
    sh packaging/alpine/install-test.sh
    sh fdk-shell/packaging/alpine/install-test.sh
'

echo "===== FDK alpine pipeline OK (FDK + fdk-shell) ====="
ls -la dist/
