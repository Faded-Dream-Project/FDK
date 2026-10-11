# FDK Packaging and Release

This document covers everything around distributing FDK: building it
from source, installing the binary packages the project publishes,
what those packages contain, how the release pipeline produces and
validates them, and how a maintainer cuts a release.

> **v1.0.0 is released.** The public version is `1.0.0` — the
> deliberate, recorded decision of 2026-10-12 (`docs/versioning.md`),
> made after the picker overhaul closed the 2026-10-11 retraction's
> named blockers and after the project owner's real-hardware
> validation. The packaging pipeline is live: a `v*` tag push runs
> the full cross-distribution matrix (build + native package +
> clean-container install-tests per family) and stages a draft
> GitHub Release with every validated artifact plus `SHA256SUMS`.
> The release-posture guard (`fdk_require_release_posture`) keeps
> its place as infrastructure: if the public version ever returns
> to a pre-1.0 posture, packaging discloses a `PACKAGING-SKIP`
> rather than staging an inconsistent payload.

- Repository home: https://github.com/FemBoyGamerTechGuy/FDK
- Releases: https://github.com/FemBoyGamerTechGuy/FDK/releases
- Build from source: `docs/build.md` (the authoritative reference)

## The release model in one paragraph

FDK is released as **source archives plus native binary packages**
(.deb, .rpm, .pkg.tar.zst, .apk, .xbps) built by GitHub Actions on
GitHub-hosted runners, each target built **inside its own pinned
distribution container** and **install-tested in clean containers**
of every supported target before the artifacts are attached to a
draft GitHub Release. The release is published by a human after
review — nothing goes public automatically.

## Building from source

See `docs/build.md` for the complete reference. The short form on
any modern Linux with GCC, pkg-config, and the X11 development
headers:

```sh
make                # debug build (ASan+UBSan) + tools
make test           # headless test suite
make test-x11       # X11 integration suite (auto-starts a throwaway Xvfb)
make install        # PREFIX defaults to /usr/local
```

For an optimized install to a system prefix:

```sh
make FDK_CONFIG=release all
make FDK_CONFIG=release install PREFIX=/usr LIBDIR=/usr/lib
```

> **Pin `FDK_CONFIG=release` on the install invocation too.** The
> Makefile's configuration stamp exists so debug and release objects
> never mix — but its flip side is that a plain `make install` after
> `make release` **silently rebuilds everything in debug config**
> (sanitizers linked into the installed shared library) because the
> install target's prerequisites are re-evaluated in the default
> configuration. `make FDK_CONFIG=release install` in one invocation
> (or on both invocations) builds and installs exactly what you asked
> for. Every packaging recipe in `packaging/` pins the variable for
> exactly this reason, and the Debian build additionally fails loudly
> if a sanitizer runtime appears in the payload's ELF `NEEDED` list.

Optional Wayland backend: install `libwayland-dev` +
`libxkbcommon-dev` first and the build picks both backends up
automatically (`docs/build.md` "Optional Wayland").

## Installing binary packages

