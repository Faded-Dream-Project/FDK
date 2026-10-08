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
