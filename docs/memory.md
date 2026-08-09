# FDK Memory Management

## Ownership model

FDK uses explicit ownership, not implicit reference counting or
garbage collection, as the default model:

- Every `fdk_*_create()` function returns an object the caller owns.
- Every ownable object has a matching `fdk_*_destroy()` (or, for the
  root context, `fdk_shutdown()`) that the owner must call exactly
  once when done.
- Destroying a container-like object (once containers exist, from
  Phase 4 onward) destroys everything it owns — e.g. destroying a
  `fdk_context` destroys any windows still open on it, per
  `fdk_shutdown()`'s documented behavior in `fdk_core.h`.
- Passing an object to a "child of" relationship (e.g. adding a widget
  to a container, once that exists) transfers ownership to the parent
  unless the API explicitly documents otherwise. This will be called
  out per-function as those APIs land.

Reference counting is not ruled out for specific cases where shared
ownership is the only sane model (candidate: theme objects shared
across many widgets, in Phase 6), but it is not the default — per
project principle, FDK does not build a general object framework just
because GLib/Qt have one.

## Internal allocation

All internal heap allocation goes through `src/core/alloc_internal.h`
(`fdk_alloc`, `fdk_alloc_array`, `fdk_realloc`, `fdk_free`) rather than
calling `malloc`/`calloc`/`realloc`/`free` directly from arbitrary
`.c` files. This is not part of the public API. Reasons:

1. **One place to handle allocation failure.** FDK never calls
   `abort()` or `exit()` on OOM — allocation failures are logged and
   `NULL` is returned, and callers propagate `FDK_ERR_OUT_OF_MEMORY`
   up through the `fdk_result` system. An application embedding FDK
   gets to decide how to react to memory pressure, not have FDK decide
   for it.
2. **`fdk_alloc`/`fdk_alloc_array` zero-initialize.** This eliminates
   an entire class of "forgot to initialize a field" bugs in a
   toolkit with many small structs, at the cost of a `calloc` instead
   of `malloc` (fine for FDK's allocation sizes and frequency; revisit
   only if profiling says otherwise, per the project's
   don't-optimize-before-profiling principle).
3. **Central point for future leak-tracking in debug builds** — not
   implemented yet in Phase 1, but the indirection exists specifically
   so it can be added later (e.g. wrapping `fdk_alloc` in debug builds
   to record call sites) without touching every call site in the
   codebase.
4. **Overflow-checked array allocation.** `fdk_alloc_array(count,
   elem_size)` rejects `count * elem_size` that would overflow
   `size_t`, rather than silently wrapping and under-allocating —
   this matters anywhere a size comes from parsed/untrusted input
   (theme files, protocol messages), per `docs/security.md`.

## Debug-build memory safety

The default (`make` / `make static` / `make shared` without
`release`) build compiles with `-fsanitize=address,undefined`. This
is intentional and should stay on for all development and CI —
`make release` (no sanitizers, `-O2 -DNDEBUG`) is only for the final
shipped artifact. Every test in `tests/` is expected to pass clean
under ASan+UBSan with zero leaks; a test that "passes" but leaks or
hits UB is a bug, not a pass.