*(Forward-looking: applies to the first published release and every
one after it — see the note above for why none exists yet.)* All
commands below assume you downloaded the artifacts from the
[releases page](https://github.com/FemBoyGamerTechGuy/FDK/releases)
and verified them (`sha256sum -c SHA256SUMS`). `<ver>` below is the
release's version (the file names on the page carry it).

### Debian / Ubuntu (.deb)

```sh
sudo apt install ./libfdk1_<ver>-1_amd64.deb ./fdk-dev_<ver>-1_amd64.deb
```

Runtime package `libfdk1`, development package `fdk-dev`. Built on
Debian 12 (bookworm, glibc 2.36); install-tested on Debian 12,
Debian 13, Ubuntu 24.04, and Ubuntu 22.04 (glibc 2.35 — verified by
the pipeline's informational probe, which passes). Anything older
(glibc < 2.35) is below the build floor — build from source there.
`apt` resolves the runtime dependencies (libx11-6, libxext6, zlib1g,
libwayland-client0, libxkbcommon0) automatically.

### RHEL 10 family / Fedora (.rpm)

```sh
sudo dnf install ./libfdk1-<ver>-1.*.rpm ./fdk-devel-<ver>-1.*.rpm
```

Runtime package `libfdk1`, development package `fdk-devel`. Built on
AlmaLinux 10 (glibc 2.38) so the artifacts are forward-compatible
with newer Fedora; install-tested on AlmaLinux 10 and Fedora 42.
RHEL 8/9 are **not** covered by these artifacts — build from source
there. RPM's automatic soname dependencies resolve the runtime
libraries.

### Arch Linux (.pkg.tar.zst)

```sh
sudo pacman -U fdk-<ver>-1-x86_64.pkg.tar.zst
```

One package (Arch convention for a library this size) carrying the
runtime and the development files together.

### Alpine Linux (.apk, musl)

```sh
sudo apk add --allow-untrusted fdk-<ver>-r0-x86_64.apk fdk-dev-<ver>-r0-x86_64.apk
```

Runtime package `fdk` + development package `fdk-dev` (Alpine
convention splits `-dev`). **These are musl binaries** — they run on
Alpine and other musl systems only, never on the glibc distributions
above. The upstream packages are unsigned (see *Signing* below), so
`--allow-untrusted` skips apk's signature check; apk still resolves
the automatic `so:` dependencies from your enabled repositories.

### Void Linux (.xbps, glibc)

```sh
xbps-install --repository="$PWD" fdk-libs fdk-devel
```

Runtime package `fdk-libs` + development package `fdk-devel` (Void
convention). xbps resolves the shared-library dependencies from your
configured repositories.

### After installing: link your application

```sh
cc myapp.c $(pkg-config --cflags --libs fdk) -o myapp
```

Run `fdk-theme list` to see the shipped theme faces and
`fdk-theme set <face>` (or `fdk-set`) to switch the live theme.

## Runtime dependencies

`libX11`, `libXext`, `zlib`, and — when the Wayland backend is
compiled in, which the published packages always do —
`libwayland-client` and `libxkbcommon`. Every package manager above
resolves them automatically. The GPU present lane loads `libGL`
dynamically when a hardware GL renderer exists and silently stays on
the CPU present path otherwise (`docs/rendering.md`); no GL package
is required. FDK performs its own font discovery (DejaVu Sans or a
similar default UI font is picked up automatically; override with
`$FDK_FONT_FILE` / `$FDK_FONT_DIRS` — see `docs/text.md`).

## What the packages contain (and deliberately omit)

Every package family ships:

- **runtime**: `libfdk.so.1` (the SONAME; real file
  `libfdk.so.<version>`) and the ten shipped `.fdk` theme faces in
  `/usr/share/fdk/.FDKThemes`
- **development**: the headers in `/usr/include/fdk`, the
  `libfdk.so` development symlink, `fdk.pc` for pkg-config, and the
  `fdk-theme` / `fdk-set` / `fdk-prefs` command-line tools

The split granularity follows each distribution's own convention
(Debian: `libfdk1`+`fdk-dev`; Fedora: `libfdk1`+`fdk-devel`; Arch:
single `fdk`; Alpine: `fdk`+`fdk-dev`; Void: `fdk-libs`+`fdk-devel`).

**The static archive (`libfdk.a`) is not shipped in any binary
package.** Under the FDK Proprietary License's separate-runtime model
(Section 4(d)), distributing an application with the FDK runtime
bundled, embedded, or statically linked requires separate written
authorization — shipping the archive in every `-devel` package would
invite exactly that. Build from source when you legitimately need
the static archive for local, non-distributed development.

Licensing note: the binary packages are built and published **by the
Licensor** in the official repository — the distribution channel
authorized under the license's Section 4(d). Applications built with
FDK remain their authors' property (license Section 5); nothing in
the packaging claims otherwise.

## Supported distributions and architectures

| Target | Built on | Install-tested on | Architectures |
| --- | --- | --- | --- |
| `.deb` | Debian 12 (bookworm) | Debian 12, Debian 13, Ubuntu 24.04, Ubuntu 22.04 | x86_64 |
| `.rpm` | AlmaLinux 10 | AlmaLinux 10, Fedora 42 | x86_64 |
| `.pkg.tar.zst` | Arch Linux (base, rolling) | Arch Linux | x86_64 |
| `.apk` (musl) | Alpine 3.22 | Alpine 3.22 | x86_64 |
| `.xbps` | Void (glibc) | Void (glibc) | x86_64 |

arm64 is **not** produced: the pipeline is
architecture-parameterized end to end (the scripts use `uname -m`
and the images are multi-arch), but no arm64 build has been
validated, so none is claimed or shipped. Do not assume the x86_64
packages run anywhere other than the distributions listed above —
the glibc/musl floors are real (the Debian packages need glibc ≥
2.36; the RPMs glibc ≥ 2.38) and the release notes state them per
release.

## Reproducing the package builds

Every target's whole pipeline is a shell script in this repository,
driven through Docker from a Linux host with Docker installed:

```sh
sh packaging/ci/debian.sh    # build (debian:bookworm) + install-tests
sh packaging/ci/fedora.sh    # build (almalinux:10) + install-tests
sh packaging/ci/arch.sh      # build + install-test (archlinux:base)
sh packaging/ci/alpine.sh    # build + install-test (alpine:3.22, musl)
sh packaging/ci/void.sh      # build + install-test (void-glibc)
```

Each script: pulls the pinned image, builds FDK inside it with the
full test battery, produces the native package with the target
distribution's own tools, then install-tests the package in CLEAN
containers (native package manager, ldconfig, pkg-config discovery,
`fdk-theme`, a compile-and-run smoke test under Xvfb, and removal
verification). Artifacts land in `dist/`.

The per-target packaging metadata lives beside the scripts:
`packaging/debian/` (control files + build script),
`packaging/fedora/fdk.spec`, `packaging/arch/PKGBUILD`,
`packaging/alpine/APKBUILD`, `packaging/void/template` (xbps-src
reference) + `packaging/void/build-xbps.sh` (xbps-create driver).
The smoke test itself is `packaging/common/smoke.c` — a public-API
only program that opens a real window, runs the theme/font path, and
verifies the runtime version.

The GitHub Actions workflows (`.github/workflows/`) run exactly
these scripts — `ci.yml` on pushes/PRs, `release.yml` on `v*` tags —
so the CI artifacts and anything you reproduce locally are produced
by the same code.

## Sanitizer posture per environment

FDK's debug builds carry ASan+UBSan by default, and the leak gate
(exit-on-any-leak) is enforced wherever the toolchain supports it:
CI's **ubuntu job** and every development machine run the full
suites strictly. Two deliberate, disclosed relaxations exist in the
container pipelines:

- **glibc distro containers** run the X11 suite with
  `LSAN_OPTIONS=exitcode=0`: every test must still pass, but
  exit-time leak *reports* originating in third-party libraries
  (older distro libxcb leaves ~336 bytes allocated at
  `XCloseDisplay`; the stacks contain zero FDK frames) do not fail
  the run — the same third-party-exit-allocation class the project
  already suppresses for Mesa (`tests/data/lsan-gl.supp`).
- **musl and minimal-image targets** (Alpine; Void) build with
  `FDK_SANITIZE=0`: those environments ship no sanitizer runtimes
  (`-lasan`/`-lubsan` cannot link), so the same debug configuration
  runs without instrumentation there. The sanitizer gates remain
  enforced on the glibc environments above.

Nothing FDK-owned is ever exempted: a leak or UB report with FDK
frames fails everywhere it can run.

## How maintainers create and publish a release

1. **Version bump** — an explicit owner decision recorded first in
   `docs/versioning.md`: update `include/fdk/fdk_version.h`
   (major/minor/patch + string) and the pin in
   `tests/test_core.c::test_version`. The Makefile, pkg-config file,
   package versions, and SONAME all derive from the header
   automatically. If the ABI broke, the SONAME major moves with
   `FDK_VERSION_MAJOR` — nothing else to do.
2. **Full battery locally** — `make && make test && make test-x11 &&
   make release && make examples && make verify-exports` (plus
   Wayland where a compositor rig exists).
3. **Commit and push to `main`** — CI runs the full packaging matrix
   on the push. Let it go green.
4. **Tag** — `git tag -a v<version> -m "FDK <version>" && git push
   origin v<version>`. The release workflow builds every package
   from the exact tagged revision, then creates a **draft** GitHub
   Release with the artifacts, SHA256SUMS, and generated notes.
5. **Review and publish** — check the draft's artifacts and notes
   (they are generated from the artifacts that actually built:
   a failed optional target appears as "not in this release", never
   as a claim), then publish it from the Releases page.

Required checks: the Debian, RPM, Arch, and source-archive jobs must
all pass or the release job is skipped entirely. Alpine and Void are
allowed to fail without blocking the release (they are the newest
pipelines) — their artifacts are then simply absent and the notes
say so.

Workflow permissions are minimal: everything runs on the ephemeral
`GITHUB_TOKEN`; only the final release job holds `contents: write`
and only for creating the draft. No personal token is stored
anywhere in the repository or workflows.

## Signing

Release packages are **not GPG-signed**. Integrity is provided by
the `SHA256SUMS` file on the release page served over HTTPS, plus
the release's provenance (the CI run that built it, linked from the
release). Per-distribution signing (a Debian apt repository, an RPM
repository with GPG keys, apk keys) would require maintained
repository infrastructure and is future work — noted honestly here
rather than half-implemented.

## Troubleshooting

- **`error while loading shared libraries: libfdk.so.1`** — the
  runtime package (`libfdk1` / `fdk` / `fdk-libs`) is not installed;
  the development package alone is not enough at run time.
- **`Package 'fdk' not found` from pkg-config** — the development
  package is not installed, or pkg-config is not looking in the
  right libdir (try `PKG_CONFIG_PATH=/usr/lib64/pkgconfig` on
  non-multiarch layouts).
- **`version 'GLIBC_2.xx' not found`** — your distribution's glibc
  is older than the package's build floor (Debian packages: 2.36,
  RPMs: 2.38). Build from source instead; do not force-install.
- **apk refuses the package** — upstream packages are unsigned:
  `apk add --allow-untrusted ./fdk-...apk` (see *Signing*).
- **Wrong architecture** — releases ship x86_64 only; on arm64 build
  from source (`make FDK_CONFIG=release install PREFIX=/usr`).
- **Theme not found after install** — the faces live in
  `/usr/share/fdk/.FDKThemes` and are discovered through the default
  `$XDG_DATA_DIRS`; check yours still includes `/usr/share`.
- **The X11 suite fails under a strange environment** — the suite
  starts its own Xvfb; make sure `Xvfb` is installed and `/tmp` is
  writable. Under a container without Xvfb the packaging scripts
  skip it with an `XVFB-SKIP` line and say so.
- **Reproducibility** — the package builds are not byte-reproducible
  (archives carry timestamps); the source `git archive` tarball is.
  Reproduce any package with the `packaging/ci/<distro>.sh` scripts.
