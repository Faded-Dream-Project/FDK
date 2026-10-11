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
    # Xvfb lives in the CRB repository on RHEL-10 clones.
    dnf install -y -q --enablerepo=crb \
        gcc make pkgconf-pkg-config rpm-build cpio \
        libX11-devel libXext-devel zlib-devel \
        wayland-devel libxkbcommon-devel \
        dejavu-sans-fonts dejavu-sans-mono-fonts \
        xorg-x11-server-Xvfb
    sh packaging/fedora/build-rpm.sh
'

for img in almalinux:10 fedora:42; do
    docker pull -q "$img"
    RUN "$img" 'sh packaging/fedora/install-test.sh'
done

echo "===== FDK rpm pipeline OK ====="
ls -la dist/
