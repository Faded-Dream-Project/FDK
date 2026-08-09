# FDK Testing

## Two test suites, deliberately separated

**`make test`** — platform-independent. Never requires a display of
any kind (X11 or Wayland). Safe to run in any CI container with zero
setup. Covers: allocation, version/error-string correctness, and
`fdk_init()`'s behavior when no display is reachable at all (including
with a display explicitly requested — `FDK_PLATFORM_X11` /
`FDK_PLATFORM_WAYLAND` — to confirm there's no silent fallback).

**`make test-x11`** — X11 platform integration. Requires a reachable
X11 display. If `$DISPLAY` is already set when you run it, it tests
against that (a real desktop session, or an Xvfb/Xephyr you started
yourself). If not, it starts a throwaway Xvfb automatically, runs
against it, and tears it down afterward — no manual setup needed even
in a bare CI container, as long as `Xvfb` is installed.

There is currently no `make test-wayland` — see "Wayland test
coverage" below for why, honestly.

## Why the split

Per project requirement: ordinary `make test` must not depend on the
developer having a graphical desktop session. `fdk_init()` in Phase 2
genuinely does connect to a real display, unlike Phase 1's stub — so
without this split, plain `make test` would fail in any headless
environment (which is most CI). Splitting the suite is the actual fix,
not faking a platform connection or skipping platform tests silently.

## What `make test-x11` actually verifies

Real, observable behavior against a live X server (see
`tests/test_x11_integration.c`):

- Connect and clean shutdown
- Connect with a custom `app_id`
- `fdk_quit()` called before `fdk_run()` doesn't crash
- `fdk_run()` returns immediately when no windows are open (per its
  documented exit condition in `fdk_core.h`)
- Window create → show → hide → show → destroy
- Window size is correctly reported after creation
- `fdk_window_set_title()`, including with `NULL`
- **Resize round-trip**: calling `fdk_window_resize()` and actually
  receiving a real `FDK_EVENT_WINDOW_CONFIGURE` event back through
  `fdk_run()`'s dispatch path, with the correct new size — this is the
  test that proves the event loop, X11 event translation, and
  callback dispatch all work together, not just that individual
  pieces compile.

**Known, honest gap:** `WM_DELETE_WINDOW` (the close-request path,
`FDK_EVENT_WINDOW_CLOSE_REQUEST`) is not exercised by `make test-x11`.
Triggering it requires a window manager actually running to send the
`ClientMessage` — bare Xvfb has no window manager by default. The test
says so explicitly (`[skip]` line) rather than silently omitting
coverage. Closing this gap would mean either running a minimal WM
(e.g. a scripted `xdotool`/synthetic `XSendEvent` from a second
process) inside `make test-x11`, or a separate `make test-x11-wm`
target — deferred rather than half-implemented; the underlying
`WM_DELETE_WINDOW` registration and `ClientMessage` handling in
`src/platform/x11/x11_connection.c` and `x11_dispatch.c` is real code,
just not exercised by an automated test yet.

## Wayland test coverage

There is no automated Wayland integration test yet. Reason, stated
plainly: unlike X11 (where Xvfb gives a real, if minimal, headless
server), there is no equivalently simple, universally-available
headless Wayland compositor to spin up the way `make test-x11` spins
up Xvfb. (Candidates like a nested `weston --backend=headless-backend`
exist but were not available/verified in the environment this backend
was developed in — see the project's "no fake completion" principle:
this is recorded as a gap to close, not silently worked around.) The
Wayland backend (`src/platform/wayland/`) does compile cleanly and
link correctly, and its logic mirrors the X11 backend's structure
closely enough that the same category of bugs would likely be caught
by code review and by eventually adding `make test-wayland` against a
headless Weston, once that's set up and verified in a real CI
environment. Until then, treat the Wayland backend as **implemented
but only integration-tested manually against a real compositor**, not
as verified to the same bar as the X11 backend.

## Known Xvfb flakiness (investigated and fixed)

During development, `make test-x11` was intermittently flaky —
roughly 30-50% failure rate on repeated runs, with `fdk_init()`
failing to connect on the second or third connection attempt within
the same test binary. Root-caused via a systematic isolation process
(see the actual investigation transcript in project history if
available) to **two separate real issues**, both now fixed:

1. `fdk_x11_window_destroy()` called `XDestroyWindow()` followed only
   by an implicit flush on the next request, not an explicit
   `XSync()`. A caller that destroys a window and immediately calls
   `fdk_shutdown()` (which calls `XCloseDisplay()`) could race: the
   destroy request might still be sitting in Xlib's client-side write
   buffer when the socket closes. Fixed by adding `XSync()` after
   `XDestroyWindow()` in `x11_window.c`.
2. **The actual primary cause**: the original `make test-x11` Makefile
   recipe used `$RANDOM` to pick a throwaway display number, but Make
   recipes run under `/bin/sh`, which on this project's development
   environment is `dash` — and `dash` does not implement `$RANDOM`
   (it silently expands to empty string). Every invocation therefore
   used the exact same fixed display number, causing a stale-socket
   race between successive test runs. Fixed by deriving the display
   number from the shell's own PID (`$$$$` in the Makefile, which
   Make expands to `$$` for the shell, which the shell expands to its
   PID) instead of `$RANDOM`.

Both fixes are in place; `make test-x11` was stress-tested for
stability (multiple consecutive clean runs) after each fix to confirm
before moving on, per the project's "verify, don't assume" standard.
This section stays in the docs as a record of what was found and why,
not just what the fix was — useful if similar flakiness ever
resurfaces in a different environment.

## Sanitizers

Every test binary (both suites) is built with AddressSanitizer +
UndefinedBehaviorSanitizer by default (see `docs/build.md` — this is
the standard debug-build configuration, not a special test-only flag).
A test that "passes" but leaks memory or trips UB is a failure, not a
pass, per `docs/memory.md`.
