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

## The Phase 11 memory-safety audit

The stabilization pass audited the whole tree against the following
criteria, with the results recorded here so the audit is reproducible
rather than a vibe:

1. **No raw malloc/free outside `src/core/alloc.c`.** Verified by
   grep: every allocation site goes through `fdk_alloc`/
   `fdk_alloc_array`/`fdk_realloc`/`fdk_free` (the checked,
   zero-initializing, overflow-guarded wrappers).
2. **Every allocation path has a matching free on EVERY exit.**
   Verified by the parsers' no-partial-results discipline (theme,
   catalog: destroy-the-working-object on any error) and by the
   widget teardown walk (subclass destroy hook, then children,
   then arrays, then a11y strings, then relation edges, then the
   widget itself). The deferred-destroy machinery keeps frees off
   the dispatch/paint stacks.
3. **No unchecked multiplication before allocation.**
   `fdk_alloc_array` guards `count * size` overflow centrally; the
   remaining hand-rolled grows in widget/list/tree/menu code were
   audited for the same shape (capacity doubling on `size_t`, with
   the count check before the multiply).
4. **Dangling-pointer classes are structurally prevented**: widget
   destroy unlinks before any callback can observe it; a11y
   relations are removed from BOTH ends at destroy (no dangling
   relation targets); layout-batch dirty entries are forgotten at
   destroy (no flush into freed memory); window destroy tears down
   the root with the ownership marker dropped first.
5. **Every test binary runs under ASan+UBSan with leak detection**
   (the default build), including the GUI suites against a real X
   server. The Phase 10/11 sessions' new suites (a11y, i18n, layout
   batching) all run leak-clean; the leaks they DID find during
   development (a dangling subscription recorder, an Entry CHAR-run
   NULL deref) were fixed, not suppressed.
6. **Bounded inputs everywhere a parser touches memory**: theme and
   catalog files (1 MiB / line / string / entry caps), Entry's text
   buffer (64 KiB), the a11y subscriber and relation tables (16
   each), the layout dirty list (4096 + wholesale fallback). No
   attacker-sized allocation scales with input size without a cap.

Known accepted sharp edges (documented, not hidden): FDK's object
model is explicitly NOT thread-safe (docs/threading.md — use one
thread, or full externally-synchronized handoff); passing a
destroyed widget pointer is ordinary C undefined behavior like a
double free, exactly as documented in fdk_widget.h.
