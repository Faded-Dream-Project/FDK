# Faded Dream ToolKit (FDK)

FDK is a native C GUI toolkit being built for Artix Linux, targeting
the practical role GTK and Qt currently serve for applications that
choose to target it. Pure C17, minimal dependencies, no GTK/Qt
dependency, X11 and Wayland support, and its own `.fdk` theme format.
See `docs/roadmap.md` for the full project plan and current status.

**Status: Phase 1 — Foundation.** Core lifecycle, error handling,
logging, and build system are implemented and tested. There is no
windowing, rendering, or widget system yet — see "What works today"
below for exactly what that means in practice.

## Requirements

- GCC with C17 support (GNU Make + GCC 13+ on Artix Linux; see
  `docs/build.md`)
- No third-party dependencies yet (`docs/dependencies.md`)

## Building

```sh
make            # debug build (ASan+UBSan on by default)
make test       # build and run the test suite
make examples   # build the example programs
```

See `docs/build.md` for the full command reference, including release
builds and `make install`.

## What works today

```c
#include "fdk/fdk.h"

int main(void) {
    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        /* handle failure */
    }

    fdk_run(ctx);       /* returns immediately for now — no platform
                            layer/event loop yet, see docs/roadmap.md */
    fdk_shutdown(ctx);
    return 0;
}
```

Run `examples/01_hello_world.c` (via `make examples`) to see this
actually build and execute. Init/shutdown, logging, error codes, and
allocation are real and tested — windows and widgets are not yet
implemented.

## Project principles

- **No GTK, no Qt, no wrapping either.** FDK implements its own
  widget system, rendering, layout, event handling, and window
  decorations. See the full requirements this project is being built
  against for the complete list of what's excluded and why.
- **Minimal dependencies, always justified.** Every dependency FDK
  takes on is documented in `docs/dependencies.md` before it's added,
  with license, purpose, and whether it's optional.
- **No copyleft, anywhere in the dependency graph.** See
  `docs/licensing-policy.md`.
- **Correct over quick; architecture over feature-count.** See
  `docs/roadmap.md`'s phase structure — each phase is meant to leave a
  working, tested foundation for the next, not a pile of stubs.

## Documentation

| Doc | Covers |
|---|---|
| `docs/architecture.md` | Layering, module boundaries, public/internal header split |
| `docs/roadmap.md` | Phase-by-phase plan and current status |
| `docs/build.md` | Build system reference |
| `docs/memory.md` | Ownership model, allocation policy |
| `docs/threading.md` | UI-thread affinity, worker-thread rules |
| `docs/abi-policy.md` | Current (pre-1.0) ABI stance and the post-1.0 policy |
| `docs/dependencies.md` | Every current and anticipated dependency, with justification |
| `docs/licensing-policy.md` | What licenses are/aren't allowed in, and the audit procedure |

## License

FDK is proprietary software. See `LICENSE` — note that it is currently
a draft flagged for legal review, not a finalized license.
