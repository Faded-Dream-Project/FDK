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
    # libxtst-dev: the fdk-shell X11 suite drives its passive-grab
    # passthrough test with XTEST-synthesized keys — test
    # infrastructure, the same class as the X11 dev headers.
    apk add --no-cache \
        build-base pkgconf tar coreutils alpine-sdk \
        libx11-dev libxext-dev libxtst-dev zlib-dev \
        wayland-dev libxkbcommon-dev \
        font-dejavu \
        harfbuzz fribidi font-wqy-zenhei \
        libxrandr-dev
    # The per-script Noto faces (Arabic, Devanagari, Hebrew, Thai,
    # Bengali, ...) for the shaping suite's six-script fallback
    # matrix — the §10 desktop report's root cause (an unsorted
    # script table) was invisible on machines that only had the one
    # CJK font. Alpine splits font-noto per script; a repo without
    # the split is DISCLOSED (the matrix's legs skip honestly).
    apk add --no-cache font-noto-arabic font-noto-hebrew \
        font-noto-devanagari font-noto-thai font-noto-bengali \
        2>/dev/null || \
        echo "SCRIPT-FONT-SKIP: no per-script font-noto packages in this repo — the six-script matrix legs skip honestly (disclosed)"
    # A name-keyed CFF font for the CFF suites real-font legs.
    # Optional and disclosed: the crafted-font legs always run.
    apk add --no-cache font-urw-base35 2>/dev/null || \
        echo "URW-FONT-SKIP: no font-urw-base35 in this repo — the CFF real-font legs skip honestly (disclosed)"
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
