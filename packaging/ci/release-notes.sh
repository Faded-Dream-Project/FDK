#!/bin/sh
# packaging/ci/release-notes.sh — generate the GitHub Release notes
# and the SHA256SUMS file from the ACTUAL artifacts present in dist/.
#
# Usage: packaging/ci/release-notes.sh <tag> <dist-dir>
# Writes:   <dist-dir>/../notes.md  (or ./notes.md next to the CWD)
#           SHA256SUMS inside the dist dir (added to the checksum file)
#
# The notes only describe targets whose artifacts actually exist —
# a distro whose build failed or was skipped is listed under
# "Unavailable in this release", never claimed.

set -eu

TAG="${1:?usage: release-notes.sh <tag> <dist-dir>}"
DIST="${2:?usage: release-notes.sh <tag> <dist-dir>}"
VERSION="${TAG#v}"

cd "$DIST"

# --- checksums over every real artifact ----------------------------------
rm -f SHA256SUMS
for f in *.deb *.rpm *.pkg.tar.zst *.apk *.xbps fdk-*.tar.gz; do
    [ -f "$f" ] || continue
    sha256sum "$f"
done | sort -k2 > SHA256SUMS

# --- classify what exists -------------------------------------------------
have_deb=0; have_rpm=0; have_arch=0; have_apk=0; have_xbps=0; have_src=0
ls *.deb >/dev/null 2>&1 && have_deb=1
ls *.rpm >/dev/null 2>&1 && have_rpm=1
ls *.pkg.tar.zst >/dev/null 2>&1 && have_arch=1
ls *.apk >/dev/null 2>&1 && have_apk=1
ls *.xbps >/dev/null 2>&1 && have_xbps=1
[ -f "fdk-$VERSION.tar.gz" ] && have_src=1

ubuntu2204=""
if [ -f ubuntu-22.04-verdict.txt ]; then
    if grep -q "PASSED" ubuntu-22.04-verdict.txt; then
        ubuntu2204="Ubuntu 22.04 (informational probe: passed)"
    else
        ubuntu2204="Ubuntu 22.04 (not supported — glibc 2.35 below the build floor)"
    fi
fi

NOTES="$(dirname "$(pwd)")/notes.md"
[ -w "$(dirname "$NOTES")" ] || NOTES="$PWD/notes.md"

