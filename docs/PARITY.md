# FDK vs GTK4 / Qt6 — the parity audit

**Purpose.** The owner's requirement: a developer must be able to
build ANY desktop application with FDK and not feel the toolkit's
limits — the API surface GTK4 and Qt6 Widgets cover, FDK covers
too, and where a category is deliberately thinner the reason is
stated here, not hidden. This audit enumerates what FDK ships
(797 exported symbols, 36 widget classes, both backends), compares
it category by category against GTK4 and Qt6 Widgets, and lists
the gaps with a priority verdict. It was produced by reading
FDK's headers and sources directly — every "have" claim below is a
real, exported, tested API, not a plan.

**How to read the verdicts.**

- **PARITY** — the category is covered at the same conceptual
  depth as GTK4/Qt6 (the spelling may differ).
- **PARITY (different shape)** — covered, but FDK solves it with a
  deliberately different mechanism; the difference is design, not
  absence.
- **PARTIAL** — the core is there, named sub-features are not.
- **GAP** — GTK4/Qt6 have it, FDK does not.

---

## 1. Core object model

**GTK4**: GObject (refcounting, signals, properties, interfaces).
**Qt6**: QObject (parent ownership, signals/slots, properties,
moc).
**FDK**: plain C structs + create/destroy + explicit event
callbacks; no inheritance, no reflection, no signal marshalling.

**Verdict: PARITY (different shape), by design.** FDK is a C
toolkit without a runtime object system; the cost and the gain are
both real (no property/signal introspection; also no GObject
boilerplate, no moc, smaller binaries). This is the root
difference every other "different shape" verdict traces back to.

## 2. Windows, surfaces, backends

- Windows: create/destroy, show/hide, title, icon, size limits,
  resizable, decorated, minimize/maximize/fullscreen/restore with
  state queries, activate + activation tokens (xdg-activation),
  per-window shortcuts, input regions, cursor shapes, decoration
  font, popup windows with grabs. **PARITY.**
- Render surfaces: framebuffer get/present, damage tracking
  (invalidate, bounds), clip stack, save-PNG, offscreen surface
  creation (from image/bytes/format). **PARITY** (GPU lane with
  automatic CPU fallback; GTK4's GSK / Qt6's RHI are scenegraph
  renderers — FDK is immediate-mode over a framebuffer, see §9).
- Backends: X11 and Wayland, chosen at runtime, both first-class
  (one test suite each). GTK4/Qt6 additionally ship Windows/
  macOS. **PARTIAL — by scope**: this is a Linux desktop toolkit;
  a third backend is a project, not a gap to patch.

## 3. Widgets

36 classes: button (4 roles: normal/suggested/destructive/link),
toggle, checkbox, radio, menu + menu bar + menu button (4 arrow
variants, check/radio/separator items), entry (+search entry, 7
built-in icon slots), textview, label (wrap/nowrap/ellipsize),
picture (fill/contain/cover fits), spinner, progress, levelbar
(continuous/discrete), slider, spin, scrollbar via scrollview,
separator, frame, expander, notebook, stack + stackswitcher,
paned, toolbar, statusbar, list (single/multi/none selection),
tree, iconview, calendar, date picker, time picker, combo, canvas.

**GTK4 widget inventory diff**: missing are `GtkAssistant`
(deliberate policy: OUT), `GtkActionBar` (toolbar + custom
containers cover the layout), `GtkFixed` (FDK containers position
explicitly — same role), `GtkColorChooserWidget`/
`GtkFontChooserWidget` (the DIALOGS exist —
`fdk_dialog_choose_color/font` — the inline widget form does
not; choosers.c ships some inline chooser machinery),
`GtkScaleButton`, `GtkVolumeButton`, `GtkRecentChooser`,
`GtkAppChooserWidget`. The GRID container exists (1.4.x:
`fdk_grid_create/_attach` with colspan/rowspan,
row/column expand, homogeneous, spacing, padding — the GtkGrid
feature set; corrected 2026-11-01, an earlier revision of this
document wrongly listed grid as a gap). **PARTIAL** — the
every-application set is complete; the chooser-widget forms are
the honest missing tail.

**Qt6 Widgets diff**: Qt's ~180 classes include a rich
item-view/MVC layer (QTableView, QHeaderView, QDataWidgetMapper,
proxies/sorting-filtering models). **§23/B2e CLOSED the model
half**: fdk_model.h ships the row store (stable ids, change
signals, forests), the SORT and FILTER proxies with model faces,
and live List/Tree bindings whose selection maps by row id
(reading through sort/filter keeps what was selected selected)
plus the reorder write-back. **PARTIAL** on the remaining tail:
no table-widget with per-column headers (a List per column and
the model cover it), no QDataWidgetMapper (form-to-model
binding), no drag-reorder GESTURE on the widget (the
fdk_list_bound_reorder_selected API is the model-side half the
app wires to its drag handler).

