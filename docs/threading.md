# FDK Threading Model

## The rule

**All FDK objects (`fdk_context` and everything reachable from it —
windows, widgets, timers, once they exist) must only be accessed from
the thread that called `fdk_init()`.** Conventionally this is the
application's main thread; FDK refers to it as the "UI thread."

This is the same model GTK, Qt, and most native GUI toolkits use, for
the same reasons: window systems (X11, Wayland) are themselves
single-connection/single-thread-affine at the protocol level in
practice, and a lock-everything approach would cripple both
performance and API ergonomics for no real benefit to typical GUI
applications.

## Worker threads

Applications may freely use additional threads for background work
(networking, computation, file I/O) — FDK does not restrict what an
application does off the UI thread. What a worker thread must **not**
do is call any `fdk_*` function on an object owned by the UI thread's
context.

## Getting work back onto the UI thread

Phase 1 does not yet implement a cross-thread scheduling primitive —
this is planned for Phase 2 (Platform Layer) alongside the event loop
and idle-callback queue described in the project's core requirements
(see `docs/roadmap.md`). The intended shape (subject to refinement
once implemented against real usage):

```c
/* Planned, not yet implemented: */
void fdk_invoke_on_ui_thread(fdk_context *ctx, void (*fn)(void *), void *user_data);
```

A worker thread calls this to hand a callback + data pointer to FDK;
FDK queues it and runs it on the UI thread during the next event loop
iteration, ordered with other pending events/idle callbacks. This
document will be updated with the finalized signature and thread-safety
guarantees when that lands — this section is a placeholder specifically
so `fdk_core.h`'s doc comments have something concrete to point to
rather than an unstated promise.

## Per-function documentation

As each public function is added, its doc comment states explicitly
whether it's UI-thread-only (the default assumption unless stated
otherwise) or safe to call from any thread. `fdk_log()` and its
sink-configuration functions are documented individually in
`fdk_log.h` since logging is the one subsystem with different rules
(see that header — the sink itself, once installed, is safe to invoke
from any thread; installing/changing it is not).
