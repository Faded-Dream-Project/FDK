# Building FDK

## Requirements

- GCC with C17 support (developed against GCC 13; any reasonably
  current GCC should work — FDK is distro-agnostic and is not built
  around any specific distribution's toolchain)
- GNU Make
- X11 development headers (`libx11-dev` on Debian/Ubuntu,
  `xorg-x11proto-devel`/`libX11-devel` on Arch/Fedora, etc.) —
  always required; X11 is FDK's baseline backend and the runtime
  auto-detection's fall-through when Wayland is unavailable
- Optional: Wayland development headers (`libwayland-dev`,
  `wayland-protocols`, `libxkbcommon-dev` on Debian/Ubuntu and most
  others) — auto-detected at build time; if absent, FDK builds as
  X11-only and the runtime `FDK_PLATFORM_WAYLAND` selection fails
  cleanly with `FDK_ERR_NO_DISPLAY` rather than crashing
- `Xvfb`, only if you want to run `make test-x11` without an existing
  desktop session (optional — `make test` never needs it)

## Commands

```sh
make            # debug build: libfdk.a + libfdk.so, ASan+UBSan enabled
make release    # optimized build (-O2 -DNDEBUG), no sanitizers
make static     # libfdk.a only
make shared     # libfdk.so only
make test       # build and run the platform-independent test suite
                # (no display required — safe for any CI, see docs/testing.md)
make test-x11   # build and run the X11 integration test suite
                # (uses $DISPLAY if set, otherwise auto-starts/stops a
                # throwaway Xvfb — requires Xvfb to be installed)
make examples   # build example programs, linked against libfdk.a
make bench      # build and run the performance baseline harness
                # (release objects, no sanitizers; see docs/performance.md)
make install    # install headers + both libraries + fdk.pc
                # (PREFIX=/usr/local by default)
make uninstall  # remove what `install` put there
make clean      # remove build/ entirely
```

Override variables on the command line, e.g.:

```sh
make release PREFIX=/usr
make CC=clang
```

### Linking an application

After `make install`, applications find FDK through pkg-config:

```sh
cc myapp.c $(pkg-config --cflags --libs fdk) -o myapp
```

The installed `fdk.pc` carries the platform dependencies the build
resolved (X11, and Wayland/xkbcommon when enabled) as
`Requires.private` — the shared library needs nothing extra from the
app, and static linking pulls the transitive set in automatically.
The version in `fdk.pc` is generated from `include/fdk/fdk_version.h`
at install time, so it can never drift from the headers.

For building against the tree without installing, compile with
`-Iinclude` and link `build/libfdk.a` (plus `-lX11 -lXext -lm`, and
the Wayland libs when built with Wayland enabled) — the same flags
the Makefile computes.

## Optional Wayland build

By default (`make` with no knobs), FDK auto-detects Wayland dev
availability via `pkg-config`:

- If `wayland-client` AND `xkbcommon` are both found → the Wayland
  backend is built and `FDK_PLATFORM_WAYLAND` works at runtime.
- If either is missing → the Wayland backend is silently skipped,
  `src/platform/wayland_disabled.c` is compiled in its place (it
  returns `NULL` from `fdk_platform_wayland_ops()` and 0 from
  `fdk_platform_wayland_display_present()`), and at runtime
  `FDK_PLATFORM_AUTO` falls through to X11 while
  `FDK_PLATFORM_WAYLAND` fails cleanly with `FDK_ERR_NO_DISPLAY`.
  No link dependency on `libwayland-client` / `libxkbcommon` is
  produced in this configuration.

This is the build that runs by default in environments without
Wayland dev headers — for example, a minimal CI container, or any
distribution whose base install doesn't pull in Wayland development
files. FDK should build cleanly there; this is what makes the project
genuinely distro-agnostic rather than implicitly requiring a
Wayland-rich environment.

Two overrides exist for cases where the build's intent should be
explicit rather than inferred:

```sh
make FDK_DISABLE_WAYLAND=1   # never build Wayland, even if pkg-config finds it
make FDK_ENABLE_WAYLAND=1   # require Wayland — hard error if missing,
                             # rather than silently building X11-only
```

Use `FDK_DISABLE_WAYLAND=1` when building for a system that
specifically does not want Wayland support linked in (e.g. an
embedded target where Wayland's runtime dependencies are
unavailable). Use `FDK_ENABLE_WAYLAND=1` in CI configurations where
a silent X11-only fallback would mask a real packaging regression
that removed Wayland dev headers from the image.

## Why debug builds default to ASan+UBSan

Per project principle ("do not fake completion," `docs/memory.md`), a
test suite that passes but leaks memory or triggers undefined behavior
isn't actually passing. Sanitizers are on by default specifically so
that's caught locally, every time, without needing a separate CI-only
configuration someone forgets to run. `make release` drops them for
the shipped artifact, where their runtime cost isn't acceptable.

## Why static and shared builds use separate object directories

`build/obj/` holds plain objects (used by `libfdk.a`); `build/obj-pic/`
holds `-fPIC` objects (used by `libfdk.so`). Static library objects
don't need to be position-independent, and giving them a separate tree
means running `make static` followed by `make shared` (or `make all`,
which does both) can never silently link stale non-PIC objects into
the `.so` — an early version of this Makefile had exactly that bug
during initial bring-up (a `CFLAGS += -fPIC` override applied to the
target didn't force prerequisite objects to be recompiled with it),
caught by `make release` failing at link time with a relocation error.
Kept here as the reason, not just the mechanism, in case anyone is
tempted to "simplify" this back to one object tree.

## Warning policy

The build compiles with `-Wall -Wextra -Wpedantic -Wshadow
-Wstrict-prototypes -Wmissing-prototypes -Wconversion
-Wsign-conversion -Wcast-qual -Wpointer-arith -Wundef -Wwrite-strings`.
Per project principle, warnings get fixed, not suppressed — if a
warning flag is ever removed from this list, that removal itself
should be justified in the commit that does it.

The single exception is the Wayland backend's own translation units,
which get `-Wno-cast-qual` applied via `extra_flags` only when their
source path is under `src/platform/wayland/`. The reason is
`wayland-scanner`'s generated protocol headers (xdg-shell and the
Phase 8 xdg-decoration-unstable-v1) and
`libwayland-client`'s own listener-registration inlines
(`wl_proxy_add_listener`'s `(void (**)(void))` cast) trigger that
warning upstream, in code FDK does not own or control. No other
warning is suppressed anywhere in the project, and no source file
outside `src/platform/wayland/` receives any `-Wno-...` flag.

The scanner output lives in
`src/platform/wayland/generated/` and is CHECKED IN (both protocols
are stable enough that regenerating per build buys nothing and costs
a build-time wayland-scanner dependency). Regenerating, if ever
needed:

    wayland-scanner client-header < xdg-decoration-unstable-v1.xml         > src/platform/wayland/generated/xdg-decoration-unstable-v1-client-protocol.h
    wayland-scanner private-code < xdg-decoration-unstable-v1.xml         > src/platform/wayland/generated/xdg-decoration-unstable-v1-protocol.c

## Why debug and release share object *paths* but never objects (the config stamp)

The same class of bug as the obj/obj-pic split above, found live in
1.3.4: `make release && make` used to produce a silently MIXED
binary. The old `release:` target was a target-specific
`CFLAGS := ... -O2 -DNDEBUG` override over the same `build/obj/`
paths — and make's up-to-date check compares timestamps only, so the
release objects (freshly written, newer than every source) looked
"up to date" to the next plain `make`, which then linked them into
binaries compiled with `-fsanitize=address`. The observable was
subtle and nasty: LeakSanitizer reported fontconfig's
process-lifetime allocations as leaks in the compositor-death rig
(not because anything leaked — because the `__lsan_disable` bracket
in `src/text/fontscan.c` compiles to NOTHING without the sanitizer,
so the release objects never marked those allocations as ignored),
and LSan's exit path (`_exit(23)` on report) also swallowed the
example's buffered "exited cleanly" line, failing the rig on a
build-state artifact.

The fix is the configuration stamp: `make release` re-invokes make
with `FDK_CONFIG=release`, every object depends on
`build/.config-<mode>-wl<0|1>`, and the stamp rule rewrites itself
(making it newer than every object, forcing a full rebuild) only
when the configuration actually changed. The key carries BOTH axes
that change object contents — debug/release and Wayland on/off —
because the second axis is environment-derived (pkg-config
visibility of libwayland-dev/libxkbcommon-dev) and can flip without
any source change; a rig or environment reset that hides the
toolchain would otherwise leave the enabled-backend objects "up to
date" inside a build that no longer contains them (the 1.3.7 find:
sway-rig apps linked against the disabled stub ignored a valid
`WAYLAND_DISPLAY`). Same-config builds keep normal
incremental behavior — the stamp rule runs on every invocation but
is a no-op unless the mode name differs, so its mtime only moves on
a real switch. The stamps live under `build/` and vanish with
`make clean`.

Two practical consequences worth knowing: switching configurations
costs one full rebuild (correct, not a regression — the alternative
is linking the wrong objects), and "the binary in `build/` is
whatever I last asked for" is now actually true. If you need both
configurations side by side, build one, copy the artifact out, and
build the other; the project deliberately has one active
configuration per tree rather than two build directories, because
every rig script addresses `build/tests/...` and `build/examples/...`
by path.

## Why the static archive is removed before re-archiving

The third member of the same bug family, found live in 1.3.7:
`ar rcs` REPLACES the members it is handed but never DROPS members
that fell out of the list. When a build configuration switch removes
objects (the Wayland toolchain disappearing from the environment
flips `BUILD_WAYLAND` to 0, and `wayland_*.o` leaves `LIB_OBJS`),
the stale objects stay inside `libfdk.a` next to the fresh
`wayland_disabled.o` — and every subsequent link resolves symbols
from a backend the current build no longer contains, against
libraries that may not even be installed anymore. The link fails
far from the cause (an `undefined reference to wl_proxy_get_version`
from `wayland_seat.o` nobody asked for).

The fix: the `$(STATIC_LIB)` rule does `rm -f $@` before
`$(AR) rcs`, so the archive is always exactly the current member
list. Together with the config stamp above, this closes the whole
family: objects can no longer outlive the configuration that
produced them, whether through shared paths (1.3.4) or through
archive accumulation (1.3.7).

## Why the shared library links with a generated version script

Without a version script, a shared library exports every global
object linked into it. FDK's `.so` linked that way through 1.3.8:
787 symbols under the debug build, of which 441 are the public
API — the rest was backend internals (`fdk_x11_*`,
`fdk_wayland_*`), cross-module seams (`fdk__*`), the vendored stb
symbols (`stbi_*`/`stbtt_*`), the wayland-scanner interface
structs, and ASan's `__odr_asan.*` instrumentation aliases.
Surface noise is the smaller problem: it is an INTERPOSITION
hazard. An application that vendors its own stb_image gets FDK's
internal `stbi_*` calls bound to the app's copy (or vice versa) by
the dynamic linker — two stb versions silently mixed across a DSO
boundary.

The `.so` therefore links with
`-Wl,--version-script=$(BUILD_DIR)/libfdk.exports.map`, and the
map is GENERATED at build time, never committed. The export list
is the INTERSECTION of two sets that each regenerate from source
on every build: the names declared in `include/fdk/*.h` (the
generator greps the `fdk_name(` token, so a public function's
name and its opening paren must sit on ONE line — return types
and parameter lists may wrap freely; every header already follows
this) and the globals the static archive actually defines (`nm`
over `libfdk.a`). Intersection, not union: header mentions that
are not linkable symbols (typedefs like `fdk_plural_category`,
static-inline helpers like `fdk_ok`) drop out on the archive
side; archive globals that are internal drop out on the header
side. Because both sides come from source, the exported surface
cannot drift from the headers the way a committed hand-edited
list would.

`make verify-exports` is the belt-and-braces check that the
generator's one blind spot cannot hide: it reads the ACTUAL
dynamic symbol table of the linked `.so` (not the map file),
requires every export to be `fdk_`-prefixed, and walks every
`fdk_*` archive global that is NOT exported to assert it is not
mentioned in `include/fdk` at all — a declaration the grep missed
(say, a name and paren wrapped across lines) is
public-but-unexported, a real bug, and the check fails loudly.
Run it alongside the builds in both configurations: the archive
differs per config (ASan instrumentation symbols exist only in
debug), but the exported surface must not.
