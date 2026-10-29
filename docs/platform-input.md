# Platform input: how keys become events

This is the contract doc that several source files point at
(`fdk_event.h`'s scancode block, `shortcut.c`, `x11_platform.h`'s
window-lookup note, `wayland_seat.c`'s repeat note). It pins how the
two backends land on ONE keyboard event model, and where they
deliberately differ.

## The scancode contract

`fdk_scancode` (fdk_event.h) is **Linux evdev numbering**. Both
backends derive their keycodes from that space on Linux, by
different arithmetic:

- **X11**: protocol keycodes are evdev + 8 (guaranteed by the X
  protocol spec itself, not an XKB query). `x11_events.c` subtracts
  8. Keycodes below 8 are protocol reserved and rejected.
- **Wayland**: `wl_keyboard::key` reports evdev codes DIRECTLY —
  no offset. `wayland_seat.c` copies them through unchanged.

The convergence is why applications can bind "the W key" (scancode
17) for WASD-style movement and it works identically on both
backends and across layouts. The small `FDK_KEY_*` define list in
fdk_event.h names the keys with toolkit-level meaning (traversal,
activation); it is deliberately NOT a keyboard enumeration.

## Text vs. identity

Every key event carries both:

- `scancode` — the physical key (layout-stable). Bind SHORTCUTS on
  this.
- `codepoint` — the layout-resolved glyph the key produces under the
  current modifier state (Shift+A → 'A'; dead keys and IME
  composition resolve through the same field on X11). Use this for
  TEXT ENTRY.

Resolution paths differ per backend:

- **X11 (1.3.1)**: an XIM input context (XCreateIC with
  XIMPreeditNothing | XIMStatusNothing) per window;
  Xutf8LookupString fills the codepoint, giving full-Unicode text
  entry including compose-key sequences. Before 1.3.1 the plain
  XLookupString path was Latin-1-only.
- **Wayland**: xkbcommon state (the wl_keyboard keymap) resolves
  keysym → UTF-32 per key press.

## Input methods — IME composition (1.1.0, M-1)

One event model covers both backends: `FDK_EVENT_IME_PREEDIT`
(the composition string an input method is building — empty clears
it; the payload carries the composition caret as a UTF-8 byte
offset) and `FDK_EVENT_IME_COMMIT` (the final text; never empty).
`text` is UTF-8 owned by FDK for the callback duration only — the
drag-payload rule. Routing mirrors keys: the FOCUSED Entry or
TextView consumes both (preedit renders inline at the caret through
the `fdk_entry_set_preedit` / `fdk_textview_set_preedit`
groundwork; commit splices at the caret through the single
mutation primitive, so undo, on_changed, and a11y all ride it).
When nothing focused consumes them they reach the application's
window callback — custom text surfaces implement their own
composition there. A single-codepoint commit ALSO arrives as the
ordinary KEY_DOWN's codepoint (compose keys keep working for
codepoint-only consumers); multi-codepoint commits — the CJK case
an IME exists for — arrive ONLY as `IME_COMMIT`.

Backend machinery:

- **Wayland**: `zwp_text_input_v3` (optional global — absent means
  no IME on that compositor; keys still flow). The v3 batch state
  machine is honored exactly: `preedit_string`/`commit_string`
  latch pending state, `done` applies it (commit replaces preedit
  first — the v3 ordering), `enter`/`leave` gate enable/disable.
  Surrounding text is pushed on focus gain and after IME routing
  through the window layer's `ime_update_surrounding` hook (the
  focused text widget's text + caret in bytes). One compositor
  quirk is closed client-side: wlroots drops the focused surface
  on window destroy WITHOUT sending `leave`, which leaves the
  v3 input enabled — and `enable`-on-enabled is a state-machine
  no-op (the compositor never re-activates the IM). FDK tracks the
  enabled session and closes a stale one at the next `enter`
  (disable + commit first), so window switches keep IME working.
  `delete_surrounding_text` (autocorrect-style context edits) is
  consumed and logged, not applied — level-2 material; most CJK
  engines never send it.
- **X11 (1.1.0)**: the per-window XIC now asks for
  `XIMPreeditCallbacks | XIMStatusNothing` FIRST (falling back to
  the 1.3.1 PreeditNothing posture when the input method refuses
  the callback style): the IM hands composition to FDK through the
  preedit start/draw/done callbacks, the per-IC composition buffer
  is maintained by a pure, headless-tested UTF-8 splice
  (character-index ↔ byte-offset — the translation every XIM
  client historically corrupts), and commits flow
  Xutf8LookupString: one codepoint stays the KEY_DOWN codepoint,
  more than one dispatches `IME_COMMIT` and the key event is
  swallowed.

