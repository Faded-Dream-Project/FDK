# FDK Packaging

This document covers everything around distributing FDK: building it
from source, what the per-distribution packaging recipes produce, and
how CI keeps those recipes proven on every push. **The project is in
development — nothing is published**: there are no binary packages,
no releases page, no tags; the packaging pipeline exists as a
validation check so the recipes can never silently rot.

> **What should I build from?** `main` — the only branch. The
> project is in development with no stable release yet; build from
> source (`docs/build.md`, the authoritative reference) at whatever
> commit you care to pin.

> **The release-posture guard** (`fdk_require_release_posture`) stays
> as infrastructure: while the public version carries a pre-release
> suffix (as it does now), packaging discloses a `PACKAGING-SKIP`
rather than staging an inconsistent payload — the posture the whole
> project is in.

- Repository home: https://github.com/Faded-Dream-Project/FDK
- Build from source: `docs/build.md` (the authoritative reference)

## The packaging pipeline in one paragraph

FDK's packaging recipes build **native binary packages** (.deb, .rpm,
.pkg.tar.zst, .apk, .xbps) on GitHub-hosted runners, each target built
**inside its own pinned distribution container** and **install-tested
in clean containers** of every supported target — as a CI validation
check on every push to `main`, never as a publish step. Nothing is
uploaded as a release; the artifacts exist to prove the recipes still
work.

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

## What the packages install (per-family reference)

*(No published packages exist — this section is the reference for
what the CI-validated recipes in `packaging/` produce and what a
from-source `make install` lays down. Should a release ever be cut,
the same layout applies to it.)* The per-family install commands, for
completeness, assuming locally built artifacts in `dist/` verified
against a `SHA256SUMS` you generated yourself; `<ver>` is the
version the artifacts carry.

### Debian / Ubuntu (.deb)

```sh
sudo apt install ./libfdk1_<ver>-1_amd64.deb ./fdk-dev_<ver>-1_amd64.deb
```

Runtime package `libfdk1`, development package `fdk-dev`. Built on
Debian 12 (bookworm, glibc 2.36); install-tested on Debian 12,
Debian 13, Ubuntu 24.04, and Ubuntu 22.04 (glibc 2.35 — verified by
the pipeline's informational probe, which passes). Anything older
(glibc < 2.35) is below the build floor — build from source there.
`apt` resolves the runtime dependencies (libx11-6, libxext6,
libxrandr2, zlib1g, libwayland-client0, libxkbcommon0)
automatically.

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

`libX11`, `libXext`, `libXrandr` (the multi-monitor output model —
a direct link dependency since M-6), `zlib`, and — when the Wayland
backend is compiled in, which the packaging recipes always do —
`libwayland-client` and `libxkbcommon`. Every package manager above
resolves them automatically. The GPU present lane loads `libGL`
dynamically when a hardware GL renderer exists and silently stays on
the CPU present path otherwise (`docs/rendering.md`); no GL package
is required. FDK performs its own font discovery (DejaVu Sans or a
similar default UI font is picked up automatically; override with
`$FDK_FONT_FILE` / `$FDK_FONT_DIRS` — see `docs/text.md`).

## Pre-release versions in package metadata

While the version string carries a hyphenated pre-release suffix —
`1.1.0-rc2` today — the five package grammars disagree about that
suffix, and four of them reject it outright, so the pipeline
translates it once, at the metadata layer (`fdk_packver` in
`packaging/ci/common.sh`):

| Family | Raw `1.1.0-rc1` becomes | Why |
| --- | --- | --- |
| deb | `1.1.0~rc1` | dpkg accepts `1.1.0-rc1` but orders it **above** the future `1.1.0` final (the hyphen starts the debian revision); `~` is dpkg's native pre-release marker and sorts before the final |
| rpm | `1.1.0~rc1` | `-` is the Version/Release separator — `Version: 1.1.0-rc1` is an illegal spec (`Illegal char '-'`); `~` is rpm's pre-release marker |
| apk | `1.1.0_rc1` | `-` is reserved for the `-rN` pkgrel; `_rc` is apk's native pre-release suffix family |
| arch | `1.1.0rc1` | `pkgver` forbids hyphens entirely |
| xbps | `1.1.0rc1` | versions must not contain dashes, underscores, or special chars (void-packages Manual); a dash also makes the pkgname-version split ambiguous, which broke repository lookups |

Two scope rules keep this honest. First, the mapping touches
**package metadata only** — the `Version` fields, the package
filenames, and the tarball/top-dir names the metadata-driven builds
derive (rpm `%setup`, makepkg `$srcdir`, abuild `builddir`). The
`.so` real-file name is **numeric-only** (`libfdk.so.1.1.0`, from
the Makefile's `FDK_SO_VERSION` — pre-release suffixes never appear
in the .so file name, both because that is the GNU convention and
because Alpine's abuild derives the apk `so:`provides version from
the filename tail, where a suffixed tail makes `apk index` reject
the whole package — reproduced and pinned locally), while the
pkg-config `Version` and the smoke test keep the **raw** string,
byte-identical to what `fdk_get_version_string()` reports — a
package must never claim a version the library itself does not.
Second, a plain numeric version passes through every grammar
unchanged, so were a final numeric release ever cut, all five
spellings would be identical and the mapping would be inert.

## What the packages contain (and deliberately omit)

Every package family ships:

- **runtime**: `libfdk.so.1` (the SONAME; real file
  `libfdk.so.<version>`) and the one shipped `.fdk` theme face —
  `faded-dream.fdk`, the packaged copy of the built-in default — in
  `/usr/share/fdk/.FDKThemes` (the 1.0.0 alignment: additional
  faces are user-installed `~/.FDKThemes` drop-ins, not package
  content)
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

Licensing note: the packaging recipes exist so that, should the
Licensor ever publish binary packages through the official
repository, the channel is the one authorized under the license's
Section 4(d) — built and published by the Licensor. Applications
built with FDK remain their authors' property (license Section 5);
nothing in the packaging claims otherwise.

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
validated, so none is claimed or produced. Do not assume the x86_64
builds run anywhere other than the distributions listed above —
the glibc/musl floors are real (the Debian packages need glibc ≥
2.36; the RPMs glibc ≥ 2.38).

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
these scripts — `ci.yml` on pushes/PRs, as validation checks —
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

## Version bumps (the discipline, for completeness)

A version bump remains an explicit owner decision recorded first in
`docs/versioning.md`: update `include/fdk/fdk_version.h`
(major/minor/patch + string) and the pin in
`tests/test_core.c::test_version`. The Makefile, pkg-config file,
package versions, and SONAME all derive from the header
automatically. If the ABI breaks, the SONAME major moves with
`FDK_VERSION_MAJOR` — nothing else to do. There is no release
runbook because there are no releases: publishing one would be a
new owner decision, recorded in `docs/versioning.md` first, and
would come with its own process written then.

Workflow permissions are minimal: everything runs on the ephemeral
`GITHUB_TOKEN` with `contents: read`. No personal token is stored
anywhere in the repository or workflows.

## Signing

Packages are **not GPG-signed** — nothing is published, and should a
release ever exist it would carry a `SHA256SUMS` file served over
HTTPS alongside the CI provenance (the run that built it) as its
integrity story. Per-distribution signing (a Debian apt repository,
an RPM repository with GPG keys, apk keys) would require maintained
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
- **Wrong architecture** — the packaging matrix builds x86_64 only;
  on arm64 build from source (`make FDK_CONFIG=release install
  PREFIX=/usr`).
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