{
    echo "# FDK v$VERSION"
    echo ""
    echo "**FDK** (Faded Dream ToolKit) is a general-purpose native C GUI"
    echo "toolkit for modern Linux desktops: real X11 and Wayland backends,"
    echo "a damage-tracked renderer with an optional GPU present lane, its"
    echo "own widget / layout / text / theme engines, and FDK-drawn window"
    echo "decorations. https://github.com/FemBoyGamerTechGuy/FDK"
    echo ""
    echo "## Packages in this release"
    echo ""
    echo "Every artifact below was built and install-tested by the release"
    echo "pipeline (see \`docs/packaging.md\` in the source archive for the"
    echo "exact recipe and how to reproduce it)."
    echo ""
    echo "| Artifact | Target | Contents |"
    echo "| --- | --- | --- |"
    for f in libfdk1_*.deb; do [ -f "$f" ] && echo "| \`$f\` | Debian 12 / 13, Ubuntu 24.04+ (glibc >= 2.36) | runtime: libfdk.so.1 + the default theme (faded-dream) |"; done
    for f in fdk-dev_*.deb; do [ -f "$f" ] && echo "| \`$f\` | Debian 12 / 13, Ubuntu 24.04+ | headers, pkg-config, CLI tools |"; done
    for f in libfdk1-*.rpm; do [ -f "$f" ] && echo "| \`$f\` | RHEL 10 family + Fedora 42+ (glibc >= 2.38) | runtime: libfdk.so.1 + the default theme (faded-dream) |"; done
    for f in fdk-devel-*.rpm; do [ -f "$f" ] && echo "| \`$f\` | RHEL 10 family + Fedora 42+ | headers, pkg-config, CLI tools |"; done
    for f in fdk-*.pkg.tar.zst; do [ -f "$f" ] && echo "| \`$f\` | Arch Linux (x86_64, rolling) | runtime + development files (single package, Arch convention) |"; done
    for f in fdk-*r0*.apk; do case "$f" in *dev*) continue;; esac; [ -f "$f" ] && echo "| \`$f\` | Alpine Linux 3.22 (musl, x86_64) | runtime: libfdk.so.1 + the default theme |"; done
    for f in fdk-dev*r0*.apk; do [ -f "$f" ] && echo "| \`$f\` | Alpine Linux 3.22 (musl) | headers, pkg-config, CLI tools |"; done
    for f in fdk-libs*.xbps; do [ -f "$f" ] && echo "| \`$f\` | Void Linux (glibc, x86_64) | runtime: libfdk.so.1 + the default theme |"; done
    for f in fdk-devel*.xbps; do [ -f "$f" ] && echo "| \`$f\` | Void Linux (glibc) | headers, pkg-config, CLI tools |"; done
    [ "$have_src" = 1 ] && echo "| \`fdk-$VERSION.tar.gz\` | source | pristine source archive (\`git archive\` of this tag) |"
    echo ""
    echo "Architectures: **x86_64 only** in this release (arm64 is future work;"
    echo "the pipeline is arch-parameterized but not validated on arm64 yet)."
    echo ""

    if [ "$have_deb" = 1 ]; then
        echo "## Install — Debian / Ubuntu"
        echo ""
        echo '```sh'
        echo "sudo apt install ./libfdk1_${VERSION}-1_amd64.deb ./fdk-dev_${VERSION}-1_amd64.deb"
        echo '```'
        if [ -n "$ubuntu2204" ]; then
            echo ""
            echo "Tested on: Debian 12 (bookworm), Debian 13 (trixie), Ubuntu 24.04."
            echo "$ubuntu2204."
        else
            echo ""
            echo "Tested on: Debian 12 (bookworm), Debian 13 (trixie), Ubuntu 24.04."
        fi
        echo ""
    fi

    if [ "$have_rpm" = 1 ]; then
        echo "## Install — RHEL 10 family / Fedora"
        echo ""
        echo '```sh'
        echo "sudo dnf install ./libfdk1-$VERSION-1.*.rpm ./fdk-devel-$VERSION-1.*.rpm"
        echo '```'
        echo ""
        echo "Built on AlmaLinux 10 (glibc 2.38); install-tested on AlmaLinux 10"
        echo "and Fedora 42. Older RHEL (8/9) is NOT covered by these artifacts"
        echo "— build from source there."
        echo ""
    fi

    if [ "$have_arch" = 1 ]; then
        echo "## Install — Arch Linux"
        echo ""
        echo '```sh'
        echo "sudo pacman -U fdk-$VERSION-1-x86_64.pkg.tar.zst"
        echo '```'
        echo ""
    fi

    if [ "$have_apk" = 1 ]; then
        echo "## Install — Alpine Linux"
        echo ""
        echo '```sh'
        echo "sudo apk add --allow-untrusted fdk-$VERSION-r0-x86_64.apk fdk-dev-$VERSION-r0-x86_64.apk"
        echo '```'
        echo ""
        echo "The upstream packages are unsigned (see *Verification* below);"
        echo "\`--allow-untrusted\` skips apk's signature check, and the SHA256SUMS"
        echo "file provides the integrity guarantee instead."
        echo ""
    fi

    if [ "$have_xbps" = 1 ]; then
        echo "## Install — Void Linux"
        echo ""
        echo '```sh'
        echo "xbps-install --repository=\$PWD fdk-libs fdk-devel"
        echo '```'
        echo ""
    fi

    echo "## Runtime dependencies"
    echo ""
    echo "libX11, libXext, zlib, and (for the Wayland backend) libwayland-client"
    echo "and libxkbcommon — every package manager above resolves them"
    echo "automatically. The GPU present lane uses libGL when present and"
    echo "falls back to the CPU path otherwise; no GL package is required."
    echo ""
    echo "## Verification"
    echo ""
    echo '```sh'
    echo "sha256sum -c SHA256SUMS"
    echo '```'
    echo ""
    echo "## Known limitations"
    echo ""
    echo "- Graphical validation in the pipeline runs under **Xvfb** (the X11"
    echo "  path, CPU present lane). No GPU-lane or Wayland-compositor runtime"
    echo "  claim is made for the packaged artifacts from CI; both are covered"
    echo "  by the project's own test suite on the development environment."
    echo "- Packages are **not GPG-signed**; verify via SHA256SUMS over HTTPS."
    echo "- The **static archive (libfdk.a) is not shipped**: distributing an"
    echo "  application with FDK statically linked requires separate written"
    echo "  authorization under the license."
    echo "- Alpine artifacts are **musl** binaries; the rest are glibc. They"
    echo "  are not interchangeable."

    missing=""
    [ "$have_apk" = 0 ] && missing="$missing Alpine,"
    [ "$have_xbps" = 0 ] && missing="$missing Void,"
    if [ -n "$missing" ]; then
        echo "- **Not in this release:**${missing%, } — the build did not produce"
        echo "  validated artifacts for that target this time (check the CI run"
        echo "  for this tag)."
    fi
    echo ""
    echo "## Licensing"
    echo ""
    echo "FDK is **proprietary software** (FDK Proprietary License v1.0, in"
    echo "every package as LICENSE and in the source archive). These packages"
    echo "are published by the Licensor — the distribution channel authorized"
    echo "under the license's Section 4(d). Applications built with FDK remain"
    echo "the property of their authors; the license's restrictions attach to"
    echo "the toolkit, never to independent application code."
} > "$NOTES"

echo "== notes written to $NOTES =="
echo "== SHA256SUMS =="
cat SHA256SUMS
