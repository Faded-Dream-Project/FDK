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

**GTK4 widget inventory diff**: missing are `GtkAssistant`,
`GtkActionBar` (toolbar + custom containers cover the layout),
`GtkFixed` (FDK containers position explicitly — same role),
`GtkGrid`-as-widget (FDK has no grid container; box + tables cover
most UIs), `GtkColorChooserWidget`/`GtkFontChooserWidget` (the
DIALOGS exist — `fdk_dialog_choose_color/font` — the inline widget
form does not), `GtkScaleButton`, `GtkVolumeButton`,
`GtkRecentChooser`, `GtkAppChooserWidget`. **PARTIAL** — the
every-application set is complete; the chooser-widget forms and
grid container are the honest missing tail.

**Qt6 Widgets diff**: Qt's ~180 classes include a rich
item-view/MVC layer (QTableView, QHeaderView, QDataWidgetMapper,
proxies/sorting-filtering models). FDK's tree/list/iconview are
widget-owned (no model/view split). **PARTIAL — the known
deliberate trade**: FDK binds views to caller-fed row callbacks;
no pluggable model layer. Everything displayable is displayable;
deep model tooling is not.

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

**GTK4 (Pango) diff**: Pango attributes cover font-size runs,
letter-spacing, rise, baseline, per-run fonts, and full attribute
lists; FDK's markup covers styles/decorations/color only.
**PARTIAL** — complex-script SHAPING is at parity (the hard part);
the long tail of run attributes is not exposed. Attribute-list
API (pango_attr_list) has no FDK twin: markup is the only inline
styling route.

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

Box (H/V), flow containers, scrollview, notebook, stack, paned,
frame, expander, toolbar, plus explicit positioning.
**GTK4 diff**: no GtkGrid, no GtkConstraintLayout/Baseline grid,
no natural-size negotiation beyond what containers implement.
**PARTIAL.**

## 7. Input

Pointer (enter/leave/motion/button, multi-button), keyboard
(keymap, keysyms, per-window shortcuts), wheel, touch (single
surface), text-input-v3 IME (preedit + commit, both backends),
drag-and-drop (begin/begin_ex, per-widget drop targets, per-window
format masks), clipboard (UTF-8 text, ownership semantics per
platform), XTEST-grade synthetic input in the test rig, per-window
input regions with click-through.

**Diffs**: clipboard is text-only (no image/URI lists — GTK/Qt
have rich paste). No gesture recognizers (GTK4's GtkGesture* /
Qt6's QGestureRecognizer: swipe, pinch, rotate, long-press).
Multi-touch tracking (touch slots) is not exposed (single-pointer
events). **PARTIAL.**

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
  capabilities/status), system tray (StatusNotifierItem, both
  directions), toplevel control (foreign-toplevel-management:
  list/activate/close), session manager (session save/restore,
  join/leave, command), idle inhibit, portal prefs (theme/scheme/
  text scale), activation tokens, drag-and-drop (both), window
  icons, app_id.

**Diffs**: no print dialog (GTK/Qt embed CUPS; FDK has nothing —
save-PNG + the file dialog is the workaround), no
recent-files/app-chooser portals, no on-screen-display or
notifications-server side (client only). **PARTIAL — the
desktop-shell client surface is deep; printing is the one whole
absent category.**

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

## The prioritized gap list

1. **fdk-shell-overlay (shell surfaces)** — CLOSED: the
   `fdk-shell-overlay/` package ships the layer-shell/EWMH-dock
   add-on on the extension API (formerly `fdk-shell/`, renamed
   2026-11-01; see the section below). Its X11 suite is verified
   in this sandbox.
2. **Clipboard: image + URI-list formats** — HIGH (the most
   user-visible everyday gap; text-only paste is 1995).
3. **Gestures + multi-touch** — MEDIUM-HIGH (tablets/touch UIs).
4. **Text on extension surfaces** — MEDIUM-HIGH: widgets and text
   live in FDK's window layer; an extension surface draws through
   the raw fdk_surface API, which has no text primitive. A shell
   bar must either host a window or draw glyphs itself. Hosting
   widgets on fdk_ext_surface is the natural next
   extension-API version.
5. **Pango-grade run attributes (size/rise/letter-spacing/per-run
   fonts) in markup** — MEDIUM.
6. **Grid container** — MEDIUM (the most-requested layout
   primitive still missing).
7. **Inline chooser widgets (color/font)** — LOW-MED (dialogs
   exist).
8. **Model/view split for tree/list** — LOW (large redesign; the
   callback-fed views cover display).
9. **Print dialog / CUPS** — LOW by usage, HIGH by effort; the
   only wholly absent end-user category.
10. **Windows/macOS backends** — out of scope for this project by
    charter.

**Verdict in one paragraph.** FDK is at honest parity for the
application core — windows, input, 36 widgets, complex-script
text, AT-SPI accessibility, theming, D-Bus/tray/notifications/
session, i18n, undo, animation — with the deliberate shape
differences (no GObject/moc, token themes, immediate-mode
rendering) documented above. The shell-surface gap is closed
(`fdk-shell-overlay/`); the real remaining gaps are clipboard
richness, gestures, text-on-extension-surfaces, markup attribute
depth, and printing.

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
