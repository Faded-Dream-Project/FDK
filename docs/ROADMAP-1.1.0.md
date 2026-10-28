# ROADMAP-1.1.0 — the gap audit and the proposed release scope

**Status:** IN IMPLEMENTATION (Phase 2, owner-approved). Version is
`-rc2`. Per-item status is marked inline as work lands; the
owner's Phase 2 directive: implement every tier (MUST, SHOULD,
COULD, DEFER), re-audit when done, keep going until nothing
meaningful remains or the sandbox blocks it.

**Base:** `main` @ `7db4a81` (2026-10-26). All verdicts were read out
of the tree at that commit — headers, backends, tests, docs — not
from memory of what was planned.

**Verified against:** `main` @ `ccafa7c` (2026-10-27, owner-directed
verification pass — see the Verified status table below). Verdicts
below that changed since the base are marked inline with
**[VERIFIED-CHANGED]**; the original audit text is kept.

**The rule this audit is judged by** (the project's own): *if an app
needs it and FDK lacks it, that's a bug in FDK.* GTK4 and Qt6 are the
feature references for the core; the wlr-layer-shell spec and
gtk-layer-shell are the references for fdk-shell. Judgment calls are
marked as such.

**How to read the registers:**

- Verdict: **present** (shipped and tested), **partial** (works with
  named limits), **missing** (absent).
- Effort: **S** (a focused slice), **M** (a multi-part slice with
  rigs and tests), **L** (a subsystem, several slices).
- ABI risk: whether closing the gap needs anything beyond FDK's
  append-only discipline (docs/abi-policy.md). Every item below is
  closable append-only; the two version bumps (extension API v2, no
  SONAME change) are the only ABI-adjacent events, and both are
  additive.

---

## Verified status (2026-10-27, @ `ccafa7c`)

Every item was re-judged against the live tree: sources inspected,
the full local battery run (`make test`, `make test-x11` 3x,
`make test-dbus` incl. the libdbus cross-implementation leg,
`make FDK_SANITIZE=0 test` + `test-dbus`, `make FDK_CONFIG=release
all examples verify-exports` at 757 exports, the sway-rig Wayland
integration suite, fdk-shell unit + X11 + Wayland suites, the
bar rig on BOTH sway and labwc, and the prefix-isolation proof).

**Counts:** DONE 15 (S-2 and S-1 pending owner verification) ·
PARTIAL 11 ·
IN PROGRESS 1 (M-9) · MISSING 30 ·
DEFERRED (by design) 5 · BLOCKED-BY-SANDBOX 0 — of 62 items.
Re-verified 2026-10-27 (post-trayhost): the full battery plus the
notifyd rig and the trayhost rig (all three phases green each).

**Phase 2 owner directives** (the mission's lettered tasks,
tracked beside the register; the register's 62-item math is
unaffected):

| id | item | status | evidence | remaining |
|----|------|--------|----------|-----------|
| T-B | reusable notifications + tray: the app-facing API in FDK core (notification client, StatusNotifierItem export, DBusMenu server) on the native D-Bus client, and the host-side model module in fdk-shell (watcher/host, item state, dbusmenu client, notification server/queue — data + events, no drawing), with trayhost/notifyd as thin reference UIs | **PARTIAL** — side 1 (the core app-facing API) DONE: fdk_notify.h + fdk_tray.h (src/desktop/), the byte-array bulk form in the value model, live-bus rigs with in-test mock daemon/watcher (make test-dbus, all green); docs EXTENDING.md §§14-15 + DECISIONS D-008 | side 2: the fdk-shell host-side model split (extract protocol/state from the renderers, the no-drawing module + minimal examples) |
| T-C | UI font size from the theme/settings (font_size + optional ui_scale, default 16px; a desktop text-scaling preference followed by default, FDK settings overriding); notification box, paddings, icons, tray cells scaled from it | **DONE (should work; owner verification pending)** | src/theme/ui_metrics.c (the four-channel resolution, live-following the XSETTINGS/portal/settings events — the color-scheme module's shape); notifyd/trayhost scale every dimension off fdk_ui_font_px() with --font-size pinning; core chrome via fdk__ui_derived_px (14-px baseline, ratios preserved, FDK_UI_FONT_SIZE=14 = the historical pixels); tests/test_ui_metrics.c + the rigs' XS sweeps (12/16/24/32: glyph-free pads, on-screen menus, trace-math) all green; DECISIONS D-009; owner steps TESTING-ON-DESKTOP.md §8 | your eyes on the real desktop (§8) |

| id | item | status | evidence | remaining |
|----|------|--------|----------|-----------|
| A-01 | output events | **DONE** | `fdk_shell_output_*` + live sway hotplug legs (tests/test_wl.c: "hotplug ADDED fired", "mode change -> CHANGED", "unplug -> REMOVED") — d2bc0de | — |
| A-02 | output metrics | **DONE** | `fdk_shell_output_get_info`; "initial set enumerable, metrics 1024x768 scale 1" (wl) + "the screen as one output, metrics exact" (x11) | X11 per-output metrics ride M-6's RandR |
| A-03 | X11 multi-monitor | **MISSING** | fdk-shell/src/x11/x11.c: "X11 without RandR probing (0.1.0 posture): ONE output" | M-6: RandR 1.3 enumeration + CRTCs + RRNotify |
| A-04 | namespace | **DONE** | configurable namespace; "namespace -> WM_CLASS res_class" (x11 tests) + "wl namespace layer" (wl tests) | — |
| A-05 | runtime parameter changes | **MISSING** | create-time desc only; no `fdk_shell_layer_set_*` runtime API | C-5 |
| A-06 | input regions / click-through | **PARTIAL** | full-transparency popups: `input_transparent` (fdk_window_options), XShape + `wl_surface_set_input_region` empty (1.3.2); no arbitrary partial regions | S-4: region API on windows + ext surfaces |
| A-07 | ext-API input+scale hooks | **MISSING** | no ops fields for it; FDK_EXT_API_VERSION still 1 | folded into S-4 (core-side), see D-002 |
| A-08 | auto-hide / reveal | **MISSING** | no API | C-5 (needs A-06) |
| A-09 | `_FDK_WM_LAYER` | **DONE** | set on every layer; "_FDK_WM_LAYER level names" test | — |
| A-10 | cascading submenus | **DONE** | `fdk_shell_popup_create_child` any depth; "popup cascades: 3-deep create/render + family sweep" (x11) + "2-deep cascade popup + family sweep" (wl) | — |
| A-11 | tray host | **DONE (should work; owner verification pending)** | trayhost (fdk-shell/examples/trayhost.c) — the full org.kde.StatusNotifierWatcher host on the native client: both registration dialects, the four properties (Get/GetAll/PropertiesChanged), item loading with the interface fallback chain and the a(iiay) decode, New* signal handling, vanished-item drops via NameOwnerChanged, and the complete dbusmenu round (GetLayout two-out-arg parse, AboutToShow, Event, mnemonic stripping, cascading submenus with edge flip); the rig (make -C fdk-shell test-tray) proves protocol (real dbus-daemon, libdbus+GLib cross-implementation callers, gdbus-monitor signal receipt), hostile registrations, the etiquette matrix, X11 pixels+XTEST-clicks+menu-cascade+Passive-fade/NeedsAttention under Xvfb+openbox, and sway layer-shell handshake+pixels. Owner steps: docs/TESTING-ON-DESKTOP.md §§5–7 | S-1 (D-007 records the shape decisions) |
| A-12 | notification surfaces | **DONE (should work; owner verification pending)** | notifyd (fdk-shell/examples/notifyd.c) — the full org.freedesktop.Notifications server on the native client; the rig (make -C fdk-shell test-notifyd) proves protocol (real dbus-daemon, libdbus+GLib cross-implementation callers, gdbus-monitor signal receipt), hostile/rate limits, name etiquette, X11 pixels+XTEST-clicks+EWMH under Xvfb+openbox, and sway layer-shell handshake+pixels. Owner steps: docs/TESTING-ON-DESKTOP.md §§1–4 (labwc, real incumbents, real clients, hotplug) | — |
| A-13 | outside-click dismissal, real serial | **DONE** | the core wl suite's menu leg drives xdg_popup.grab with REAL injector serials against sway: "outside click dismissed" (tests/test_wayland_integration.c) | owner's desktop: dismissal on layer popups specifically |
| B-01 | IME | **PARTIAL** (X11 compose) / **MISSING** (Wayland) | no text-input-v3 binding; XIM `PreeditNothing`; entry/TextView preedit groundwork renders it | M-1 |
| B-02 | text shaping | **DONE (level 1)** | M-2 shipped: the dlopen'd HarfBuzz + FriBidi + charset-filtered fontconfig fallback pipeline (src/text/shaping.c, docs/text.md "Complex-script shaping"); every text consumer reads shaped placements; tests/test_shaping.c pins both postures incl. the Latin compatibility re-exec; level 2 (paragraph bidi, RTL caret granularity, justification) stays open | — |
| B-03 | touch | **MISSING** | no wl_touch / XI2 touch / event types | M-3 |
| B-04 | window icons + PID | **MISSING** | no `_NET_WM_ICON`/`_NET_WM_PID`/`WM_CLIENT_MACHINE`/toplevel-icon; no `fdk_window_set_icon` | M-4 (+S-6 activation) |
| B-05 | X11 RandR (core) | **MISSING** | x11_connection.c: one screen, no output model | M-6 |
| B-06 | dark mode following | **DONE** [VERIFIED-CHANGED: audit said partial] | XSETTINGS `Gtk/ColorScheme` (x11_xsettings.c) + `org.freedesktop.appearance` portal on the native client; live: "SettingChanged flips the scheme + theme live" — d2a253f | — |
| B-07 | X11 DPI awareness | **DONE** [VERIFIED-CHANGED: audit said missing] | shipped 1.4.5: Xft.dpi + physical-size fallback, integer scale [1,3]; `test_x11_hidpi_gui` end-to-end on a private 192-dpi Xvfb | — (RandR scale is N/A: X11 has no scale protocol) |
| B-08 | D-Bus foundation | **DONE** [VERIFIED-CHANGED: audit said missing] | M-7's native client (src/dbus/, 112 exported fdk_dbus_* of 757 total, commit 4d42c8c): live daemon battery incl. the ported libdbus cross-implementation leg; 20k-mutation fuzz + prefix-exhaustive truncation in `make test` | — |
| B-09 | portal FileChooser fallback | **MISSING** | own dialogs everywhere (fdk_dialog.h: "no portal") | C-1 — **moved to CORE** on the native client (D-002) |
| B-10 | AT-SPI interop | **MISSING** | internal a11y tree only; comments state the no-daemon posture | S-8 — **CORE** on the native client (D-002) |
| B-11 | idle/screensaver inhibition | **MISSING** | no idle-inhibit / org.freedesktop.ScreenSaver | S-3 |
| B-12 | cursor shape set | **PARTIAL** | public surface exactly 2 shapes (fdk_window.h: DEFAULT/POINTER) + internal per-edge resize + enter re-apply (c58f5da W2) | S-5: full named set |
| B-13 | drag icons + drop targets | **PARTIAL** | DnD window-level both backends; drag icon explicitly absent ("start_drag with NO icon surface") | S-9 |
| B-14 | image formats | **PARTIAL** | stb static raster + nanosvg; no animated | C-4 |
| B-15 | GPU on Wayland | **MISSING** | CPU SHM only; GL lane is X11/GLX | D-1 (deferred by design) |
| B-16 | subsurfaces | **MISSING** | no wl_subsurface | D-2 |
| B-17 | tablets / gestures | **MISSING** | no zwp_tablet, no pinch/swipe | D-3 / C-6 |
| B-18 | widget residue | **PARTIAL** [VERIFIED-CHANGED: better than audited] | **editable combo DONE** (`fdk_combo_set_editable`); date/time picker MISSING | S-10: the picker |
| B-19 | session restore protocols | **PARTIAL** | typed prefs store; no WM_SAVE_YOURSELF / xdg session | D-5 |
| M-1 | IME both backends | **MISSING** | = B-01 | the whole item |
| M-2 | shaping level 1 | **DONE** | = B-02 (level 1; level 2 open) | — |
| M-3 | touch basics | **MISSING** | = B-03 | the whole item |
| M-4 | window identity | **MISSING** | = B-04 icons/PID part | the whole item |
| M-5 | fdk-shell outputs | **DONE** (rc2, 0.2.0) | live sway output hotplug/metrics legs + namespace + `_FDK_WM_LAYER` — d2bc0de | — |
| M-6 | X11 multi-monitor | **MISSING** | = A-03/B-05 | the whole item |
| M-7 | D-Bus foundation + dark mode | **DONE** (foundation 4d42c8c; dark mode d2a253f) | live-bus battery green incl. cross-implementation; wire bytes pinned vs captured reference output; fuzzed; CI makes skips fatal | — [VERIFIED-CHANGED: the row's "rides on top (next)" is stale — dark mode landed] |
| M-8 | popup cascades | **DONE** (rc2, 0.2.0) | 3-deep x11 + 2-deep wl family sweeps — d2bc0de | — |
| M-9 | the release gate | **IN PROGRESS** | discipline verified: no tags beyond v1.0.0, rc line held, per-push CI, per-item docs | owner's real-desktop sign-off before any tag |
| S-1 | tray host | **DONE (should work; owner verification pending)** | = A-11 (trayhost + the rig; D-007 records the shape decisions) | owner verification via docs/TESTING-ON-DESKTOP.md |
| S-2 | notification display | **DONE (should work; owner verification pending)** | = A-12 (notifyd + the rig; D-006 records the shape decisions) | owner verification via docs/TESTING-ON-DESKTOP.md |
| S-3 | idle inhibition | **MISSING** | = B-11 | wl protocol + ScreenSaver D-Bus |
| S-4 | input regions | **PARTIAL** | = A-06 | the region API |
| S-5 | cursor shape set | **PARTIAL** | = B-12 | the named set |
| S-6 | xdg_activation + startup notification | **MISSING** | no code | the whole item |
| S-7 | X11 DPI awareness | **DONE** [VERIFIED-CHANGED: audit said missing] | = B-07 (1.4.5) | — |
| S-8 | AT-SPI2 level 1 | **MISSING** | = B-10 | roles/states/focus over the D-Bus client |
| S-9 | drag icon + drop targets | **MISSING** (icon) / **PARTIAL** (targets) | = B-13 | both halves |
| S-10 | widget residue | **PARTIAL** | = B-18 (editable combo done) | the date picker |
| C-1 | portal FileChooser | **MISSING** | = B-09; **CORE** per D-002 | the fallback |
| C-2 | AT-SPI text interfaces | **MISSING** | = B-10 | after S-8 |
| C-3 | color emoji | **MISSING** | no COLRv0/v1 | after M-2 |
| C-4 | animated images | **MISSING** | = B-14 | GIF first |
| C-5 | re-anchor + auto-hide | **MISSING** | = A-05/A-08 | both |
| C-6 | basic gestures | **MISSING** | = B-17 | after M-3 |
| D-1 | EGL/GPU Wayland | **DEFERRED** | by scope discipline (1.2.0) | — |
| D-2 | subsurfaces | **DEFERRED** | by scope discipline | — |
| D-3 | tablets / stylus | **DEFERRED** | by scope discipline | — |
| D-4 | foreign-toplevel | **DEFERRED** | by scope discipline | — |
| D-5 | xdg session save/restore | **DEFERRED** | by scope discipline | — |

---

## Part A — fdk-shell vs the layer-shell world

What 0.1.0 already does (verified in `fdk-shell/src/`, pinned by
`tests/test_x11.c`, `tests/test_wl.c`, `tests/bar_rig.sh`):

- **Layers** background / bottom / top / overlay — protocol truth on
  Wayland (wlr-layer-shell v4, version-clamped, on-demand keyboard
  degraded honestly on pre-v4 compositors); EWMH mapping on X11
  (dock type + per-level `ABOVE`/`SKIP_*`, sticky pre-map AND
  post-map, the fullscreen-over-docks ceiling documented).
- **Anchors / margins / exclusive zone** — all four edges, OR-combined,
  corner and centered cases; zone `-1` (auto) / `0` / `>0`; the
  wlroots XOR-anchoring reservation rule mirrored in
  `_NET_WM_STRUT_PARTIAL` (plus the legacy 4-card strut).
- **Keyboard interactivity** — none / exclusive (a real `XGrabKeyboard`
  on X11) / on-demand (v4 on Wayland; best-effort focus on X11).
- **Configure negotiation** — ack + `notify_configure` into FDK, with
  the present gate (no commit before the first configure; the labwc/
  sway protocol-killer is structurally impossible now).
- **Per-output selection** (Wayland; index or "every output"), output
  enumeration with connector names, output-removal → `closed` →
  close-request delivery.
- **Popups on any layer** — full grab/dismissal contract, the
  pointer-only grab lesson, desktop-switch dismissal, parent sweep.
- **Fractional scaling / HiDPI** — adopted surfaces get
  `wp_fractional_scale_v1` + output-enter scale tracking at adoption
  (`wayland_ext.c`), same as windows.
- **Hover cursors** (arrow / hand, re-applied on enter), self-
  positioned dock hints (`USPosition`), EXPOSE repaint contract,
  five-family packaging with a versioned FDK dependency, the
  prefix-isolation build proof, and the WM-side contract for a true
  overlay layer (the three-step private-atom spec in the README).

### fdk-shell gap register

| id | gap | verdict | effort | note |
|----|-----|---------|--------|------|
| A-01 | **Output events** — no notification when outputs are added / removed / resized. The registry listener tracks it; the app can only poll `fdk_shell_output_count`. A panel cannot react to a monitor being plugged in. | missing | S–M | the data exists (wl.c keeps live records); an event callback is the missing piece |
| A-02 | **Output metrics** — no size / scale / position query per output. Apps need this to size panels per-output before the first configure, and it is the substrate A-06 (X11 multi-monitor) builds on. | missing | S | pair with A-01 |
| A-03 | **X11 multi-monitor** — exactly one output (the screen); no RandR. Per-monitor docks are impossible; a top bar spans all monitors with one strut. | missing | M | RandR 1.3 output enumeration + CRTCs; hotplug = RRNotify; per-output geometry feeds the same anchoring math |
| A-04 | **Namespace** — hardcoded `"fdk-shell"`; the spec's namespace is what compositor rules (sway `for_window`-style layer criteria) match on. gtk-layer-shell exposes `set_namespace`. | partial | S | one desc field |
| A-05 | **Runtime parameter changes** — `set_layer` / `set_anchor` / `set_margin` / `set_exclusive_zone` / `set_size` / `set_keyboard_interactivity` are all runtime-capable in the protocol (double-buffered state); fdk-shell exposes them create-time only (destroy + re-create is the documented idiom). | partial | M | the protocol allows live re-anchor without losing the surface; gtk-layer-shell allows pre-map changes of everything |
| A-06 | **Input regions / click-through** — no `wl_surface.set_input_region` on Wayland, no XShape on X11. Transparent margins of a notification overlay eat clicks; auto-hide reveal strips are impossible without it. | missing | S–M | needs a small extension-API addition (A-07) or an fdk-shell-side call on the native handle |
| A-07 | **Extension-API surface hooks for input+scale region control** — the add-on can already do both itself through the native handle (documented accessors); an ext-API v2 helper would make it uniform. Judgment: do it add-on-side first. | missing | S | append-only ops fields **[VERIFIED-CHANGED: the add-on-side-first judgment is SUPERSEDED - S-4 owns input regions core-side (windows AND extension surfaces) per the 2026-10-27 owner decision, docs/DECISIONS.md D-002]** |
| A-08 | **Auto-hide / reveal** — no API. Possible today only via destroy/recreate churn (Wayland) or hide (X11); a real answer needs A-06 plus zone animation. | missing | M | panels want it; gtk-layer-shell doesn't provide it either (it is panel-toolkit territory, and fdk-shell IS the panel toolkit here) |
| A-09 | **`_FDK_WM_LAYER` property** — the README's custom-WM contract (option 2) says fdk-shell "would set it the day a WM adopts it"; setting it unconditionally is harmless on WMs that don't know it and lets the owner's WM adopt without an fdk-shell release. | partial | S | property + README note |
| A-10 | **Cascading submenus** — core popups parent to any extension surface (verified: `fdk_ext_popup_create` → `fdk__window_create_popup_ex` accepts a popup parent), but fdk-shell's API only takes a LAYER parent, so bars can't build launcher menus with submenus. | partial | S | expose popup-of-popup in the fdk-shell API |
| A-11 | **StatusNotifier tray host** — no StatusNotifierItem/Watcher. A panel without a tray is half a panel; this is the single most-requested panel feature after the bar itself. Rides the D-Bus foundation (B-08). | missing | M–L | fdk-shell add-on surface + fd sources is exactly the architecture for it | **[DONE 2026-10-27: trayhost — should work; owner verification pending (docs/TESTING-ON-DESKTOP.md)]** |
| A-12 | **Notification surfaces** — no org.freedesktop.Notifications client. The overlay layer exists; the protocol client doesn't. Rides B-08. | missing | S–M | natural fdk-shell example (OSD/notification shell) | **[DONE 2026-10-27: notifyd — should work; owner verification pending (docs/TESTING-ON-DESKTOP.md)]** |
| A-13 | **Wayland outside-click dismissal** — machinery exists (xdg_popup grab, menu-tested) but is untested against a real seat serial; headless rigs can't produce one. | partial | S | owner's machine verification; rig with wlroots virtual pointer can synthesize real serials |

Not gaps (checked, fine): exclusive-zone edge semantics including the
partial-strut ranges; keyboard on-demand downgrade honesty; surface
lifecycle when an output disappears (closed → close request);
lifecycle when the *compositor* disappears (bar rig kills it);
popups on overlay surfaces; app_id/namespace collision behavior;
multi-shell (one per context, second init refused); docs quality.

---

## Part B — FDK core vs a complete toolkit (GTK4 / Qt6 reference)

What is already present and tested (the short version — each of these
has an API surface, tests, and docs in the tree):

- **Windows**: create/configure/close, maximize/minimize/fullscreen,
  decorations (xdg-decoration negotiation + CSD title band +
  interactive move/resize through `xdg_toplevel.move` /
  `_NET_WM_MOVERESIZE`), popups with grab + dismissal, title +
  app_id/WM_CLASS identity, multi-window, event callbacks, shortcuts
  (spec parse/format, window accelerators, menu accelerators).
- **Rendering**: software surface API — fills, rects, lines, circles,
  rounded rects (aliased + AA), vertical gradients, clip stack, blit
  / blend / transformed blit, damage tracking, PNG encode, PNG
  screenshot-verified examples; the X11 GPU lane (GLX, runtime
  dlopen'd, per-window context, size-aware lane policy, three-verdict
  self-check).
- **Text**: stb_truetype glyph rasterization + cache, metrics,
  measure/draw single-line LTR with pair kerning, markup, layout
  helpers (wrap/ellipsize/align), system font discovery via
  runtime-dlopen'd fontconfig, ttc first-face, untrusted-font
  validation.
- **Widgets** (33 create functions + dialogs): label, button, toggle,
  checkbox, radio, progress, separator, frame, entry (selection, undo,
  clipboard, preedit-render groundwork), scrollview, list, iconview,
  textview (editable multiline, 256 KiB cap, undo), picture, tree,
  slider, spin, toolbar, notebook, canvas, menu + menu bar + submenus,
  combo, spinner, paned, expander, statusbar, search entry, levelbar,
  revealer, stack + switcher, menu button, tooltip; dialogs: message,
  prompt, open/save file (icon/list views, filters), about, font
  chooser, color chooser; public generic undo stack; layout engine
  (box + grid); themes (own format, discovery, shipped faces, live
  XSETTINGS icon-theme following, icon themes incl. SVG via nanosvg).
- **Clipboard**: CLIPBOARD + PRIMARY on both backends; UTF-8 text +
  image/png; ICCCM TARGETS on X11, wl_data_device on Wayland.
- **DnD**: receive + source on both backends (XDND / wl_data_device),
  text + URI lists, percent-decoding, honest failed-transfer delivery.
- **i18n**: own catalog format (contexts, CLDR plurals), locale
  parsing, number/currency/percent/date/time formatting, plural
  operands, CLI tools; docs/i18n.md.
- **A11y (internal)**: role/name/description tree, subscribe/notify,
  actions, text-at-offset/caret/selection, relations, virtual
  containers, keyboard navigation, the narrator with its own catalog.
- **Animation**: tick/done API with easing; Wayland frame-callback
  pacing; X11 motion coalescing + release-wait collapse + startup
  trace (`FDK_STARTUP_TRACE`).
- **HiDPI (Wayland)**: fractional-scale-v1 + viewporter + output
  enter/leave scale tracking, for windows AND extension surfaces.
- **Prefs / session state**: typed key-value store per app_id, save/
  load, documented format + CLI tools.
- **Extension API v1**: kinds, adoption, event routing, render path,
  popups + present gate, fd loop sources, capability queries,
  documented native accessors; fdk-shell as the reference consumer.
- **IME groundwork** (widget level): preedit string API, inline
  preedit rendering with underline at the caret — the platform layer
  just doesn't drive it yet (B-01).

### FDK core gap register

| id | gap | verdict | impact on real apps | effort | ABI risk |
|----|-----|---------|---------------------|--------|----------|
| B-01 | **Wayland IME — no text-input-v3.** The seat binds pointer + keyboard only (`wayland_seat.c`). On X11, XIM runs `PreeditNothing\|StatusNothing` (compose keys work; no preedit). | partial (X11) / missing (Wayland) | CJK/input-method users cannot type on Wayland at all — the single largest population-level break in the toolkit | M | none — new `FDK_EVENT_IME_*` types append to the numbered enum; the union is delivered by pointer and never app-allocated |
| B-02 | **Text shaping — no HarfBuzz, no FriBidi, no per-script font fallback.** Shaping v1 is codepoint-at-a-time LTR with pair kerning (docs/text.md); unmapped codepoints render `.notdef`; one font per draw call. | **DONE (level 1)** [VERIFIED-CHANGED: M-2 shipped — the dlopen'd pipeline (shaping.c) with bidi reordering, GSUB/GPOS shaping, mark attachment, per-script charset-filtered fallback; Entry/TextView/Label/markup/Entry-caret all consume shaped placements; Latin metrics pinned identical across postures; `fdk_text_shaping_available()` + `fdk_font_has_glyph()` appended (797 exports)] | Arabic/Hebrew/Persian/Urdu now render in visual order with joining forms and ligatures; mixed-script strings pick fonts per segment; level 2 (paragraph bidi, RTL caret granularity, justification, emoji) open | M–L | none — as audited; 2 functions appended |
| B-03 | **Touch input — entirely absent.** No `wl_touch` binding (Wayland), no XI2 touch (X11); the event stream has no touch types. | missing | touchscreen laptops: no tap, no scroll, no drag on Wayland (wlroots does not emulate a pointer); X11 survives only via the server's touch→pointer emulation | M | none — `FDK_EVENT_TOUCH_*` append; `fdk_pointer_button_event`-style appended fields |
| B-04 | **Taskbar/window identity — no icons.** No `_NET_WM_ICON` (X11), no xdg-toplevel-icon-v1 (Wayland); no `_NET_WM_PID` / `WM_CLIENT_MACHINE` / `WM_WINDOW_ROLE`; no startup notification / xdg_activation. | missing | every FDK app shows a generic taskbar icon; docks/panels can't match windows to apps; no focus-stealing protection | S (icons/PID) / M (activation) | none |
| B-05 | **X11 multi-monitor / RandR — absent in core too.** One screen; no output model, no RRNotify. (Wayland core: outputs tracked, scale follows; no app-facing output API.) | missing | multi-monitor X11 desktops: windows span/clamp to the whole screen; per-monitor behavior impossible | M | none |
| B-06 | **Dark mode / system theme following — icon theme only.** XSETTINGS reads exactly `Net/IconThemeName`; no `Gtk/ColorScheme` (X11), no org.freedesktop.appearance color-scheme (Wayland). | partial | apps don't follow the desktop's light/dark preference — a visible polish break on every themed desktop | S–M | none **[VERIFIED-CHANGED: CLOSED - XSETTINGS Gtk/ColorScheme AND the org.freedesktop.appearance portal via the native D-Bus client shipped; live-proven by the portal color-scheme legs in make test-dbus]** |
| B-07 | **X11 DPI/HiDPI awareness — none.** No Xft.dpi, no RandR scale awareness; text renders at 1x. (Wayland is fully covered.) | missing | HiDPI X11/XLibre users get tiny UI | M | none **[VERIFIED-CHANGED: the audit row was WRONG - Xft.dpi + physical-size detection with integer scale shipped in 1.4.5; test_x11_hidpi_gui proves it end to end on a 192-dpi Xvfb]** |
| B-08 | **The D-Bus foundation.** No D-Bus client of any kind (by design, zero-dependency). Blocks: dark mode on Wayland (B-06), tray (A-11), notifications (A-12), portals (B-09), AT-SPI (B-10), idle inhibition on X11 (B-11). | missing | one architectural decision gates five features | L | none — new module, dlopen posture like GLX/fontconfig (libdbus-1.so.3 present in the target sandbox) **[VERIFIED-CHANGED: CLOSED by M-7 - implemented NATIVELY in raw C from the spec; no libdbus dlopen at all, no new dependency of any kind (docs/DECISIONS.md D-001); the dlopen speculation above predates the decision and never applied]** |
| B-09 | **Portal dialogs** — own widgets everywhere; no xdg-desktop-portal FileChooser fallback. | partial | functionally fine everywhere; required only under Flatpak-style sandboxes | M | none **[VERIFIED-CHANGED: the fallback is CORE work on the native D-Bus client per the 2026-10-27 owner decision (D-002), not a Flatpak-only nicety; verdict stays MISSING until it lands]** |
| B-10 | **AT-SPI2 interop** — internal a11y tree exists; Orca and other OS screen readers cannot see FDK apps (no D-Bus). Deliberate no-daemon-dependency posture so far. | missing | blind users on real desktops get nothing from FDK apps even though the internal tree is rich | L | none |
| B-11 | **Idle/screensaver inhibition** — no org.freedesktop.ScreenSaver, no idle-inhibit-unstable-v1. | missing | video players keep the screen blanking mid-play | S–M | none |
| B-12 | **Cursor shape set** — 2 shapes (arrow/hand) + per-edge resize cursors; Wayland rides cursor-shape-v1 (all names available), X11 uses core `XCreateFontCursor` (no themes). | partial | standard shapes (text beam, wait, crosshair, grab) missing from the public surface | S | none |
| B-13 | **Drag icons + widget-level drop targets** — DnD is window-level (apps hit-test); no drag icon on either backend. | partial | usable but visibly rough; file managers do manual hit-testing today | S (icon) / M (targets) | none |
| B-14 | **Image formats** — stb_image decode (JPEG/PNG/BMP/TGA/PSD/GIF-static/HDR/PIC/PNM) + PNG encode + nanosvg. No animated GIF/WebP/APNG/AVIF; SVG without filters/masks/text. | partial | static raster + SVG solid; animated content missing | S–M | none |
| B-15 | **GPU rendering on Wayland** — CPU SHM only; the GL lane is X11/GLX. | partial | Wayland renders software (the toolkit's design center; the GL lane was an X11 perf answer) | L | none |
| B-16 | **Subsurfaces** — no wl_subsurface; no X11 child-window compositing. | missing | video-in-UI, offscreen compositing overlays | M | none — would ride extension API v2 |
| B-17 | **Tablets / stylus / gestures** — no zwp_tablet, no pinch/swipe anywhere. | missing | drawing tablets dead; pinch-zoom absent | M–L | none |
| B-18 | **Widget-set residue** — no date/time picker, no editable combo; everything else on the GTK4 checklist has an FDK counterpart (menus, lists, trees, tables via grid/tree, text editing with selection and undo, tooltips, pickers, scroll views, tabs). | partial | small, visible gaps | S each | none **[VERIFIED-CHANGED: the editable combo SHIPPED - fdk_combo_set_editable; only the date/time picker remains]** |
| B-19 | **Session restore protocols** — prefs cover app state; no xdg_session/WM_SAVE_YOURSELF. | partial | niche on the Linux desktop today | M | none |

Also checked and fine: fractional scaling plumbing for extension
surfaces; clipboard manager interop basics; keymap handling (xkbcommon
on Wayland, evdev-offset contract both backends); key repeat;
scancode/codepoint dual identity; decoration negotiation; CSD drag;
expose/repaint contracts; startup tracing; the five-family packaging
and CI matrix; security posture (bounded reads, validated fonts,
64 KiB caps); threading model (documented UI-thread-only).

---

## The proposed 1.1.0 scope

### MUST — blocks calling 1.1.0 complete

| # | item | gaps closed | effort |
|---|------|-------------|--------|
| M-1 | **IME, both backends**: bind text-input-v3 (enable/surrounding-text/commit/preedit/deactivate on focus changes), deliver new IME events, wire Entry/TextView preedit (the groundwork API already renders it); X11: XIM preedit callbacks (`XIMPreeditCallbacks`) driving the same events. | B-01 | M |
| M-2 | **Text shaping level 1**: HarfBuzz via runtime dlopen (the GLX/fontconfig posture) + FriBidi (dlopen) + per-script font fallback chain through fontconfig; Entry/TextView/Label/markup all draw shaped runs. Color emoji explicitly NOT in M-2 (see C-3). **[DONE — shipped with the placement seam (one shaped structure every consumer reads), the 26.6-scaled stb-backed hb font funcs, cluster-attributed wrap/ellipsize, the shaped Entry caret + hit-test, and tests/test_shaping.c running both postures]** | B-02 | M–L |
| M-3 | **Touch basics**: wl_touch binding → touch events (slots, tap → click synthesis policy documented, two-finger scroll); X11 XI2 touch via dlopen'd libXi with the same event model. Gestures/tablets stay deferred. | B-03 | M |
| M-4 | **Window identity**: `_NET_WM_ICON` + xdg-toplevel-icon-v1 (set both, harmless where unsupported) + `_NET_WM_PID` + `WM_CLIENT_MACHINE` + `WM_WINDOW_ROLE`; `fdk_window_set_icon` API. | B-04 (icons/PID part) | S |
| M-5 | **fdk-shell outputs**: **DONE (rc2, fdk-shell 0.2.0)** — live output events (ADDED/REMOVED/CHANGED; proven against sway's create_output/mode/unplug IPC) + per-output metrics (position, logical size, scale, physical mm — fdk_shell_output_get_info); configurable namespace (Wayland's zwlr namespace / X11's WM_CLASS res_class); `_FDK_WM_LAYER` set on every layer. X11 side: the screen as one output with real metrics; per-output X11 records land with M-6's RandR. | A-01, A-02, A-04, A-09 | M |
| M-6 | **X11 multi-monitor**: RandR output enumeration + change events in core (app-facing output API, shared with M-5's fdk-shell surface) + per-output docks in fdk-shell. | A-03, B-05 | M |
| M-7 | **The D-Bus foundation + dark mode**: **DONE (the foundation, rc2)** — a native client in raw C from the public specification (src/dbus/, 115 exported symbols), zero new dependencies of any kind, riding the FDK event loop; wire bytes pinned against captured reference-implementation output; fuzzed in `make test`; live-bus suite `make test-dbus` (real dbus-daemon: calls, errors, timeouts, match rules/signals, properties, name watching, introspection, close-from-callback reentrancy; CI makes skips fatal). Dark-mode following rides on top (next). **[VERIFIED-CHANGED: it no longer rides on top - dark mode (B-06) LANDED at d2a253f; M-7 is complete]** | B-08, B-06 | L |
| M-8 | **fdk-shell popup cascades**: **DONE (rc2, fdk-shell 0.2.0)** — fdk_shell_popup_create_child, any depth (xdg_popup-on-xdg_popup / the X window tree), family teardown sweeps children with their parent (the re-scan sweep — a saved-next chain dangles). | A-10 | S |
| M-9 | **The release gate**: every MUST lands with its rigs in this repo's style (in-test compositors/WMs, pixel readbacks, protocol traces), CHANGELOG + EXTENDING.md + README updates, extension API v2 bump if ops grow, five-family packaging version bumps, full local battery + CI green — and the owner's sign-off before any tag. | — | S |

### SHOULD — the polish tier (land if the schedule holds after MUST)

| # | item | gaps closed | effort |
|---|------|-------------|--------|
| S-1 | **StatusNotifier tray host** in fdk-shell (rides M-7's D-Bus) | A-11 | M–L | **[DONE 2026-10-27: trayhost, the rig, D-007 — should work; owner verification pending]** |
| S-2 | **Notification display** — org.freedesktop.Notifications client as an fdk-shell example/add-on on the overlay layer | A-12 | S–M | **[DONE 2026-10-27: notifyd, the rig, D-006 — should work; owner verification pending]** |
| S-3 | **Idle inhibition** — idle-inhibit-unstable-v1 (Wayland) + org.freedesktop.ScreenSaver (X11, rides M-7) | B-11 | S–M |
| S-4 | **Input regions / click-through** — wl_surface.set_input_region + XShape, exposed on windows and extension surfaces | A-06, A-07 | S–M |
| S-5 | **Cursor shape set** — full named set over cursor-shape-v1 + core font cursors | B-12 | S |
| S-6 | **xdg_activation + startup notification** | B-04 (activation part) | M |
| S-7 | **X11 DPI awareness** (Xft.dpi + RandR scale) | B-07 | M |
| S-8 | **AT-SPI2 level 1** — roles/names/states/focus/activate over M-7's D-Bus; text interfaces later | B-10 | L |
| S-9 | **Drag icon + widget-level drop targets** | B-13 | S+M |
| S-10 | **Widget residue** — date picker, editable combo | B-18 | S each |

### COULD — if the MUST/SHOULD tiers close early

| # | item | gaps closed |
|---|------|-------------|
| C-1 | Portal FileChooser fallback | B-09 |
| C-2 | AT-SPI text interfaces + relations export | B-10 |
| C-3 | Color emoji (COLRv0/v1 after HarfBuzz lands) | B-02 |
| C-4 | Animated images (GIF first, WebP via dlopen) | B-14 |
| C-5 | fdk-shell runtime re-anchoring + auto-hide/reveal mode | A-05, A-08 |
| C-6 | Basic gestures (pinch/swipe) on the touch event model | B-17 |

### DEFER — 1.2.0, by scope discipline

| # | item | gaps closed |
|---|------|-------------|
| D-1 | EGL/GPU lane on Wayland | B-15 |
| D-2 | Subsurfaces (needs extension API v2 parent-surface kinds) | B-16 |
| D-3 | Tablets / stylus | B-17 |
| D-4 | foreign-toplevel management | — |
| D-5 | xdg session save/restore protocols | B-19 |

---

## ABI impact

**Nothing in this roadmap forces an ABI break.** Every gap closes
append-only, which docs/abi-policy.md already governs:

- New event types (IME, touch) append to the numbered `fdk_event_type`
  enum; new payloads append to the `fdk_event_data` union, which is
  always delivered by pointer from FDK and never allocated by
  applications — growth is binary-compatible for old callers.
- New APIs (icons, outputs, input regions, D-Bus surfaces) are new
  symbols on `libfdk.so.1`; the SONAME does not move.
- The extension API may grow ops fields (input-region hook); that is
  the documented `struct_size` append pattern and bumps
  `FDK_EXT_API_VERSION` to 2 — old add-ons (fdk-shell 0.1.0 binaries)
  keep working, exactly the compatibility case the versioning was
  designed for.
- `fdk_shell_layer_desc` grows (namespace, and whatever A-05 needs)
  behind its `struct_size` — same rule, no rebuild required for old
  callers.

The honest ABI caution in the whole plan: M-2 (shaping) changes what
pixels text produces. That is a rendering-behavior change inside the
same API, not an ABI change — but golden-image tests and the CJK
probe corpus should be re-baselined deliberately, and theme authors
should be told text metrics can shift by a pixel.

## What this audit could NOT assess from the sandbox

- **IME end-to-end**: no fcitx5/ibus daemon here. text-input-v3 can be
  tested against a scripted compositor-side peer (the sway headless
  rig can speak the server side), and XIM preedit against a synthetic
  XIM server — but real-input-method behavior needs the owner's
  desktop (labwc + fcitx5 recommended).
- **Real portal/AT-SPI daemons**: not installed. The D-Bus layer can
  be tested against `dbus-run-session` (present here) with mocked
  services; real xdg-desktop-portal and at-spi2 daemons are not
  available.
- **NVIDIA GL-lane behavior** (the 1050 Ti): only the owner's machine
  exercises the real driver path; the sandbox covers Xvfb/software GL.
- **Multi-monitor hardware**: emulated only — sway headless can create
  multiple outputs (wl side testable); Xvfb RandR emulated CRTCs give a
  partial X11 story. Real hotplug unplug-replug timing needs hardware.
- **Touch hardware**: XI2 events can be synthesized; wl_touch needs a
  compositor-side injector (wlroots virtual-pointer style rig).
- **CI status at the audit base** was not re-verified during the audit
  (API rate-limited anonymously); the push of this document includes
  an authenticated check. **[VERIFIED-CHANGED 2026-10-27: the
  verification pass DID check CI authenticated — and found the base's
  own run red (all five packaging jobs at d2bc0de: fdk-shell's
  hand-maintained Makefile version). Root-caused and fixed at
  4d86464; two further CI defects found and fixed the same day (the
  present-then-readback race that flaked alpine, and the void
  mirror outage). Job-level green verified per push since.]**
- **[VERIFIED-CHANGED 2026-10-27]** Two premises above are stale: the
  headless rigs CAN produce real input serials now (the
  fdk-wl-inject virtual-pointer/keyboard rig — A-13 was verified
  against sway with them), and labwc IS runnable in the sandbox
  (bar rig A3/A4 legs pass on it). Real-input-method (fcitx/ibus),
  real portal/AT-SPI daemons, NVIDIA, physical hotplug, and touch
  hardware still need the owner's desktop — TESTING-ON-DESKTOP.md
  is the checklist.

## The release bar, restated

1.1.0 gets called complete when the MUST tier is green in CI and on
the owner's two real desktops (labwc/Wayland, Openbox+NVIDIA/X11),
the SHOULD tier is either green or explicitly cut by the owner, and
the CHANGELOG reads like the audit above never had to say "missing"
about anything a real application hits in daily use. Until then the
version stays `1.1.0-rc2` (rcN bumps with the candidate's own
history - docs/versioning.md), no tags, no releases.
