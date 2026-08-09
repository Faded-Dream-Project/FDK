# Faded Dream ToolKit (FDK)

FDK is a native C GUI toolkit being built for Artix Linux, targeting
the practical role GTK and Qt currently serve for applications that
choose to target it. Pure C17, minimal dependencies, no GTK/Qt
dependency, real X11 and Wayland backends, and its own `.fdk` theme
format (coming in Phase 6). See `docs/roadmap.md` for the full project
plan and current status.

**Status: Phase 2 — Platform Layer complete.** Core lifecycle, a real
X11 backend, and a real Wayland backend are implemented and tested.
Applications can create windows, show them, resize them, and receive
real translated keyboard/pointer/configure/close events on both
backends. There is no rendering, widget system, or window decoration
system yet — see "What works today" below for exactly what that means
in practice, and `docs/roadmap.md`'s Phase 2 entry for an honest,
specific list of what is and isn't covered.

## Requirements

- GCC with C17 support (GNU Make + GCC 13+ on Artix Linux; see
  `docs/build.md`)
- X11 and Wayland development headers/tools — see `docs/dependencies.md`
  for exact packages, purposes, and licenses
- `Xvfb`, optionally, only if you want to run `make test-x11` without
  an existing desktop session

## Building

```sh
make            # debug build (ASan+UBSan on by default)
make test       # platform-independent test suite (no display needed)
make test-x11   # X11 integration test suite (real window lifecycle,
                # auto-starts a throwaway Xvfb if $DISPLAY isn't set)
make examples   # build the example programs
```

See `docs/build.md` for the full command reference, including release
builds and `make install`.

## What works today

```c
#include "fdk/fdk.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_window.h"

static void on_event(fdk_window *window, const fdk_event_data *event, void *user_data) {
    fdk_context *ctx = user_data;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdk_window_destroy(window);
        fdk_quit(ctx);
    }
}

int main(void) {
    fdk_context *ctx = NULL;
    fdk_init(&ctx, NULL); /* connects to a real X11 or Wayland display */

    fdk_window *window = NULL;
    fdk_window_options opts = { .title = "Hello", .width = 640, .height = 480 };
    fdk_window_create(ctx, &opts, &window);
    fdk_window_set_event_callback(window, on_event, ctx);
    fdk_window_show(window);

    fdk_run(ctx); /* real poll()-based event loop */
    fdk_shutdown(ctx);
    return 0;
}
```

Run `examples/01_hello_world.c` (via `make examples`) to see a fuller
version of this — it opens a real window, logs resize/keyboard events,
and exits cleanly when closed. It needs a reachable X11 or Wayland
display to run.

There is no renderer yet (Phase 3), so a created window shows whatever
blank/undefined content the platform gives an unbuffered surface —
the window, its events, and its lifecycle are all real; there's just
nothing drawn inside it.

## Project principles

- **No GTK, no Qt, no wrapping either.** FDK implements its own
  widget system, rendering, layout, event handling, and window
  decorations. The X11 and Wayland backends talk directly to Xlib and
  libwayland-client — the two explicitly project-permitted platform
  interfaces — not through any intermediate toolkit.
- **Minimal dependencies, always justified.** Every dependency FDK
  takes on is documented in `docs/dependencies.md` before it's added,
  with license, purpose, and whether it's optional.
- **No copyleft, anywhere in the dependency graph.** See
  `docs/licensing-policy.md`.
- **Correct over quick; architecture over feature-count.** See
  `docs/roadmap.md`'s phase structure — each phase is meant to leave a
  working, tested foundation for the next, not a pile of stubs.
- **No fake completion.** Phase status in `docs/roadmap.md` lists what
  is NOT covered as carefully as what is — e.g. Phase 2 has no
  automated Wayland integration test yet, and says so plainly rather
  than claiming "Wayland support" without qualification.

## Documentation

| Doc | Covers |
|---|---|
| `docs/architecture.md` | Layering, module boundaries, public/internal header split |
| `docs/roadmap.md` | Phase-by-phase plan and current status |
| `docs/build.md` | Build system reference |
| `docs/testing.md` | The two-tier test suite, what each covers, and known environment quirks |
| `docs/memory.md` | Ownership model, allocation policy |
| `docs/threading.md` | UI-thread affinity, worker-thread rules |
| `docs/abi-policy.md` | Current (pre-1.0) ABI stance and the post-1.0 policy |
| `docs/dependencies.md` | Every current and anticipated dependency, with justification |
| `docs/licensing-policy.md` | What licenses are/aren't allowed in, and the audit procedure |

## License

FDK is proprietary software. See `LICENSE` — note that it is currently
a draft flagged for legal review, not a finalized license.