## 4. Text and fonts

- Rendering: HarfBuzz shaping + FriBidi bidi + per-script font
  fallback (the M-2 pipeline), ligatures, clusters (selection
  geometry is cluster boxes), emoji (dedicated emoji.c), CFF and
  TTF via stb-truetype, font enumeration + loading from
  file/system default, metrics, glyph queries, cache stats.
- Markup: `<b> <i> <u> <s> <color=#rrggbb[aa]>` with CSS-style
  innermost-wins color nesting, entity escapes.
- Layout: measure/shaping spans, layout.c (line layout), entry
  preedit (IME inline composition).

**GTK4 (Pango) diff — §23/B2c CLOSED the named set**: fdk_attr.h
ships the attribute layer (size_pt/delta, per-run FONT FAMILY
resolved through fontconfig, background, the five Pango underline
styles each in its own color, strikethrough color, rise,
letter-spacing, links with hit-testing) + the attribute-list API
(fdk_attr_list) applied over ranges, flowing through the SAME
HarfBuzz/FriBidi pipeline (per-run fonts shape, kern, and
fallback correctly). Markup: `<span ...>`, `<sub>`, `<sup>`,
`<big>`, `<small>`, `<a href>` parse into it. **PARITY** on the
attribute set this round targeted; the remaining Pango tail
(baseline-shift, gravity, OpenType feature settings, font
stretch) is unexposed — stated, not hidden.

## 5. Styling and theming

- Theme engine: load/find/destroy themes, enumerated available +
  allowed sets, per-key color and metric getters, author/file
  metadata, default-theme creation, icon theme rendering,
  system color-scheme query (portal), text-scale query.
- Theme format: FDK's own settings-file themes (`'Solar Flare'`
  et al.).

**GTK4 (CSS) / Qt6 (QSS)**: both style via cascading stylesheets
with selectors, state pseudo-classes, and custom CSS properties.
FDK's themes are token sets (colors + metrics), not selectors.
**PARTIAL — by design**: token themes keep FDK tiny; the cost is
no per-widget-class restyling without code. An application that
needs arbitrary restyling cannot express it in a theme file.

## 6. Layout containers

Box (H/V), GRID (attach with colspan/rowspan, row/column expand,
homogeneous, spacing, padding), flow containers, scrollview,
notebook, stack, paned, frame, expander, toolbar, plus explicit
positioning. **GTK4 diff**: no GtkConstraintLayout (the grid and
box cover the practical layouts). **PARTIAL** (corrected
2026-11-01: grid has shipped since 1.4.x — an earlier revision of
this document wrongly listed it as missing).

## 7. Input

Pointer (enter/leave/motion/button, multi-button), keyboard
(keymap, keysyms, per-window shortcuts), wheel, TOUCH with slot
tracking (TOUCH_DOWN/MOTION/UP/CANCEL per contact), synthesized
tap + two-finger scroll, PINCH and N-finger SWIPE gesture events
(the touch engine, identical on both backends), text-input-v3 IME
(preedit + commit, both backends), drag-and-drop (begin/begin_ex
with drag icons, per-widget drop targets, per-window format
masks, TEXT + URI_LIST with decoded POSIX-path payloads, INCR-
capable drops), clipboard (UTF-8 text, image PNG with INCR for
large payloads, PRIMARY selection on both backends), tablets/
stylus (proximity, pressure, tilt), XTEST-grade synthetic input
in the test rig, per-window input regions with click-through.

**§23 closed both named diffs.** Gestures: fdk_gesture.h ships
the RECOGNIZER layer (click with 1/2/3 counting, long-press with
delay_factor, drag begin/update/end, pan with the orientation
latch, swipe velocity+direction, zoom, rotate) fed the event
stream a window already receives, with claim semantics — on top
of the raw PINCH/SWIPE events the touch engine keeps
synthesizing. Clipboard: fdk_clipboard_set_uris/get_uris added
the text/uri-list surface on BOTH backends (the wire format, the
TARGETS/mime negotiation, a two-process verified rig). The
one-content-at-a-time model stands BY DESIGN (the documented
clipboard semantics — text, image, or uri-list, each replacing
the last), not as a gap. **PARITY** (different shape on the
multi-target point: GTK's simultaneous offers vs FDK's replace
model — stated, not hidden).

