# DOC-GAPS.md — the P5 blind-walk report (extension API v3)

The blind example (`examples/h-status-panel.c`) was written ONLY from
`docs/EXTENDING.md` and the public headers, with `src/` unread. This
file records every point where the docs were wrong, missing, or
misleading — and what FDK changed because of it.

## 1. Idle-inhibit was unreachable from a surface — FIXED IN FDK

`EXTENDING.md` §9's capability table lists `FDK_EXT_CAP_IDLE_INHIBIT`
and nothing anywhere says the inhibitor is WINDOW-API-only. The
docs' own canonical add-on (a panel!) could not hold the screen
awake: `fdk_window_set_idle_inhibited` takes an `fdk_window*`, and an
ext surface is not one.

**FDK change**: `fdk_ext_surface_set_idle_inhibited(surface, inhibit)`
(extension API v3) — the same inhibitor object routed through the ext
surface's internal window; same UNSUPPORTED contract, same capability
probe. The docs got the surface-scope note.

## 2. The layer bar's height was undocumented — the 0-px wall

An anchored-edge layer surface (TOP + LEFT + RIGHT) gets its height
from the CLIENT's committed size. `fdk_ext_layer_surface_set_size`
exists in `fdk_extend.h`, but `EXTENDING.md` §4 never says an
anchored-edge panel MUST call it — the blind bar configured at
1280x720 and then stayed invisible at 0 px until the size was set.
The example now sets 32 px before showing; the docs got the note.

## 3. The session-lock docs were WRONG about handle sources — FIXED IN FDK

`fdk_extend.h` said the raw lock-surface handles come "from the
add-on raw-event surface **or from an FDK ext surface's native
accessor**". The second half is a protocol violation: a wl_surface
carries ONE role, and an ext surface's native handle already carries
the layer/window role — `get_lock_surface` on it answers protocol
error 2 and the compositor kills the connection (caught live by the
first honest run of the rewritten example; sway's log:
`Cannot assign role ext_session_lock_surface_v1 to wl_surface@28,
already has role zwlr_layer_surface_v1`).

**FDK change**: the docs now state the role-less rule plainly, and
the FIRST-CLASS path exists: `fdk_session_lock_screen_create(ctx,
lock, out)` mints FDK's own role-less surface, attaches the role on
the first bound output, installs the configure listener (acked, then
routed through the ext-surface configure flow), and renders — no raw
handles at all. `examples/f-session-lock.c` is the FDK-native shape;
`f-session-lock-raw.c` keeps the escape hatch.

## 4. No public wl_output accessor — FIXED IN FDK

The lock role wants one surface per output; the opt-in
`fdk/ext/wayland.h` had a seat accessor (`fdk_ext_wl_seat`) but no
output accessor, so an add-on had to open the raw-event surface just
to bind one output.

**FDK change**: `fdk_ext_wl_output_count(ctx)` /
`fdk_ext_wl_output(ctx, index)` (append-only, v3) — the same bound
outputs FDK's own output model drives.

## 5. Accurate parts worth naming (not every wall is a doc bug)

- §5's event names (`FDK_EVENT_WINDOW_CONFIGURE`, `FDK_EVENT_KEY_DOWN`)
  and `fdk_ext_surface_set_event_callback` are correct; the blind
  draft's first guesses (`FDK_EVENT_CONFIGURE`, `set_callback`) were
  the author's error, corrected by reading `fdk_event.h`.
- The configure payload nests (`event->configure.size.width` —
  `fdk_size`), documented in `fdk_event.h` ✓.
- §6's rendering flow (get_surface → get_info → draw → present) works
  exactly as written; the first-configure-then-first-frame ordering
  behaved as §5's EXPOSE note promises.

## 6. Example honesty (the same walk, applied to (d) and (f))

- `examples/d-input-method.c` now uses `fdk_ext_wl_seat` (the
  documented accessor) instead of a raw-event queue; the protocol
  listener stays vendor-carried (the documented add-on contract).
  The raw path lives in `d-input-method-raw.c`.
- `examples/f-session-lock.c` is entirely first-class (three public
  calls + the draw loop). The raw path lives in
  `f-session-lock-raw.c`.
