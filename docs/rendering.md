# FDK Rendering (Phase 3)

This document describes FDK's software rendering layer: the public
`fdk_surface` API (`include/fdk/fdk_surface.h`), the damage-tracking
model that makes partial redraws correct, the clip stack, offscreen
surfaces, frame pacing, and how the X11 and Wayland backends present
pixels without any backend type leaking into application code.

Everything here is implemented and tested — headless in
`tests/test_render.c` (`make test`, no display needed, thanks to
offscreen surfaces) and against real display servers in
`tests/test_x11_integration.c` (`make test-x11`) and the weston-based
Wayland verification (see `docs/testing.md`).

## The frame model

A rendered FDK application owns its loop:

```c
while (running) {
    fdk_pump_events(ctx, 15);          /* input, resizes, close      */
    if (!fdk_surface_frame_ready(surface))
        continue;                      /* compositor-paced (Wayland) */
    /* draw with helpers and/or raw pixels */
    fdk_surface_present(surface);      /* only the damage is sent    */
}
```

Three properties define the model:

1. **Draw into the surface** — `fdk_surface_get_info()` hands back the
   pixel pointer, dimensions, and stride (in `fdk_u32` units, `>=`
   width). The layout is XRGB8888 (`R<<16 | G<<8 | B`, top byte
   ignored) on both backends, so no per-pixel conversion happens
   anywhere in FDK. The pointer is valid until the next
   `get_info`/`present` — reacquire every frame.
2. **Damage is recorded as you draw** — every drawing helper adds the
   region it touched to the surface's damage region. Applications
   writing pixels directly through `info.pixels` must call
   `fdk_surface_invalidate(rect)` themselves; a raw write without
   damage declaration is *documented to not reach the screen* (there
   is an integration test that proves it: the pixel sits in the
   framebuffer, the server keeps the old value, until the application
   declares the damage).
3. **Present sends only the damage** — and is a true no-op when
   nothing changed. On X11 that means one sub-image `XPutImage` per
   damaged rectangle (a >=75%-damaged frame switches to a single
   whole-image put — one request beats dozens of overlapping ones).
   On Wayland it means per-rect `wl_surface.damage` hints before the
   commit — and no commit at all for an unchanged frame.

A resize changes the contract in one way: a framebuffer re-acquired
at a NEW size has undefined content, so the damage region resets to
"everything" — the application's next frame after a resize must
repaint the whole surface. (`FDK_EVENT_WINDOW_CONFIGURE` / resize
detection: apps typically set a `needs_full_redraw` flag, exactly as
`examples/02_software_render.c` does.)

## Damage tracking internals

- The damage region is a bounded list of at most
  `FDK_SURFACE_MAX_DAMAGE` (64) half-open rects in surface-local
  pixel coordinates, plus an "everything" flag. Overflow degrades to
  full damage — correctness (every changed pixel must reach the
  screen) is never traded for the bookkeeping bound.
- `fdk_surface_get_damage_bounds()` reports the union bounding box —
  what a partial-redraw app checks to decide how much of a cached
  scene to re-render.
- A successful `present()` consumes the damage region. A failed one
  retains it, so a retry re-presents the same region.

### Why Wayland partial damage is correct, not just fast

The Wayland protocol says compositors may *ignore* damage hints and
scan out the whole committed buffer. A fresh buffer therefore cannot
carry garbage outside the damaged region — a compositor that ignores
the hints would display it. FDK guarantees this by construction
(`src/platform/wayland/wayland_window.c`):

- wl_shm buffers are **recycled**, not destroyed: a
  `wl_buffer::release` marks the slot reusable and keeps the buffer
  and its client mapping alive.
- Every buffer handed to the application (recycled or fresh) is
  **pre-filled with a copy of the currently visible frame**
  (`prefetch_visible_frame`). The next frame then differs from the
  screen exactly where the application draws — which is precisely
  what the damage hints claim, whether or not the compositor honors
  them.

Measured effect (weston 14 headless, `02_software_render`): ~1 shm
pool per frame before recycling; 3 pools for 173 commits after —
buffers are created at startup and on resize, nothing per frame.

## The clip stack

```c
fdk_surface_push_clip(surface, clip);   /* intersected with current */
/* ... all drawing constrained to the intersection ... */
fdk_surface_pop_clip(surface);
```

- Pushing **never expands**: the pushed rect is intersected with the
  clip below it; an empty intersection stays on the stack and makes
  every drawing helper a safe no-op until it is popped.
- Depth is bounded (`FDK_SURFACE_CLIP_DEPTH`, 32); pushing deeper
  returns `FDK_ERR_INVALID_ARGUMENT` without touching the stack.
  Popping an empty stack is a documented defensive no-op.
- `fdk_surface_get_clip()` returns the effective clip — the
  intersection of all pushed clips, *not* clamped to the surface —
  which is what geometry-composing callers want.