## 8. Accessibility

Roles for every widget class (63 roles incl. dialog, calendar,
canvas, tree-item), states (checked/expanded/busy/selected/...),
relations (labelled-by, controller-for, ...), actions (activate/
increment/...), events (bounds/caret/name/children/value changed),
AT-SPI bridge live on both backends, text offsets (char/word/
line). **PARITY — this is GTK4/Qt6-level coverage** (AT-SPI2 is
the same substrate both use on Linux; Qt's Windows/macOS backends
have no FDK equivalent by scope).

## 9. Rendering architecture

**GTK4**: GSK render lists/Vulkan/GL/NGL/Cairo. **Qt6**: RHI +
QPainter on raster. **FDK**: immediate-mode CPU framebuffer with
an optional EGL/GLX present lane (GPU compositing of the app's
buffer, driver-probed, CPU-fallback). **PARITY (different
shape)**: FDK does not build scenegraphs; for application UIs the
practical difference is very large batched-scene workloads. An
application doing heavy per-frame vector work draws on CPU.

## 10. Desktop integration

- D-Bus: a NATIVE wire client (fdk_dbus.h, 105 decls — bus,
  calls, signals, properties, watch; no libdbus linkage).
- Notifications (org.freedesktop.Notifications: send/withdraw/
  capabilities/status) plus the SERVER side (fdk-shell-overlay's
  notifyd), system tray host AND item (StatusNotifierItem, both
  directions), toplevel control (foreign-toplevel-management:
  list/activate/close), session manager (session save/restore,
  join/leave, command), idle inhibit, portal prefs (theme/scheme/
  text scale) and the xdg-desktop-portal FileChooser, activation
  tokens, drag-and-drop (both), window icons, app_id.

**Diffs**: no print dialog (GTK/Qt embed CUPS; FDK has nothing —
save-PNG + the file dialog is the workaround), no recent-files/
app-chooser portals. **PARTIAL — the desktop-shell client surface
is deep; printing is the one whole absent category.**

## 11. i18n

