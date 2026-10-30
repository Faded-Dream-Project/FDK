#!/bin/sh
# packaging/ci/debian.sh — host-side orchestrator for the FDK .deb
# pipeline. Runs on a GitHub-hosted Ubuntu runner (docker available)
# or any docker-capable Linux box.
#
#  1. BUILD inside a pinned debian:bookworm container (glibc 2.36,
#     GCC 12): the full test battery, then the native dpkg-deb build.
#  2. INSTALL-TESTS in CLEAN containers of every supported target:
#     Debian 12, Debian 13, Ubuntu 24.04 (release blockers), plus an
#     informational Ubuntu 22.04 probe (glibc 2.35) that reports its
#     verdict without gating the release.
#
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

echo "===== FDK debian pipeline ====="
docker pull -q debian:bookworm

RUN debian:bookworm '
    set -eu
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    # libxtst-dev: the fdk-shell X11 suite drives its passive-grab
    # passthrough test with XTEST-synthesized keys — test
    # infrastructure, the same class as the X11 dev headers.
    apt-get install -y --no-install-recommends \
        build-essential pkg-config \
        libx11-dev libxext-dev libxtst-dev zlib1g-dev \
        libwayland-dev libxkbcommon-dev \
        xvfb fonts-dejavu-core \
        libharfbuzz0b libfribidi0 fonts-wqy-zenhei fonts-urw-base35 \
        libxrandr-dev \
        fonts-noto-core
    # fonts-noto-core: the per-script faces (Arabic, Devanagari,
    # Hebrew, Thai, Bengali, ...) the shaping suite six-script
    # fallback matrix needs — the unsorted-table root cause behind
    # the §10 desktop report was invisible on machines that only
    # had the one CJK font.
    # The default sans-serif stays DejaVu Sans (a fontconfig pin
    # below): the GUI geometry tests are calibrated against its
    # metrics, and fonts-noto-core would otherwise win the default
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
    sh packaging/debian/build-deb.sh
    sh fdk-shell/packaging/debian/build-deb.sh
'
# The container writes dist/ as root; take it back for the runner user
# so the verdict marker below (and artifact upload) can write it.
sudo chown -R "$(id -u):$(id -g)" dist 2>/dev/null || true

for img in debian:bookworm debian:trixie ubuntu:24.04; do
    docker pull -q "$img"
    RUN "$img" 'sh packaging/debian/install-test.sh'
    RUN "$img" 'sh fdk-shell/packaging/debian/install-test.sh'
done

# Informational older-glibc probe: reported, never gating. The verdict
# is recorded in dist/ so the release notes state exactly what was seen.
docker pull -q ubuntu:22.04
mkdir -p dist
if RUN ubuntu:22.04 'sh packaging/debian/install-test.sh'; then
    echo "ubuntu-22.04 install-test PASSED (glibc 2.35) — support claim upgraded" \
        > dist/ubuntu-22.04-verdict.txt
    echo "== INFO: ubuntu:22.04 (glibc 2.35) install-test PASSED — support claim upgraded =="
else
    echo "ubuntu-22.04 install-test did NOT pass (glibc floor above 2.35) — documented as unsupported" \
        > dist/ubuntu-22.04-verdict.txt
    echo "== INFO: ubuntu:22.04 (glibc 2.35) install-test did NOT pass — documented as unsupported; NOT a release blocker =="
fi

echo "===== FDK debian pipeline OK (FDK + fdk-shell) ====="
ls -la dist/