- The clip applies to every primitive and to `fdk_surface_blit`'s
  destination side. This is the constraint primitive the widget layer
  will use to keep children inside their parents.

## Primitives

All helpers blend source-over with straight (non-premultiplied)
alpha — `a = 1.0` replaces, `a < 1.0` tints. All are clipped to the
clip stack and record damage automatically. All are crisp integer
geometry (no antialiasing yet — that is deliberate, listed in
`docs/roadmap.md` as future work).

| Helper | Algorithm / notes |
|---|---|
| `fill` | whole (clip-visible) surface; memset-fast path for opaque colors |
| `fill_rect` | clipped span fill |
| `draw_rect` | 1px border; edges strictly between corners so **no pixel blends twice** |
| `fill_gradient_vertical` | per-row lerp in the *original* rect space so clipping doesn't skew it |
| `draw_line` | Bresenham, all octants; endpoints inclusive |
| `draw_circle` | per-row chord ends, 8-way symmetric (midpoint-equivalent) |
| `fill_circle` | scanline chords `sqrt(r^2 - dy^2)` per row |
| `fill_rounded_rect` | corner-band rows widen by the chord toward the middle; radius clamped to half the shorter side |
| `draw_rounded_rect` | straight edges + four exact quarter arcs, each pixel plotted once |
| `blit` | opaque surface-to-surface copy; clipped on source AND destination (bounds + clip stack); damages the destination |

`draw_rect`'s corners and `draw_rounded_rect`'s arcs are plotted
exactly once each — with translucent colors a double blend is a
visible artifact, not a style choice, and the headless tests pin the
single-blend behavior by exact pixel comparison.

## Offscreen surfaces

```c
fdk_surface *sprite = NULL;
fdk_surface_create(64, 64, &sprite);      /* application-owned */
/* draw anything into it */
fdk_surface_blit(window_surface, x, y, sprite, full_rect);
fdk_surface_destroy(sprite);
```

- No display connection is required to create, draw, read, or destroy
  them — which is also why the entire renderer is testable in
  `make test` on a bare CI box.
- Their stride is deliberately padded to 16-pixel multiples so the
  stride-aware paths real backends exercise are always taken in
  tests too.
- `present()` on an offscreen surface is a no-op that resets the
  damage region (the "frame close" half of the frame model);
  `fdk_surface_blit` is how its content travels.
- Ownership is inverted relative to window surfaces: the application
  owns offscreen surfaces and destroys them explicitly; window
  surfaces belong to their window (`fdk_surface_destroy` on a window
  surface is refused with a logged warning, not a crash).

Typical uses today: sprite caches and expensive static compositions.
Future use: the widget layer's rendering (widgets paint into
offscreen surfaces; the window composites them via blit + clip).

## Frame pacing

`fdk_surface_frame_ready(surface)` answers "may I draw the next
frame now?":

- **Wayland**: every present requests a `wl_surface.frame` callback
  after its commit. `frame_ready()` is false until that callback
  arrives (it is delivered while the application pumps events — the
  contract that makes `pump`-then-`ready`-then-`draw` the right loop
  shape), or until a 250 ms guard interval elapses. The guard exists
  because hidden surfaces legitimately never receive frame
  callbacks: FDK paces, it never starves.
- **X11**: always true — the core protocol has no per-frame
  compositor feedback (X11 presentation feedback would be an
  extension-level feature; see roadmap).
- **Offscreen**: always true.

This keeps a rendered app from rendering faster than the display
consumes frames — the Wayland demo's commit timestamps space at the
compositor's repaint clock rather than the application's loop rate.

## Backend seam

The render layer talks to backends through two optional
`fdk_platform_ops` entries plus one optional pacing query (see
`src/platform/platform_internal.h`):

- `window_get_framebuffer` — returns the window's CPU-drawing buffer,
  recreating it on size changes.
- `window_present(pwindow, damage)` — presents only the damaged
  region. The `fdk_platform_damage` struct carries up to 64
  unclamped rects or a "full" flag; backends clamp to their real
  bounds. Backends may coarsen granularity but must never present
  garbage outside the damage as new content (the Wayland prefill rule
  above is how that backend meets the contract).
- `window_frame_ready` (optional) — the pacing query; NULL means
  "always ready".

A backend with no software framebuffer (a hypothetical GPU-only
backend) leaves the two render ops NULL and the public API reports
`FDK_ERR_UNSUPPORTED` — nothing pretends to work.

## What is deliberately not here yet

Honest gaps, tracked in `docs/roadmap.md`'s Phase 3 section: no
MIT-SHM fast path on X11 (per-damage-rect `XPutImage` is a memcpy —
fine locally, worth replacing eventually and required for remote
X), no X11 double-buffering, no transforms, no image decoding, no
alpha-masked (as opposed to opaque) blits, no antialiased
primitives, no text (`src/text/` is future work), and no HiDPI
buffer-scale handling. Each of those has a designed-in place to land
without reshaping this layer.
