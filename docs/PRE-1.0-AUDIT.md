# PRE-1.0-AUDIT.md — the freeze checklist (extension API v3, 2026-11-10)

A checklist, not code changes: every public symbol and type that would
be painful to change after 1.0, flagged by exposure class. Counts are
from the tree at `f2116c7`.

## 1. The ABI-freeze surface (what 1.0 signs)

| Header | Functions | Typedefs | Pain-to-change class |
|---|---|---|---|
| fdk_core.h | 8 | 5 | HIGH — the context/init/shutdown spine |
| fdk_window.h | 38 | 4 | HIGH — the application's main handle |
| fdk_surface.h | 62 | 10 | HIGH — rendering + the dmabuf import |
| fdk_widget.h | 36 | 5 | HIGH — the widget tree + the embed budget |
| fdk_text.h | 24 | 11 | MEDIUM — shaping tier, layout, metrics |
| fdk_event.h | 3 | 21 | HIGH — the event union's layout is caller-visible |
| fdk_extend.h | 53 | 15 | HIGH — the whole extension API (v3) |
| ext/wayland.h, ext/x11.h | ~20 | 4 | LOW — opt-in escape hatches (native types by design) |

~290 public functions, ~85 public typedefs, 34 event types, 19
capability flags, FDK_EXT_API_VERSION 3.

## 2. Layout exposure — the struct-bytes contracts

- **`fdk_widget`'s 1024-byte embed budget** (fdk_widget.h:311): the
  FIXED, PADDED widget struct (`sizeof(fdk_widget) <= 1024`) lets
  internals grow without breaking callers who embed it. After 1.0 the
  1024 ceiling is ABI: growing it moves every embedded widget. The
  freeze checklist: `sizeof(fdk_widget)` must be printed at release
  and pinned in the changelog.
- **`fdk_event_data`'s tagged union** (fdk_event.h): by-value delivery
  means the union's size is caller-visible (stack slots). Appending
  event TYPES to the enum is safe; appending PAYLOAD structs that
  grow the union past the current max member is NOT. Freeze the union
  size at 1.0; new payloads must fit existing slots or ride a pointer.
- **The input-struct contracts** (`fdk_ext_layer_options`,
  `fdk_dmabuf_params`, the kind ops' `struct_size` fields): these are
  the SAFE growth pattern — FDK reads exactly struct_size bytes and
  fields append only. The checklist: never reorder, never repack; the
  struct_size mechanism is the ABI.
- **`fdk_color`, `fdk_rect`, `fdk_size`, `fdk_span`**: primitive
  layouts everywhere; a repack breaks every compiled add-on.

## 3. Semantics frozen at 1.0 (change = a version bump)

- **The cap flag NUMBERS** (1-19): `FDK_EXT_CAP_*` values are caller
  constants — renumbering breaks compiled add-ons. Append-only, like
  the API.
- **The result codes** (`fdk_result`): negative values are ABI. New
  codes append; never renumber, never reuse a retired number's
  meaning silently.
- **The event type numbers** (fdk_event.h): same contract as caps.
- **`FDK_EXT_API_VERSION` 3**: the v3 add-on contract — "fields
  APPENDED, never reordered" (fdk_extend.h:256-259). The version
  moves to 4 only when a break is unavoidable, and the break must be
  listed in EXTENDING.md §2.
- **The export map** (build/libfdk.exports.map): every symbol named
  there is frozen; `make test-exports` gates additions, not removals
  — removal after 1.0 needs a deprecation cycle.
- **The dmabuf fourcc table's MEANING**: UNSUPPORTED for an unlisted
  fourcc is the frozen contract; adding formats is compatible.
  Removing one is not.

## 4. Anything unstable or underdocumented (flag, don't fix here)

- **`fdk_text_shaping_tier` / `fdk_font_layout_utf8`'s cluster
  granularity**: the level-0/1 degradation classes are documented but
  the per-cluster `rtl` flag is segment-granularity, not per-run —
  an add-on relying on finer granularity will over-fit. Flag: keep
  the coarse contract explicit in the 1.0 docs.
- **The INVALID-modifier zero-copy semantics** (v3, this session):
  implicit-import-or-UNSUPPORTED is new and tested only through the
  memfd path (no DRM device in the sandbox — the GBM round-trip and
  the modifier query SKIP; CI's sway leg runs the query against
  llvmpipe). Flag: needs one DRM-box soak before signing 1.0.
- **`fdk_session_lock_screen_create`'s multi-output story**: the kind
  mints the FIRST bound output's surface; a multi-output lock screen
  needs one screen per output and the outputs accessor says which —
  but no example exercises two yet. Flag for the post-1.0 example
  set.
- **`fdk_ext_surface_set_idle_inhibited`'s X11 answer**: the inhibitor
  is Wayland-only; X11 answers UNSUPPORTED (the D-Bus fallback is the
  caller's). Documented, not example-covered.
- **The ext/x11.h escape hatch**: native X11 types by design; the
  unstable-tier gate (tests/test_matrix.c) pins that no Wayland
  protocol type leaks into the CORE headers, but ext/x11.h's own
  surface has no equivalent gate. Flag: add an X11-side declaration
  gate to the matrix test.

## 5. Not covered by a test or example (the gap list)

Updated 2026-11-11 (this session closed four of five):

- `fdk_virtual_keyboard_key` — CLOSED: the P3 protocol test injects Q
  through the VK and asserts REAL delivery (a KEY_DOWN with scancode
  16 on the focused window) on the two-output sway rig, under
  ASan/LSan.
- `fdk_ext_surface_set_cursor` — CLOSED: 09_gl_dmabuf_panel.c calls
  it add-on-facing (the labeled result printed; the documented
  UNSUPPORTED contract on a lane without the cursor machinery).
- `fdk_texture_dmabuf_query`'s max_modifiers=0 call shape — CLOSED:
  test_dmabuf.c exercises the count-only call against a live context
  and asserts the honest UNSUPPORTED without a lane (and the impl's
  guard order was verified against the docs' "out_n is always the
  true count" contract).
- The popup_attached hook's Wayland popup-parenting path — CLOSED
  2026-11-12: examples/11_ext_popup_attached.c registers a kind with
  the hook, pops a popup under sway, and asserts the hook fired and
  adopted the xdg_popup proxy (and asserts the documented X11
  negative: the hook never fires there).
- `fdk_ext_popup_create`'s X11 OR-window path — CLOSED 2026-11-12:
  examples/10_ext_popup.c pops a menu off a kind surface on Xvfb
  (draw + the close-request wiring + clean teardown under ASan) and
  documents the honest Wayland deferral. Both run in CI (Xvfb +
  sway). The audit's §5 list is now EMPTY.

## 6. The sign-off checklist (do at 1.0, in order)

1. `make test` + `test-exports` + both matrix lanes green at the
   release commit.
2. Print and pin `sizeof(fdk_widget)`, `sizeof(fdk_event_data)`,
   `sizeof(fdk_dmabuf_params)`, `sizeof(fdk_ext_layer_options)`.
3. The export map diff against the last release: additions only.
4. One DRM-box run of the dmabuf round-trip (or the SKIP recorded in
   the release notes with the self-hosted-runner note).
5. EXTENDING.md §2's version contract re-read against the final
   headers (append-only honored everywhere).
6. This file's §4 flags either resolved or accepted in the release
   notes.