Catalogs (load/parse, in-context and plural forms — gettext
discipline), number/currency/percent/date/time formatters,
date arithmetic. **PARITY** with gettext-level i18n (ICU-grade
collation/normalization — Qt's default — is absent).

## 12. Concurrency, event loop, misc platform

Single UI-thread event loop with timers, fd sources (extension
API), nested pumps; threads are caller-owned (docs/threading.md).
Undo/redo stack, animation engine (easing, 4 decl families),
logging with levels, error codes, versioning + ABI policy docs,
prefs (key-value with source/recovery states), session state.

**Diffs**: no worker-thread pool integrated with the loop
(GIO's Task / QtConcurrent), no main-context dispatch from other
threads (a pipe + fd source is the documented pattern).
**PARTIAL — by design** (the single-thread rule is the
toolkit's spine; see docs/threading.md).

---

## The prioritized gap list — the §23 final status

| # | Gap (as this document last stated it) | Status |
|---|---|---|
| 1 | fdk-shell-overlay (shell surfaces) | **CLOSED** — the package (formerly `fdk-shell/`, renamed this round); X11 suite green in-sandbox, live re-anchoring + auto-hide + outputs + popups verified on Xvfb |
| 2 | Clipboard: image + URI-list formats | **CLOSED** — image since 1.4.9 + INCR 1.4.12; text/uri-list set/get added this round (§23/B2a-close), both backends, two-process verified |
| 3 | Gestures + multi-touch | **CLOSED** — touch slots + PINCH/SWIPE events since 1.0.0; the RECOGNIZER layer (click/long-press/drag/pan/swipe/zoom/rotate) added this round (§23/B2b) |
| 4 | Text on extension surfaces | OPEN (the next extension-API version: hosting widgets on fdk_ext_surface) |
| 5 | Pango-grade run attributes in markup | **CLOSED** this round (§23/B2c) — the named set; the deeper tail (gravity, features, stretch) stated above |
| 6 | Grid container | **CLOSED** (shipped 1.4.x; this document's earlier revision wrongly listed it — corrected this round) |
| 7 | Inline chooser widgets (color/font) | OPEN (dialogs exist; the widget forms do not) |
| 8 | Model/view split for tree/list | **CLOSED** this round (§23/B2e) — model + sort/filter proxies + live bindings + selection-by-id + reorder write-back |
| 9 | Print dialog / CUPS | **CLOSED** this round (§23/B2f) — own PDF writer (pdfinfo-verified), native IPP client (RFC 8010/8011, no libcups), dialog + preview |
| 10 | Windows/macOS backends | OUT by charter |

**The §23 audit pass** (the GTK4 4.24 widget catalog, fetched from
https://docs.gtk.org/gtk4/widget-gallery.html — the round's final
webfetch-driven sweep): against the catalog the remaining honest
tail is — InfoBar, CenterBox, SearchBar, ActionBar, StackSidebar,
EditableLabel, LockButton, EmojiChooser, DropDown, Video/
MediaControls, AppChooser/RecentChooser widgets, ShortcutsWindow.
Each is a composition of primitives FDK ships (InfoBar = a label
+ button row; SearchBar = an entry in a revealer; CenterBox/
Overlay semantics live in set_bounds' explicit positioning), so
they are CATALOG gaps, not capability gaps — but they are named
here rather than left for the next audit to rediscover. GLArea
and Assistant remain OUT by policy (PROGRESS.md's fence).

**Verdict in one paragraph.** FDK is at honest parity for the
application core — windows, input, 36+ widget classes,
complex-script text WITH range attributes, gesture recognizers,
the model/view split, AT-SPI accessibility, theming,
D-Bus/tray/notifications/session, i18n, undo, animation, shell
surfaces (the fdk-shell-overlay package), and now printing —
with the deliberate shape differences (no GObject/moc, token
themes, immediate-mode rendering, one-clipboard-content-at-a-
time) documented above. What remains open is stated in the table:
text-on-extension-surfaces, the inline chooser widget forms, the
deeper Pango attribute tail.

---

## fdk-shell-overlay/ — the shell-surface add-on package

`fdk-shell-overlay/` (formerly `fdk-shell/`) is the first surface
add-on package — the docs/EXTENDING.md model's reference consumer:
a `zwlr_layer_surface_v1` client for Wayland shells and an EWMH
dock client for X11, with its own build, version, pkg-config
file, packaging for five distros, and test suites.

- `fdk_shell_layer_create()` with a description struct (level,
  anchors, margins, exclusive zone, keyboard mode, desired size,
  output index), then LIVE setters — `fdk_shell_layer_set_level/
  set_anchor/set_margin/set_exclusive_zone/set_desired_size/
  set_keyboard_mode`, show/hide/reveal, auto-hide with edge
  behavior — an `fdk_ext_surface*` back, so rendering, events,
  input regions, popups and subsurfaces ride the extension API
  unchanged.
- Output enumeration (`fdk_shell_output_count/name/get_info`)
  plus a hotplug callback; per-output layer surfaces (the
  shell-panel shape) or one output pinned by index. X11 reads the
  RandR monitor table.
- Keyboard modes NONE/EXCLUSIVE/ON_DEMAND (ON_DEMAND is the
  layer-shell v4 semantic; the package binds the highest version
  the compositor offers and degrades with documented behavior
  below v4).
- Popup surfaces (`fdk_shell_popup_create/_create_child`), a
  notification server (`fdk_shell_notify_*`), a tray host
  (`fdk_shell_tray_*`) — the add-on is itself a small desktop
  shell stack.

**Verified in this sandbox**: the package builds warning-clean
after the rename and its full X11 suite passes (output info and
per-output anchoring, RandR probing with stale-index fallback,
3-deep popup cascades, family sweep). The Wayland suite and the
distro builds are exercised in CI (should work here — no
compositor in the sandbox). See its own README and
fdk-shell-overlay/packaging for the full inventory.

---

## Sources (the §23 round's research — fetched, not remembered)

- Pango markup attribute reference (the B2c vocabulary):
  https://docs.gtk.org/Pango/pango_markup.html
- GTK4 gesture model + long-press default (the B2b thresholds):
  https://docs.gtk.org/gtk4/class.Gesture.html
  https://docs.gtk.org/gtk4/property.Settings.gtk-long-press-time.html
- wlr-layer-shell-unstable-v1 (requests/events/enums — the B1
  verification):
  https://wayland.app/protocols/wlr-layer-shell-unstable-v1
- IPP encoding + transport (the B2f wire):
  RFC 8010 https://www.rfc-editor.org/rfc/rfc8010.txt
  RFC 8011 https://www.rfc-editor.org/rfc/rfc8011.txt
- PDF structure (the B2f writer): Adobe's published PDF 32000-1,
  https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf
  (fetched through secondary references; the 22 MB primary was
  over the fetch budget — the structure written follows the
  xref/trailer/object model every PDF tool verifies, and the
  output is machine-verified by poppler's pdfinfo in the suite)
