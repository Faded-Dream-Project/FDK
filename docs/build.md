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
