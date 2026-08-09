# Building FDK

## Requirements

- GCC with C17 support (developed against GCC 13; any reasonably
  current GCC on Artix Linux should work)
- GNU Make
- X11 development headers (`libx11-dev`/`xorg-x11proto-devel`
  equivalent on Artix) and Wayland development headers/tools
  (`libwayland-dev`, `wayland-protocols`, `libxkbcommon-dev`
  equivalent) — see `docs/dependencies.md` for exact package purposes
  and licenses
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
make install    # install headers + both libraries (PREFIX=/usr/local by default)
make uninstall  # remove what `install` put there
make clean      # remove build/ entirely
```

Override variables on the command line, e.g.:

```sh
make release PREFIX=/usr
make CC=clang
```

## Why debug builds default to ASan+UBSan

Per project principle ("do not fake completion," `docs/memory.md"), a
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