What is deliberately level-2 (the roadmap's honest fence):
paragraph-level bidi in composition, preedit highlight spans
(v3's cursor_begin/cursor_end are read, only the caret renders),
`delete_surrounding_text`, and rich preedit styling.

## Modifier tracking

The four `fdk_key_modifier` bits (Shift/Ctrl/Alt/Super) reflect
currently-held modifiers at event time. Lock states (Caps/Num) are
deliberately absent: they are already baked into the resolved
codepoint, and matching a shortcut "regardless of Caps Lock" is the
behavior you get by construction. X11 reads the event's state mask;
Wayland derives from xkbcommon's depressed-modifier set.

## Key repeat

- **X11**: the server-side auto-repeat (requested at connect via
  XAutoRepeatOn semantics) emits a KeyPress of an already-down
  keycode; `x11_events.c` tracks a 256-bit per-keycode down-set and
  marks those presses `is_repeat = 1`. Widgets that follow
  hold-to-repeat (Entry's Backspace runs, window shortcuts) work.
- **Wayland** (1.3.5): `wl_keyboard::repeat_info` gives rate/delay
  and repeat is CLIENT-driven — the protocol never re-sends the
  press. FDK runs it: presses arm a deadline (now + delay), the
  pump's `next_wakeup_ms` cap (the platform timer analogue of the
  core timer queue's deadline cap) keeps the loop waking at period
  speed, and `dispatch_pending` fires the overdue repeats — up to
  8 per call with a resync past that, the X11 shape (the server
  queues repeats at cadence, so a slowly-pumping app reads them in
  batches and the delivered COUNT is preserved rather than the rate
  silently halving). One key repeats at a time — the latest press
  re-arms; its release, keyboard focus loss, or a 0 rate disarms;
  each repeat re-reads the live xkb state so modifiers held or
  released mid-hold shape the repeated codepoint exactly like the
  X11 server's repeats. `is_repeat` reaches widgets identically on
  both backends now.

## The shortcut layer (1.3.3)

Two consumers parse shortcut SPECS ("Ctrl+S") into
(modifier mask, scancode) pairs — `fdk_shortcut_parse` in
`fdk_event.h` documents the grammar:

1. **Application shortcuts** — `fdk_window_add_shortcut` registers
   callbacks in a per-window table.
2. **Menu accelerators** — items under a window's menu bar are
   scanned LIVE per keypress (nothing cached; the tree as it stands
   is the truth).

Dispatch order for a KEY_DOWN on a non-popup window:
`resize-edge chrome → application shortcut table → menu-bar
accelerator scan → widget tree → application window callback`.
The first consumer that matches CONSUMES the event. Consequences
worth knowing:

- **Accelerator semantics**: Ctrl+S fires wherever focus sits,
  including an Entry that would otherwise claim the key. Do not
  register editing keys the focused widget should own.
- **Physical vs. printed key**: a spec binds the PHYSICAL position
  (scancode, layout-stable). The Entry widget's built-in editing
  keys (Ctrl+C/V/X/A/Z/Y) match the PRINTED glyph (codepoint) —
  the native text-editing rule. On QWERTY these are the same keys;
  on AZERTY/Dvorak they can differ (AZERTY prints 'a' at the
  physical Q position). Both rules are intentional per layer; this
  is the one place they can disagree.
- **Popups are exempt**: while a menu popup holds the grab, its own
  tree owns the keys (Up/Down/Enter/Esc navigation); a window
  shortcut table cannot steal them.
- **Exact modifier equality**: "Ctrl+S" does not fire while Shift
  is also held. Repeat presses match like initial ones.

## Window lookup on the dispatch path

`x11_platform.h` keeps an open-addressed Window-ID →
`fdk_platform_window*` map so `dispatch_pending()` can resolve the
raw X ID an XEvent carries. Linear over a small array was chosen
over a hash deliberately (typical apps have single-digit
top-level windows); revisit only under profiling evidence.

## Pointer input

Pointer events arrive in window-local floats (`fdk_pointer_event`).
X11 delivers motion against the event window directly; Wayland
resolves surface-local coordinates via the seat's pointer focus.
Both backends synthesize the window-enter/leave pair FDK's hover
model expects, and the resize-edge cursor shaping runs in the
window layer (see fdk_window.h's resize section) — the backends
only translate cursor-shape requests.

## Cursor shaping — the full named set (1.1.0-rc2, S-5 / B-12)

`fdk_window_set_cursor` accepts the complete named set — 22 shapes
mirroring cursor-shape-v1's semantic space (the names CSS, GTK4 and
Qt6 use). The resolution order, most specific first: an explicit
application request owns the window (edges included); without one,
a widget hover shape rules (Entry / TextView / Spin — or an editable
Combo's entry — show the I-beam); without either, the resize-edge
compass. The mapping is a pure function the headless suite pins
(`fdk__cursor_resolve`, tests/test_cursor.c).

Per backend:

- **Wayland**: `wp_cursor_shape_device_v1.set_shape` when the
  compositor offers the manager (sway 1.10 does — the integration
  suite pins the full enum table row by row through the test seam);
  otherwise the XCursor THEME by hand — the shared no-libxcursor
  loader (src/platform/cursor_theme.c) parses the documented
  container format and uploads ARGB to a wl_shm cursor surface.
- **X11**: two tiers per shape. Tier 1: the same shared theme loader
  through the dlopen'd RENDER lane (libXrender.so.1 ->
  XRenderCreateCursor from an ARGB depth-32 pixmap — no new build
  dependency; absent library/extension falls through). Tier 2: the
  nearest core cursor-font glyph. Which shapes prefer the theme is
  table-driven (`named_theme_first`); the substitutions, honestly:

| shape          | X11 core-font twin when no theme   |
|----------------|------------------------------------|
| TEXT           | xterm (the I-beam) — exact         |
| CROSSHAIR      | crosshair — exact                  |
| WAIT           | watch — exact                      |
| PROGRESS       | exchange (GDK's own twin)          |
| MOVE           | fleur — exact                      |
| COPY           | none — theme or the plain arrow    |
| ALIAS          | none — theme or the plain arrow    |
| NO_DROP        | pirate (the skull)                 |
| NOT_ALLOWED    | X_cursor (the refusal X)           |
| GRAB           | hand1 (the open hand)              |
| GRABBING       | hand2 (the pointing hand — the font has no fist) |
| ALL_SCROLL     | fleur                              |
| COL_RESIZE     | sb_h_double_arrow — exact          |
| ROW_RESIZE     | sb_v_double_arrow — exact          |
| ZOOM_IN/OUT    | none — theme or the plain arrow    |
| HELP           | question_arrow — exact             |
| CELL           | plus                               |
| CONTEXT_MENU   | none — theme or the plain arrow    |
| VERTICAL_TEXT  | xterm (no vertical glyph exists)   |

The no-twin rows keep the DEFAULT ARROW when neither tier
produces a cursor — the no-lying-fallback posture (a shape that
lies about what the press does is worse than the plain arrow).
fdk-shell mirrors the set (`fdk_shell_cursor`, 0.4.0) — its values
are FDK's own, passed through unchanged.

## Touch input (1.1.0, M-3)

Direct-touch contact tracking is a first-class event family:
`FDK_EVENT_TOUCH_DOWN/UP/MOTION/CANCEL`, one slot per contact
(`fdk_touch_event.id`), positions window-local and logical like
every pointer event. The two backends translate; the POLICY lives
once in the core (src/window/touch.c):

- **Tap synthesis**: a single-slot contact lifted within
  `FDK_TOUCH_TAP_MS` and `FDK_TOUCH_TAP_SLOP` logical px of its
  start synthesizes a full left
  `FDK_EVENT_POINTER_BUTTON_DOWN`+`UP` pair at the tracked
  position — widget applications work on touchscreens for free.
  The verdict comes from the ENGINE's tracked position because
  wl_touch's up event carries no coordinates (the protocol omits
  them; XI2's TouchEnd does not, but its coordinates equal the
  last update anyway).
- **Two-finger scroll**: while exactly two slots are active, the
  CENTROID's motion emits `FDK_EVENT_POINTER_SCROLL` with
  natural-scrolling deltas (delta = -(travel): fingers drag down,
  content follows). Entering two slots cancels every pending tap;
  re-entry re-arms the centroid from the re-entry instant (no
  jump). A multi_ever latch — armed the moment any second slot
  appears, cleared only when the engine goes fully idle — makes
  the lifted-last finger of a two-finger gesture never click
  (the classic tap-after-scroll phantom).
- **Cancel**: wipes every slot and disarms all pending taps; a
  click after a compositor takeover would be a phantom press.

Trackpad two-finger scrolling is NOT this path: the platform's
own gesture recognition delivers it as ordinary
`FDK_EVENT_POINTER_SCROLL` (wl_pointer axis / wheel buttons), which
FDK already translated since Phase 2.

Per backend:

- **Wayland**: wl_touch bound from the seat's capabilities; DOWN
  carries the surface (window lookup), UP/MOTION only the id, so
  the connection keeps an id→window map (`FDK_WL_TOUCH_IDS`,
  bounded; overflow contacts drop with one INFO line). Coordinates
  are surface-local logical — a straight copy. The frame/shape/
  orientation events are no-ops (nothing in the model consumes
  batch delimiters or contact ellipses).
- **X11**: XI2 touch (XI_TouchBegin/Update/End, X Input >= 2.2)
  through dlopen'd libXi — the GL lane's load discipline (no
  build/link dependency; absent libXi or old XI = no touch events,
  never an init failure; `$FDK_NO_XI2` opts out). XI2 events arrive
  as GenericEvents through Xlib's cookie mechanism
  (XGetEventData/XFreeEventData — libX11 itself); windows select
  the touch masks at create time, and XI 2.2's emulation rule
  suppresses the core-button twin of a touch on windows that
  selected XI2 touch — FDK's synthesized tap click is the only
  click a touch produces. Coordinates are window-local PHYSICAL
  pixels, divided by the connection's integer scale exactly like
  core pointer events.

Testing (the reachability problem): sway headless's seat exposes
no touch capability (wlroots has no virtual-touch protocol), and
Xvfb has no touch injection facility — so each backend exposes the
REAL translate path as a test seam (`test_touch` in the ops table,
the 1.3.5 test_key discipline), and the full policy battery runs
headless against the engine itself (tests/test_touch.c: tap, slop,
timeout, drag, natural deltas, re-arm, phantom-tap latch, cancel,
protocol noise). Real touch HARDWARE remains the owner's desktop
check (TESTING-ON-DESKTOP.md).
