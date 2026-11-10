## [1.0.0] — 2026-11-08 — THE FIRST RELEASE
## P5(a)(b)(c)(e): the proof examples — and the walls they tore down

examples/ (built against an INSTALLED FDK, no internal include paths,
public API only; make test-examples runs (b)(c)(e) under Xvfb,
test-examples-wayland runs (a) on a layer-shell compositor):

- (a) panel-layer-shell.c — a bottom panel with an exclusive zone.
  (b) gl-canvas.c — 5 GPU frames presented. (c) custom-widget.c — a
  subclass with per-instance fields painted. (e) worker-post.c — 6/6
  worker results delivered on the UI thread.

WALLS FOUND (each a bug, each fixed):

1. The widget-subclass contract requires embedding fdk_widget first,
   but the type was opaque in the public header — the header's own
   documented example could not compile. Fixed with
   `fdk_widget_embed` (a sized opaque embed, FDK_WIDGET_EMBED_U64,
   static-asserted in the library against sizeof(struct fdk_widget));
   field access stays API-only.
2. The layer-shell role state was set but never committed: the
   compositor never sent the first configure (wlr-layer-shell hands
   out configures only after a wl_surface commit). Creation and every
   setter now commit.
3. No way to give a layer surface its client-chosen size: a
   bottom-anchored panel configured at 0x0 and FDK's configure gate
   dropped it. New public fdk_ext_layer_surface_set_size(w, h)
   (routed through the layer setter op with its own commit), and the
   configure notify gate now accepts one-sided sizes (the anchored
   dimension arrives as 0).

The debug install is sanitizer-instrumented; example runs preload the
ASan runtime (recorded in the targets).
## P1.2: explicit texture filter — fdk_texture_draw_ex

- NEW public API (fdk_surface.h): `fdk_texture_filter`
  (FDK_FILTER_NEAREST / FDK_FILTER_LINEAR) and
  `fdk_texture_draw_ex(surface, tex, dst, filter)`.
  `fdk_texture_draw` keeps its signature and is documented as the
  NEAREST default.
- Both backends implement the SAME sampler: the CPU gained a 4-tap
  edge-clamped bilinear (straight-alpha, channels interpolated
  independently, exactly like GL_LINEAR on the engine's straight-alpha
  texels); the GPU drain now sets the sampler per draw (pend slots
  never share texture objects with the glyph atlas, so the batch path
  is untouched) and texture objects are realized NEAREST — the
  engine's documented default.
- Blits: axis blits stay NEAREST (the CPU maps nearest); the
  TRANSFORMED blit books LINEAR (the CPU's transformed path is
  bilinear).
- Measured interior agreement CPU-vs-GPU: texture nearest 0.000,
  texture linear 0.000, axis blit 0.000, rotated blit 0.000 (the
  0-1 LSB contract holds at 0). The smoke now gates both filter
  regions; edge tolerances unchanged and evidence-based.
## P1 close-out: the circle half-pixel bug, the cross-section tool

- tests/crosssection.c + make test-crosssection: dumps 1px-wide
  CPU-vs-GPU coverage cross-sections (circle center-row edge, circle
  45-degree arc, rotated-blit edge) with a shift analysis in [-2,2]
  that separates a geometry offset from an AA-model difference.
- EVIDENCE: before the fix the circle's center-row boundary pixel was
  cpu=114 gpu=0 (the GPU painted it full): the CPU's fill_circle_aa
  sampled the SDF at INTEGER pixel coords while the rect/rounded AA
  paths, the ring path and GL fragments all sample at pixel CENTERS
  (x+0.5) — the CPU disk was half a pixel small. Fixed: every CPU AA
  rasterizer now samples at pixel centers. After: center row EXACT
  (114 vs 114), 45-degree arc within 1 LSB (162 vs 163), rotated-blit
  edge exact (residual 0.00 at shift 0). The remaining smoke-region
  circle residual (edge mean 25.7, max 115) is the documented
  area/ramp model difference at r=24 curvature — no offset (best
  shift 0 everywhere), tolerances stay evidence-based.
- The GPU shader was NOT shifted; the CPU was aligned to the GL grid.
## Identity fix (sandbox infra)

- scripts/setup-sandbox.sh installed the git identity as
  "Faded Dream Project" and a pre-commit hook enforcing exactly that
  name; per the owner the commit author is FemBoyGamerTechGuy
  <242450780+FemBoyGamerTechGuy@users.noreply.github.com>. The setup
  script, both git configs and the hook now agree on the correct
  identity. The five earlier session commits keep their old author
  (fixing them would need a force-push, which the rollback rules
  forbid); all commits from this session on carry the correct author.

### Added — the GPU paint engine (P1, the main goal)

- **Real GPU rendering**: FDK_RENDER_GPU now PAINTS on the GPU — no
  CPU framebuffer exists (the size-only framebuffer contract:
  `window_get_framebuffer` returns pixels==NULL in GPU mode) and
  every `fdk_surface` draw command executes as batched GPU draws.
  One GLSL program; quads CPU-expanded into one VBO; SDF rounded
  rects/circles/strokes, linear gradients, texture quads, glyph
  coverage; clip stack mirrored into per-quad clip data; flush on
  state change or 4096-quad batch fullness. Proven pixel-equal to
  the CPU rasterizer on llvmpipe (tests/test_gpu_paint.c, mean
  channel diff 0.000).
- `fdk_surface` is THE painter API in all three modes — widget paint
  code is written once (the engine hooks ride the same primitives,
  not a parallel path).
- **fdk_texture**: `fdk_texture_create/update/draw/destroy` — the
  explicit software-pixel upload path for GPU mode (RGBA8888,
  sub-region updates, identical call on the CPU path).
- GL entry points dlopen'd (libGLESv2/libGL, the lanes' posture);
  the engine owns a pwindow->surface registry so the frame pump's
  internal re-presents flush correctly; failure = the engine marks
  itself dead and present reports UNSUPPORTED (GPU mode never fakes
  a frame or downgrades silently).

### Added — the add-on surface (extension API v3 fold, pre-release)

- Render modes: `FDK_RENDER_AUTO/CPU/HYBRID/GPU` (init option >
  `FDK_RENDER` env > AUTO; per-window override; `fdk_context_render_mode`/
  `fdk_window_render_mode`; `FDK_EXT_CAP_RENDER_CPU/HYBRID/GPU`). GPU
  mode requires the presentation lane: creation fails UNSUPPORTED
  without GL, no present-time downgrade.
- Widget subclassing goes public: `fdk_widget_class.struct_size`,
  `instance_init`, `instance_finalize` (renamed from `destroy`),
  `fdk_widget_instance()` + `FDK_WIDGET_INSTANCE()`.
- First-class layer-shell: `fdk_ext_layer_surface_create` + setters,
  `FDK_EXT_CAP_LAYER_SHELL` (zwlr_layer_shell_v1 generated + bound).
- Opt-in backend headers `fdk/ext/wayland.h` and `fdk/ext/x11.h`
  (typed wl_display/registry/seats/xkb; Display*/XID + EWMH dock
  helpers) — the "workarounds banned" rule replaced by documented
  native access.
- Threading: `fdk_post_delayed`; nested/modal loops
  `fdk_ext_modal_run/quit/depth` (quit-able from any thread).
- GL access for add-ons: `FDK_EXT_CAP_GL` + the lane's native display
  via the ops; recipe on the cap (EGL/GLX on the native accessors).
- The explicitly unstable tier: `include/fdk/unstable.h`, the
  `FDK_UNSTABLE` version node in the export map, verify-exports
  split checks.

The project owner set the first release to **1.0.0**
(docs/versioning.md, 2026-11-08): the 1.0.0-rcN candidate numbering
is retired, the pre-release suffix is dropped, and the cycle labels
across the tree (code comments, docs, packaging, this changelog's
candidate entries) read 1.0.0. The decision itself carried no code
changes; the round-40 release-closing work below landed under the
same release identity before the tag.


## 1.0.0 (unreleased — the tag was withdrawn by the owner on 2026-11-09; back to development)

The packaging channel stays wired (.github/workflows/release.yml:
the bundle plus the five distro families' native packages,
deb/rpm/arch/apk/xbps, built on a v* tag), but no tag exists and
none is planned until the owner says so — the project is a
development version again.

### What you should know before running it

- **The desktop font statement is read, not chased.** FDK reads the
  desktop's font statement through the freedesktop Settings portal
  (XSETTINGS on X11 when served) at startup, and honors it for
  fresh instances, below `FDK_UI_FONT_SIZE` and FDK's own settings.
  In 1.0.0 the statement does NOT resize already-rendered text
  live — the log says "recorded; running UI keeps its startup
  size". Live resize is the first post-1.0.0 follow-up. FDK has no
  GTK linkage and reads no GTK/dconf configuration: `ldd` shows no
  gtk/gdk/gobject/pango, by the owner's locked spec.
- **Multi-monitor behavior is untested** (the verification rig has
  one output). Single-display desktops are the tested posture.
- **Known cosmetic:** at most ~1 KB of one-time D-Bus teardown
  residual, visible only in ASan debug builds; release builds do
  not ship the sanitizer.
- **The apps exit cleanly on SIGTERM/SIGINT** (logout daemons,
  `timeout`, `kill`) since round 53; a missing optional translation
  catalog logs INFO, not ERROR.

### Fixed (round 53)

- **The test apps exit cleanly on SIGTERM/SIGINT** (logout daemons,
  `timeout`, `kill`): the common boot turns the signal into the
  same main-thread teardown as a window close (was: exit 143, no
  session save). The core's handler-quit contract was already in
  place; the apps now use it.
- **No more boot ERROR for an absent translation catalog**: a
  missing optional `.fmo` is the designed fallback to source
  strings and logs INFO; a catalog that exists but is broken still
  errors.

### Changed (round 51)

- **fdk-files' tray menu is live**: the Open row names the current
  folder (refreshed through the about-to-show parity before every
  show, both protocols), and About opens a message dialog so it is
  visible even with no notification daemon on the bus.

### Added (round 50)

- **The tray click menu** — a click on the tray icon now opens the
  item's attached menu as a real dropdown: on StatusNotifier bars
  the host renders it (DBusMenu, unchanged); on classic XEmbed
  bars the toolkit pops the SAME fdk_tray_menu locally at the
  click's position (the widget layer's popup machinery, the
  adapter mirroring the model). Button 1 and button 3 both fire
  the app's on_activate (secondary flag on button 3) either way.

### Fixed (round 49)

- **The tray fallback renders icon NAMES** — round 48's fallback
  required pixmap pixels, but apps that open the item with
  `set_icon_name` only (fdk-files' shape; found by the owner's
  tint2 retest, whose log named the miss exactly) waited forever.
  The theme engine now draws the named icon at 32 px (live theme
  policy, straight alpha) and the item docks; app-set pixels still
  win and swap live. `set_icon_name` retries the fallback.

### Fixed (round 48)

- **The tray works on ANY X11 bar** — the tray item spoke only
  StatusNotifierItem, so classic bars (tint2's legacy systray,
  trayer, stalonetray, fvwm) never showed it: with no SNI watcher
  on the bus the item is invisible. Found on the owner's X11
  desktop (nm-applet docks on tint2, FDK's item did not). Fix:
  the platform's new XEmbed system-tray fallback (System Tray
  v0.2 — the protocol nm-applet falls back to on such bars): the
  item docks a real icon window into any classic systray,
  repaints on the bar's resize, unhosts on the bar's death, and
  re-docks through the MANAGER announcement of a restarted bar;
  SNI wins the moment a watcher appears. No per-bar code
  anywhere; pinned in the suite by a real in-test System Tray
  v0.2 manager (dock, wire shape, restart re-dock).

### Fixed (round 46)

- **`fdk_scale_button`: the popup never mapped** — the open path
  packed the slider and value label but never called
  fdk_window_show (the menu machinery's third step). Invisible on
  every real desktop; the SendEvent-based GUI tests could not see
  the difference. Found by the owner's field report.
- **Placement** follows the menu-anchor rule (below the button,
  centered; a root-level popup hangs below its owner like any
  menu) — the owner-bounds clamp that slammed a 176px popup onto
  the title band of a 190px window is gone.
- The GUI suite now drives the popup with REAL server-routed input
  (an XTest rig: warp, click, motion steps, press-drag-release) —
  the grab-routed path the SendEvent clicks bypassed.
- **`scripts/xtest_drag.c`**: the standalone field rig (real-input
  driver + build line in the header).

### Added (round 45)

- **`fdk_scale_button`** (fdk_widgets.h, 6 public functions): the
  slider-in-a-popup control — the GtkScaleButton/GtkVolumeButton
  row, closed. The face is a value meter; a press pops a window
  below (above when out of room) carrying a vertical slider and a
  live value label; drags fire on_value_changed live (the value
  rides the call). Dismissal: Escape, a second press, or a
  press-away routed to the popup by the grab. The family contracts
  (silent setter, range rules, step, detached no-op, popup
  abandonment) are the toggle's/combo's, verbatim.
- **`examples/21_scale_button.c`**: the demo — a volume-posture
  scale button with a live readout on the shared example helper.
- **PARITY**: the ScaleButton/VolumeButton row reclassified parity.
  The honest tail: Video/MediaControls and the policy rows.
  1,046 exported symbols.

### Added (round 44)

- **`fdk_font_button`** (fdk_dialog.h, 8 public functions): the
  font chooser dialog's button host — the color swatch button's
  font twin. The label reads "Family [Style] Size"; pressing it
  opens the font chooser dialog seeded with the current
  description; ACCEPTED settles the pick and fires on_font_set;
  CANCELLED changes nothing. The dialog's size rule (0 = 12, clamp
  6..96), the silent programmatic setter, the detached-tree no-op,
  and the BUTTON-with-value a11y contract are the color button's,
  verbatim. 1,040 exported symbols.
- **PARITY**: the GtkSwitch row reclassified parity (the toggle's
  track+knob paint IS the switch visual); the font-button row
  closed. The honest tail: ScaleButton/VolumeButton,
  Video/MediaControls, and the policy rows.

### Changed (the decoupling round)

FDK stands alone: the shell add-on (its own repository) is no
longer mentioned, referenced, or tested against anywhere in this
tree — the dependency arrow points one way (add-on -> FDK), and
FDK's docs, build, CI, and history now read as if the add-on never
lived here. Comment- and doc-only; zero behavior change.

- The bar-test rig left: `test-apps/fdk-addon/` (the HUD with its
  own Wayland shell role) and `test-apps/fdk-addon-host/` (its
  dlopen host) are deleted with their `addon` target, the
  smoke-matrix entry, and the labwc-smoke legs; the Wayland
  protocol replay rig under scripts/ is deleted too. The labwc
  smoke runs the eight application legs.
- The tree-wide sweep (52 files): CHANGELOG, PROGRESS, ROADMAP
  (the add-on's own Part A and register rows left — the register
  is 45 items now, the one core-Wayland A-item re-filed as B-20;
  the correction recorded in place), README, EXTENDING.md (the
  reference consumer is `examples/19_extension_surfaces.c` now),
  TESTING-ON-DESKTOP.md (the walkthrough's add-on-subject sections
  left; the FDK-core sections renumbered 1-19), PARITY,
  THIRD-PARTY, DECISIONS, abi-policy, dbus, versioning,
  platform-input, dependencies, the packaging scripts, the CI
  workflow, the Makefile comment block, and the code comments —
  the technical lessons stay, the name goes.
- The fdk-files tray-bus rig runs against ANY StatusNotifier
  watcher / notification daemon (TRAYHOST_BIN / NOTIFYD_BIN, or
  whatever the running desktop provides; legs without a
  counterpart skip honestly) instead of the add-on's binaries.
- Release-gate G2 is generic now (any open bug in PROGRESS's
  current-state block fails it); the parked HUD open-bug record
  left with its subject.
- Repo hygiene in the same pass: a tracked 7.9 MB ELF core dump
  (`test-apps/core`) deleted, and the core-dump ignore rule
  widened so a stray core can never be committed again.

### Added (round 40)

- **Inline chooser widgets** — the PARITY honest tail closed:
  `fdk_color_chooser_widget` (the dialog's per-pixel HSV wheel, the
  current-color swatch, and the #rrggbb entry as an embeddable
  widget — the color is live, no OK/Cancel) and
  `fdk_font_chooser_widget` (the scrolling family/style list, the
  size spinner, and the live preview as an embeddable widget). Both
  share the dialogs' engines by construction (one wheel rasterizer,
  one hex parser, one enumeration path, one preview reload — the
  dialog and the widget are two hosts of one `fdk_cc_core` /
  `fdk_fc_core`); user gestures fire the new change callbacks,
  programmatic setters stay silent (the color button's rule). 10
  new public functions in fdk_dialog.h (1010 → 1032 exports with
  the extension entry points below). Headless tests: create
  ladders, the silent-setter contract, wheel+swatch pixel proofs,
  ring/triangle picks, the unknown-family fallback, the honest
  empty enumeration, destroy-from-callback survival
  (tests/test_choosers.c).
- **Widget hosting on extension surfaces (extension API v3)** —
  `FDK_EXT_API_VERSION` 2 → 3 and
  `fdk_ext_surface_host_widgets()`: the whole widget catalog (text
  included) on any extension surface, through the window layer's
  own root machinery — a window-owned root, configure-driven
  re-layout, standard input dispatch, and the frame-paced
  auto-paint tail the dialogs run on. The add-on's event callback
  keeps the standard `fdk_window_get_root` rule (window-level
  events always; widget-consumed input does not). The X11
  integration test pins the root's idempotence, server-side pixel
  proofs, a real click activating a button, a real XResizeWindow
  re-arranging the tree, and teardown. Fixed alongside it: a
  destroy-less kind (ops.destroy == NULL, documented optional)
  crashed the teardown — the NULL guard the header always promised
  is now in the destroy path.


### Fixed (round 22)
- **Damage-rect overlap merging** (the owner's present-trace report:
  3.29M damaged pixels on a 2.07M-pixel window — 59% double-counted).
  `fdk__surface_damage_add` now merges an incoming rect into any
  existing list entry it overlaps or touches (union in place, plus a
  compaction pass for neighbors the widened rect swallows) instead
  of appending it. Two real costs died: overlapping rects shipped
  the same pixels twice in separate XPutImage sub-puts, and the
  inflated area tripped the X11 present's >=75% whole-image
  coarsening early. Regression: `test_damage_overlap_merges`
  (identical/overlapping/adjacent/contained rect families). Verified
  under load — the round-14 stress rig's scroll/hover/sbdrag storms:
  0 over-area presents (was the signature), median 1 damage rect per
  present, hover 49 fps @ 11% damage, sbdrag 43 fps @ 5.5%, scroll
  full-height strips unchanged; noisewatch 4/4 gates; core 772,
  X11 229, 4-app smoke clean.

# Changelog

FDK's public version moves only by a recorded project-owner decision
(docs/versioning.md — the rule, the enforcement, the full history
including the 2026-10-11 retraction of a premature 1.0.0). The
project is **in development — no stable release yet**: the entries
below are the development tree's own change record (version-marked
where the tree carried a numbered version), not a catalogue of
published releases; the engineering narrative lives in
ROADMAP.md and PROGRESS.md.

The format follows Keep-a-Changelog conventions adapted to this
project: entries are chronological, each carries the public
API surface delta explicitly (FDK's ABI promise — docs/abi-policy.md —
makes that the contract that matters), and version-marked entries
note their development-candidate status.

## [Unreleased-candidate features] — 2026-11-05 (round 28: the disappearing editor — prompt-dialog teardown damages the parent)

### Fixed — after a prompt dialog closes, the parent's covered pixels never repaint

**The report (the disappearing editor):** after
`fdk_dialog_show_prompt` closes, the parent window's editor area
never repaints — the region where the dialog sat stays a dead band
while the text underneath stays alive. The save picker was
unaffected. No public API change (internal to `src/widget/dialog.c`).

- **Root cause**: a modal dialog is a normal toplevel window, NOT
  a popup — so it gets none of the anchored parent damage the
  popup teardown delivers on destroy. Whether the parent ever
  hears about its uncovered pixels is then the window manager's
  choice: a reparenting WM's unmap path can simply not send the
  parent an Expose for the region, and nothing else flags it.
  Under bare Xvfb (no WM) the server's own unmap Expose happens
  to arrive — which is why every test-green run hid the bug while
  real desktops showed it.
- **The fix** (`src/widget/dialog.c`): dialogs store
  `options->parent`; `dialog_damage_parent()` invalidates the
  parent's whole widget tree when the dialog goes away, on BOTH
  paths — `dialog_respond` (answered close, damage at the top,
  before the application callbacks) and `dialog_destroyed`
  (unanswered early destroy). Whole-tree because Wayland owns
  dialog placement (rect math is not portable); one full repaint
  is correct everywhere and cheap next to the dialog that closed.
- **Lifetime discipline**: `parent` is borrowed with no outlive
  contract and the response callback may destroy it — so the
  stored pointer is never dereferenced. The helper compares its
  value against the context's registered live-window list and
  damages the match only; a destroyed parent matches nothing.
- **Regression fence** (X11 suite):
  `test_dialog_close_damages_parent` — prompt close leaves the
  parent's tree damaged (the contract any has_damage-gated paint
  loop relies on, with or without a helpful Expose), the next
  paint consumes it, and the parentless form stays legal.

## [Unreleased-candidate features] — 2026-11-05 (round 20: the disappearing folders — root cause found, proven, fixed)

### Fixed — fdk-files wiped its own listing on every watch-fired reload

- **The report.** Folders disappear when the owner covers the
  window or switches to a terminal — with or without perf record
  running, on X11 AND Wayland. Clicking a bookmark (Home) brings
  the content back. The owner's PRESENT-TRACE showed the round-19
  expose hardening working (full presents on uncover) yet the
  content still vanished — and a growing per-row-removal damage
  sequence at the trace's end: the MODEL was emptying, not the
  paint.
- **The root cause (proven twice).** `pane_reload()` and
  `pane_new_tab()` both call `pane_navigate(p, t->dir)` — and
  pane_navigate did `snprintf(t->dir, sizeof t->dir, "%.400s",
  abs_path)` where abs_path IS t->dir: overlapping snprintf is
  undefined behavior (C11 1.0.0.0) and this glibc EMPTIES the
  string (scripts/overlap_repro.c isolates it in 8 lines). So every
  watch-fired reload navigated to `""` → `opendir("")` fails →
  fmodel_load's failure path **cleared the whole store** (also
  unbatched — the per-row removal storm the owner's trace shows).
  The inotify watch fires on ANY write in the viewed folder —
  session noise in $HOME (.Xauthority and friends) is constant
  during session switches, hence "more so when I switched to X11";
  perf record was pure correlation (the terminal switch).
- **The fix, three independent layers.**
  (1) pane_navigate snapshots abs_path into a local before writing
  t->dir (kills the UB for every aliasing caller) and refuses
  empty/NULL paths outright.
  (2) fs_list_dir now distinguishes `unreadable` (opendir failed)
  from genuinely empty; fmodel_load KEEPS the current rows on a
  read failure and reports it in the status bar instead of wiping
  the view.
  (3) The legitimate empty-directory clear is batched (one CLEARED
  replay, not a per-row removal storm).
- **The regression rig (test-apps/run_noisewatch.sh + scripts/
  xfocus.c).** A background writer hammers a dotfile in the watched
  directory every 150 ms (real session-noise shape) while the rig
  drives focus loss, a REAL 10 s occlusion, and idle sits. Four
  decisive gates: no model clear after baseline, the bound row
  count never zero, pixel content identical across every stage,
  and the watch actually exercised (>= 20 fires). Plus permanent
  DRIG-gated telemetry (DRIG-WATCH / DRIG-ROWS / DRIG-CLEAR /
  DRIG-UNREADABLE) for future field reports.
- **Verification**: noisewatch 4/4 gates green under 229 watch
  fires; covercheck 4/4 shapes (round 19's rig, no regression);
  core 763; X11 229. The broken build fails the rig at the first
  watch fire (DRIG-CLEAR prev_rows=2, DRIG-ROWS=0 forever).

## [Unreleased-candidate features] — 2026-11-05 (round 19: the disappearing-folder report)

### Fixed — expose now repaints the SURFACE, not just the tree

- **The report.** The owner's release-build profile session
  (`sudo perf record -g -p $(pidof fdk-files) -- sleep 10`) leaves
  the file list BLANK after the run: covering the window for the
  recording (~10 s) and uncovering it lost the folder content until
  the next input forced a repaint. The profile numbers themselves
  are healthy (fill_rect_translucent 52% -> 10.3% after round 18;
  the ~20% of libnvidia-eglcore is the GPU present lane doing its
  job).
- **The audit.** Not reproducible in the sandbox: a REAL-occluder
  rig (override-redirect window stacked above the app — the
  server-discards-pixels path, distinct from round 16's unmap test)
  passes on the CPU lane, the forced GL lane, the bare server, AND
  under a real reparenting WM (scripts/miniwm.c, new — the smallest
  honest WM: substructure redirect, frame windows, synthetic
  ConfigureNotify). The residue the sandbox cannot see: the NVIDIA
  driver and a full EWMH WM.
- **The hardening (window.c).** The expose handler invalidated the
  widget TREE but never touched the window SURFACE's damage
  bookkeeping — a re-present that shipped only new widget damage
  left every untouched pixel relying on server-side pixels the
  server may have discarded. Every expose now marks the window
  surface full-damage, so the very next present re-sends the whole
  frame regardless of how little the tree thinks changed — and
  regardless of whether there IS a tree (apps that draw the surface
  directly ride the same guarantee). Exactness-preserving on both
  backends (X11's GL staging buffer and SHM back slot both hold the
  last-presented frame; Wayland pre-fills fresh buffers with the
  visible frame). The rig's PRESENT-TRACE confirms the shape: the
  post-uncover present is now `full`.
- **The regression rig (test-apps/run_covercheck.sh + scripts/
  occlude.c + scripts/miniwm.c).** Baseline row count, a REAL 10 s
  occlusion, gates for (a) content rows restored, (b) a present
  actually happening with zero input, (c) pixel identity against a
  forced full repaint, and (d) post-incident navigation sanity.
  Four shapes: GL/CPU lane x WM/bare server (FDK_NOGL / FDK_NOWM).
- **Verification**: core 763, X11 229, covercheck 4/4 shapes green
  (the core-suite `test_cli_tools` abort on a clean checkout was a
  build-order artifact — `make tools` before `make test`; the
  Makefile's test target does not depend on tools and never has).

## [Unreleased-candidate features] — 2026-11-05 (round 18: the release-lane round)

### Performance — the XRGB table pass (round 17's follow-through)

- **fill_rect_translucent: per-fill tables + uniform runs.** The
  owner's release-build profile (round 17 in place, ASan gone)
  showed the fixed cost correctly concentrated: 52.4% in
  fill_rect_translucent itself — the blend work, no longer the
  per-pixel overhead. Window surfaces are XRGB8888 on every
  backend, and for XRGB the blended byte depends on the
  destination only through each channel's own 0..255 value — so
  the fill now memoizes the exact float expressions into three
  256-entry tables ONCE PER FILL (bit-exact by construction: the
  same ops, same order, same rounding, evaluated 256 times
  instead of per pixel), and detects runs of equal destination
  words, storing one constant output word across the run. Measured
  on the micro-benchmark: 22x on uniform backgrounds (the row
  highlight over the plain list background), 12x on the anti-run
  worst case (every pixel differing); exactness sweep still 0
  pixels differ on both formats. The ARGB path (offscreen icon
  surfaces) keeps the round-17 hoisted form — its output depends
  on the destination alpha, which has no per-channel table.
- **The owner's build friction, fixed.** Round 17's profiling
  instructions referenced a test-apps release target that never
  existed (the apps Makefile built only the ASan debug set; the
  owner's `make release` inside test-apps failed with "no rule",
  and the run landed on a release LIB linked against a DEBUG
  app). test-apps now has a real `release` target (-O2, NDEBUG,
  no sanitizers, into build-rel/) and the root `make release`
  builds the apps too, so `make release` alone produces the
  profiling binaries. Verified: zero ASan symbols in the output.
- **-Wformat-truncation hygiene (the owner's gcc is newer than
  the sandbox's).** discover.c's three snprintf path builders
  (the stem pass, the two directory listings) now check the
  return value — a name that cannot fit the buffer is skipped,
  not truncated; fdk-edit's tab label assembles through ta_copy
  (the pattern ta_buf.h prescribes for path-derived strings)
  instead of an unbounded snprintf into a 160-byte stack buffer.
  All compile clean at -O2 -Werror.
- **Repo hygiene:** the 6MB `test-apps/core` core-dump that was
  committed by mistake (it is even in .gitignore) is untracked.
- **Verification**: core 763, X11 229, the scroll visual rig 4/4
  byte-identical with zero warnings, and the release lane builds
  end-to-end with zero warnings under the project's -Werror set.

## [Unreleased-candidate features] — 2026-11-04 (round 17: the drag-highlight profile)

### Performance — the 53% blend_pixel

- **The translucent-fill fast path (surface.c).** The owner's
  drag-select profile: 53.7% of all cycles inside blend_pixel, 45 of
  those points arriving via fdk_surface_fill_rect under row_paint —
  every selected (alpha 0.38) and hovered (alpha 0.35) row background
  blended through the per-pixel float helper: a clip test, a format
  branch, an alpha clamp, channel unpacking of the CONSTANT source,
  and two constant weight products, per pixel. fill_rect and fill
  now run a hoisted loop (fill_rect_translucent): everything
  per-pixel-invariant is computed once per fill, the body is a
  4-wide-unrolled blend, and the result is BYTE-IDENTICAL to
  blend_pixel (verified by an exhaustive 256-value destination
  sweep on both surface formats — 0 pixels differ; the pixel-
  identity gates and the scroll rigs hold). Measured 1.17-1.22x on
  the fill micro-benchmark (-O2, scalar codegen — the ARGB divide
  chain blocks auto-vectorization; an SSE2/LUT pass is the recorded
  follow-up). blend_pixel itself is unchanged for the shape/AA
  callers.
- **The rubber-band want-test is incremental (list.c).** Every
  pointer motion ran list_apply_band's membership loop over ALL
  rows — O(count) per motion pixel (a 1000 Hz pointer against a
  100k-row list is 100M iterations/s of pure testing). The apply
  now visits only the union of the previous and current band row
  windows (rows that keep their state cannot flip); a PRESS seeds
  the full window — a new gesture re-evaluates every row (the
  outside-band rows must clear unless the ctrl-base holds them;
  the row-press band regression test caught exactly this before
  it shipped).
- **Verification**: core 763, X11 229 — all green; the scroll
  visual rig passes all four storms byte-identical (down, up,
  jitter, unmap/remap sit) with zero app warnings.
- **Known issues (pre-existing, reproduced on the clean round-16
  tree — NOT this round's regressions)**: the release config
  (NDEBUG) segfaults in test_attr; the headless-sway Wayland suite
  trips a button_hits assertion (test_wayland_integration.c:2043)
  under this container's sway. Both recorded for a dedicated
  config-hygiene round.

## [Unreleased-candidate features] — 2026-11-04 (on the rc2 line, later the same day)

### Stability + performance — owner round 16: the real-hardware profile, the aggressive-scroll residue

- **The aggressive-scroll stale strips (scroll.c).** The owner's
  perf-record session also reported the visual bugs: text
  overlapping when scrolling up, the list blanking after hard
  scrolling. Root cause (found with an in-suite deterministic
  storm — two wheel notches per pump, striped content, XGetImage
  readback vs forced full repaint): the strip path's damage REPLACES
  the root damage box, and in the aggressive regime (a 30 Hz wheel
  against a ~24 Hz paint cadence) two failure shapes appeared.
  (a) A pending damage box from an earlier tick of the same frame
  goes stale when the next blit moves its pixels: the cover is now
  the union of the box where it was, the box where the blit took it
  (translated by the delta, CLIPPED to the blit footprint — an
  unclipped translate compounds its own offset every tick; the
  storm trace showed the box descending to y = -1800). (b) A later
  tick's smaller strip could DROP a still-unpainted region set by
  an earlier tick: the live root box is now re-read and unioned at
  strip time, not only at layout entry. Regression test:
  `test_scroll_storm_identity_gui` (x11 suite) — 20 rounds of two
  notches per pump, pixel-identity demanded against a forced full
  repaint.
- **The HiDPI intermediate "clear" that cleared nothing
  (surface.c + window.c).** At scale > 1 the tree paints into an
  ARGB intermediate that is "cleared" each frame with a 0-alpha
  fill — but source-over with a == 0 is a mathematical NO-OP on
  ARGB (out = dst): the old frame's pixels survived, and every
  stale glyph bled through wherever this frame drew
  semi-transparently. `fdk_surface_fill` now memsets a 0-alpha fill
  on ARGB surfaces (an exact clear), which also deletes the
  per-pixel float blend the owner's profile showed at 23%
  (`blend_pixel`) for that fill.
- **Verification**: core 763, X11 229 (+1), Wayland 65 — all green;
  the Xvfb visual rig (scripts/scrollcheck_r16.sh) passes the
  30 Hz down-storm and the unmap/remap sit pixel-identical.
- **Known residue (honest)**: a ~266 px band can still differ after
  a sustained UP-storm when the horizontal bar's visibility flips
  mid-flight (the viewport resizes between ticks; the replace-shaped
  damage drops the below-viewport stale region). The deterministic
  fixed-geometry storm is green; the flipping-geometry shape is the
  next round's first item, along with the two perf items below.
- **NOT DONE (carried)**: the 4K/scale > 1 physical-space strip
  blit (the strip path still declines at scale > 1 — the owner's
  profile showed whole-viewport fills dominating: 39.6% fill_rect +
  22.9% blend_pixel self time, partly the ASan build inflating it);
  the blend_pixel batch/hoist for translucent fills.

## [Unreleased-candidate features] — 2026-11-04 (on the rc2 line)

### Stability — owner round 15: "deleting a file leaves fdk-files broken" + the scroll notch feel

- **The delete-stability fix (list.c).** `fdk_list_remove` clamped
  the shift-click `anchor` after removing a row but left the OTHER
  row-indexed fields past the end: `last_click_row` (the
  double-click/triple-click bookkeeping) and `key_cursor` (the
  keyboard's moving end). After deleting a row at or above the
  cursor, Enter-activate silently died (the guarded read treated
  the stale index as "no cursor") and the next click's
  double-click compare could never match — the
  delete-a-file-then-the-list-feels-dead report. Both fields are
  now clamped exactly like the anchor (and `have_last_click` clears
  on an emptied list). Regression test: `tests/test_list_delete.c`
  (drives the cursor to row 5, removes every row above index 2,
  then asserts Enter-activate and arrow keys still work — fails on
  the pre-fix engine at the Enter leg).
- **The delete reproduction rig (test-apps/fdk-files/delete_rig.c +
  run_deleterig.sh).** Six XTEST-driven shapes against the real app
  under ASan+UBSan: Delete key, Shift+Delete with the modal confirm
  answered, multi-select, last row, scrolled offset, and a
  directory delete riding the async ops engine (ops_start_delete +
  delete_engine_done + pane_reload). All six green — no crash, no
  hang, no sanitizer hit, the app stays interactive after each
  delete. The rig taught the app four new telemetry lines:
  `PHASE: nav <dir>` (verified navigation, FDK_FILES_DRIG-gated),
  `DRIG-GEO path` (the location entry's bounds) and `rowh=` on the
  list GEO lines (the real row pitch — the rig's first draft
  assumed 25 px and clicked the wrong row).
- **The scroll notch (scroll.c).** The wheel step was a fixed
  48 px — glacial in a 1000 px list, violent in a 150 px sidebar.
  The notch is now viewport-proportional (the axis' own viewport
  divided by 4, clamped to 40..160 px — GTK3's rule), computed
  per-axis from the live extents; fractional deltas
  (high-resolution wheels, touchpads) multiply the step directly,
  so sub-unit ticks move a fraction of it — smooth, no row
  snapping. The ease-out flight and the gesture-accumulation rule
  (a notch while airborne adds to the pending target) are
  unchanged; test_scroll.c and the x11 suite's animation-gui legs
  pin the new arithmetic (a 150 px viewport floors at 40; the x11
  leg's 180 px bounds minus the classic bars' 12 px reservation =
  168 px viewport, 168/4 = 42).
- **Sandbox recovery (scripts/setup-sandbox.sh).** The fourth
  recycle exposed the script's own cwd bug: the `ROOT_DIR` re-pin
  resolved RELATIVE to the directory step 1 had cd'd into, so steps
  2-4 ran from `/tmp/x11dev-debs` and the Wayland toolchain never
  installed. The root is now pinned ABSOLUTELY at script start,
  before any cd.

## [Unreleased-candidate features] — 2026-11-03 (on the rc2 line)

### Performance — owner round 14: the scroll repaint (whole-viewport damage per offset change) + the hover repaint gap

- **The scroll strip path.** Every scroll offset change — wheel
  flights, thumb drags, trough paging, programmatic scrolls —
  invalidated the whole scrollview and repainted the entire
  viewport, even though a pure offset change leaves almost all
  rows already painted, just shifted. `scroll_apply_delta_damage`
  (scroll.c) now blits the preserved region inside the window
  surface (`fdk__surface_self_blit` — a new render-layer
  primitive: overlap-safe same-surface region copy, memmove
  row-walk with the direction picked by shift sign) and sets
  EXACT strip damage: the tree damage becomes the exposed strips
  only (unioned with anything pending at layout entry and the
  visible bars' thumb-precise old∪new rects), so the repaint
  walk renders a few dozen rows' worth of pixels instead of the
  viewport. The eased wheel flight is now a self-damaging
  animation (`fdk__animation_set_self_damage`) — the pump skips
  its blanket post-tick invalidate for it; declined paths (HiDPI,
  detached trees, blit failure, full-page jumps) fall back to the
  whole-widget invalidate the code always had.
- **The HiDPI gate.** At paint scale > 1 the tree paints into a
  cleared logical intermediate (the HiDPI compositing path), so
  the window surface's "preserved pixels" are not the tree's
  paint target — a blit there preserves nothing.
  `fdk__window_tree_paint_scale` (the one-way-opaque owner
  back-edge) gates the strip path off there: full-viewport damage
  stays (correct, merely slower); a physical-space blit under
  scaling is the recorded follow-up.
- **The hover repaint gap (an engine bug the new rig found).**
  `set_hovered` flipped `FDK_WF_HOVERED` and delivered ENTER/
  LEAVE — but nothing guaranteed an invalidation, and the list
  rows' class handles neither event while `row_paint` renders
  hover fills from the flag. A pointer sweep across rows left
  stale highlights wherever nothing else happened to invalidate
  (the hover storm measured it starkly: 1 kHz zigzag, ZERO
  presents). The transition now damages both sides at the single
  choke point every hover change funnels through.
- **Measured (release build, 20k-row list, the real-rate stress
  rig):** wheel 20/s: CPU 3.0% → 1.8% of one core; wheel 250/s
  (the freespin extreme): 15.7% → 8.8%; median damaged-area
  ratio 1.000 → 0.63-0.66; full-window presents 254/254 →
  0/241; scrollbar thumb drags: median ratio 0.141; hover at
  1 kHz: 43 presents/s at ratio 0.311 where it was zero. 4K
  honestly degrades to full damage (accumulated strips exceed
  the 75% present coarsening at that scale) — correct, and the
  recorded follow-up is the physical-space blit.
- **PRESENT-TRACE** (`FDK_PRESENT_TRACE=1`): one line per real
  present — monotonic ms, full|rects, damaged area, window area,
  rect count — the instrument the rig's damage-ratio medians are
  computed from. `FDK_SCROLL_NOBLIT=1` is the A/B kill-switch
  the measurements were made against (the pre-round-14 damage
  behavior, on demand).
- **The rigs** (in-tree, recycle-proof): `stress_rig.c` +
  `run_stressrig.sh` (real-rate input: wheel at RIG_RATE_HZ,
  1 kHz hover zigzag, scrollbar drags; /proc CPU sampling and a
  three-consecutive-quiet-poll settle gate) and
  `run_blitcheck.sh` + `scripts/xsnap.c` + `scripts/xexpose.c`
  (the pixel-identity gate: a blit-scrolled frame vs a forced
  full repaint must be byte-identical; it caught two real
  strip-path bugs this round).
- **API surface: unchanged** (all new seams are internal:
  fdk__surface_self_blit, fdk__widget_set_damage/_pending_damage,
  fdk__window_tree_paint_scale/_tree_surface,
  fdk__animation_set_self_damage).

### Performance — owner round 13: the core text path audit (shaping re-run per paint, the glyph-table linear scan)

- **The audit.** The owner's report: "the filemanager still feels
  slow — something is wrong in the core of FDK about performance;
  look through the code really carefully and find anywhere the
  performance can be increased." A full pass over the core render
  and text subsystems (src/text/*, src/render/*, src/widget paint
  paths, the window paint walk) found the architecture sound —
  the paint walk is damage-rect-culled with a clip stack, blend
  loops are tight integer source-over, hb_font objects are cached
  per font — but TWO core hotspots, both in the text path, plus
  one app-layer finding recorded for the next fdk-files round:

  | subsystem | finding | verdict |
  |---|---|---|
  | paint walk (widget.c paint_rec) | damage-rect + bounds culling | sound |
  | blend_mask / blend_rgba (surface.c) | integer loops, transparent-pixel skip | sound |
  | hb_font (shaping.c sh_hb_font_for) | created once per font, cached | sound |
  | glyph raster cache (text.c) | lookup = LINEAR SCAN over up to 2048 entries, run once per glyph PER PLACEMENT (every frame) | **fixed (hash index)** |
  | shaping itself (text.c measure/draw/spans) | FriBidi + HarfBuzz + 3 allocs re-run PER CALL PER FRAME; the result was freed and re-derived every time | **fixed (shaped-string cache)** |
  | scroll/entry caret paths (entry.c) | same re-shape per paint | **fixed (same cache)** |
  | fdk-files fs_stat_entry (app layer) | stat() per file ON THE UI THREAD — invisible on tmpfs, the real-disk "feels slow" | recorded for the fdk-files round (worker pool exists: fdk_thread.h) |

- **Fix A — the shaped-string cache (`fdk__shape_range_cached`).**
  A shaped result is a PURE function of (font, style, bytes): the
  subpixel phase is derived from a pen accumulated from zero
  INSIDE the string, and every consumer offsets by whole pixels —
  so identical calls can share one result. measure/draw/spans
  (text.c) and the Entry caret/geometry walks now borrow from an
  LRU cache (256 entries, <=1KB strings, keyed font+offset+count+
  bytes). Borrow contract documented in text_internal.h: no free,
  no holding across a differently-shaped call or a glyph
  eviction/style flush (both wipe the cache — a shaped result
  borrows raster pointers from the glyph tables). Styled runs
  (span/attr) bypass: no cheap identity, callers keep the owned
  contract.
- **The churn lesson (measured, then fixed).** The naive cache
  made navigation WORSE (release nav rig 500 files: 14.9% of a
  core baseline -> 19.4%): every directory load presents N
  brand-new strings, and first-sighting insertion filled the
  table with entries evicted before reuse — insert + copy +
  evict-scan per row for nothing. Admission now requires a SECOND
  sighting within a generation window (a 512-slot fingerprint
  remember-set): first miss parks in a single owned slot; a
  re-request admits it. Steady state (row measured then painted
  every frame) always admits; bulk loads never churn. Nav: 19.4%
  -> 13.6% (better than the 14.9% baseline — the second shape of
  each row's measure+draw pair now hits).
- **Oversized strings** (>1KB, e.g. the stress suite's 1MB line)
  ride a separate single big-slot: owned, freed on replace and by
  every invalidation, never admitted to the table. The empty
  string flows through the cache paths too — shape_range returns
  a real allocated result for it, and the borrow contract must
  own that (the first draft leaked it through a bypass; LSan
  caught it, the ownership rules are now explicit in the code).
- **Fix B — the glyph-cache hash index.** glyph_slot's lookup was
  a linear scan over up to 2048 entries, run once per glyph
  placement — every shaped walk, every frame; the header itself
  said "promoted to a hash when profiling says so". Open
  addressing over the key (4096 buckets, slot indices), backward-
  shift deletion on eviction, insert-before-rasterize with the key
  committed first (a fallback font's recursive glyph_slot call
  must find the table consistent). The table stays the owner; LRU
  eviction unchanged.
- **Invalidation discipline.** Glyph eviction, style flush, and
  font destroy all wipe the shaped cache (the raster pointers a
  shaped result borrows are invalidated by those events). The
  last-widget-root teardown drains it beside the animation engine
  (the same shutdown rule: no widget, no text, nothing left to
  shape).
- **Measurements** (the input rig — new this round: wheel-storm
  and hover-sweep CPU under Xvfb, plus the round-12 nav rig; % of
  one core, release build):

  | rig | baseline | naive cache | final (A+B) |
  |---|---|---|---|
  | hover sweep, 1000-file dir | 1.3% | 0.5% | **0.7%** |
  | wheel scroll, 1000-file dir | 6.2% | 5.3% | **5.5%** |
  | navigation storm, 500 files | 14.9% | 19.4% (regression) | **13.7%** |

  ASan build: scroll 14.2% -> 10.5%; hover and nav flat (the
  per-access instrumentation dominates). Everything settles to
  0.0 CPU after the storm stops (no leaked timers/work).
- **Test-layer note.** Two test_text assertions ("a second draw
  must increase glyph-cache hits") encoded the OLD layering; with
  the shaped cache in front, a second draw returns pre-resolved
  placements without touching the glyph table. The invariants
  that still hold and are asserted: no new rasterizations
  (misses flat) and pixel-identical output. test_text drains the
  shaped cache at exit (its fonts intentionally live to process
  exit; the defensive-drain precedent is test_choosers).
- **New rig:** test-apps/fdk-files/input_rig.c + run_inputrig.sh
  (wheel-storm / hover-sweep CPU sampling from /proc, both
  builds, RIG_FILES/RIG_SECONDS/RIG_MODE knobs).


### Fixed — owner round 12: the navigation CPU peg (the model-binding O(N²)), a round-11 double-click regression, and the fixed_w warning

- **fdk-files: one navigation into an N-entry directory cost
  N full view rebuilds — O(N²) with per-row harfbuzz shaping.**
  The owner's report: moving through folders fast pegs ONE core
  at 100% (verified in btop). The nav rig reproduced it: a
  6-loop storm into a 500-file dir = 96.4% of one core; the cost
  was flat from 50 to 1000 files because the dominant term was
  the binding layer, not the directory walk. gdb stacks caught it
  live: fmodel_load appends N rows + sets 4 cells each with the
  old view still bound, and EVERY mutation fired through the
  pane's proxy chain into binding_changed → rebuild_list — a full
  clear + N appends + one relayout (re-shaping every row text)
  PER MUTATION. TWO engine fixes:
  - **`fdk_model_begin_batch` / `fdk_model_end_batch` (new public
    API)**: change fires are HELD between the calls; end_batch
    releases exactly ONE FDK_MODEL_CLEARED through the normal fire
    path, so a bound view performs ONE replay. fmodel_load wraps
    its whole rebuild in one batch. Nestable (depth-counted),
    quiet on unbalanced end, no-op with nothing held, and a proxy
    FACE passed to it batches the proxy's child store.
  - **binding_changed went incremental**: ROW_INSERTED at the
    mirror's tail appends the one row (the fire runs from INSIDE
    row_append, before the app sets cells — the append is empty,
    the following ROW_CHANGED fills it in place);
    ROW_REMOVED removes the one row and shifts the id array;
    MOVED/CLEARED keep the full-rebuild fallback.
  Measured, release build (what ships): 500-file directory, 6
  rapid navigations = 96% → **14.9%** of one core; 1000-file =
  ~30%, linear in entries; idle after navigation = 1 tick in 2s
  (poll-asleep — the "forever" burn is gone entirely).
- **List: the first-ever double-click on a row never activated**
  (a round-11 regression the nav rig exposed immediately: its
  storm "into the folder" legs silently did nothing). The
  pending_band press path (an unselected row's press held as a
  potential rubber band) ran its click logic at the RELEASE but
  never armed the double-click window, so the second press of a
  double-click read as a fresh first click — opening a folder
  needed a THIRD press. The release now arms last_click_row/
  last_click_ms exactly as the direct path does.
- **List: one text-measure pass per relayout** (was two).
  list_relayout measures every row for the content width, then
  list_columns_layout re-measured every row again for the name
  column's natural width — two harfbuzz/fribidi passes per row
  per rebuild. The relayout now hands its measured width to the
  columns layout (a `name_nat_hint` parameter; -1 = measure in
  place for every other caller).
- **fdk-files: the post-bind decoration pass is batched** (3
  set_cell + icon per row used to relayout the whole list per
  row, per rebuild — the second O(N²) of the storm).
- **The `-Wunused-but-set-variable` warning the owner reported**
  (list.c:358, `fixed_w`): a 1.0.0 leftover — a dead duplicate of
  the `fixed_nat` sum computed above it (the anchored-branch math
  derives positions from the viewport directly). Removed; the
  zero-warnings rule holds again (full -Werror build clean).
- **Public API: +2** (`fdk_model_begin_batch`,
  `fdk_model_end_batch`). `make test` green — 762 [ok] checks
  including a new mutation-batching regression test (held fires,
  single release, one replay, nesting, quiet edges), and all
  three rigs re-verified end-to-end (click sweep pixel-exact,
  menu rig PASS, nav rig's new baseline recorded).


### Fixed — owner round 11: the dead rubber band, the Properties entry firing delete, the CPU peg, the wrong-row right-click

- **Menus (fdk-files): every menu entry fired the action ONE SLOT
  ABOVE it.** `item_activate` read the shared action registry at
  `idx - 1` (a 1-based left-over) while the registration wrote at
  `idx` — the same 0-based index the toolbar trampoline and the
  shortcut dispatcher both use. "Properties" ran "Delete
  permanently" (the delete-confirmation dialog the owner saw),
  "Move to trash" ran "Rename", and the FIRST entry of every menu
  fired nothing at all. Fixed to 0-based like its two siblings.
  Second defect in the same file: the context menus re-registered
  their entries into the shared 256-slot registry on EVERY popup —
  ~25 right-clicks walked the count past the array into silent
  memory corruption. Context menus now use a bounded 64-slot ring
  (`ctx_menu_build_actions`), and both dispatchers print the
  mnemonic-stripped "PHASE: act" telemetry under FDK_FILES_DRIG
  (the new menu rig's channel).
- **List/IconView: the rubber band was DEAD in a populated view —
  the drag-source controller owned every press.** The 1.0.0 veto
  (`fdk_widget_set_drag_veto`) is checked on the press's HIT
  TARGET, but a press inside a list hit-tests to a ROW (or the
  scrollview/scrollbar), never to the list — so the list's own
  veto was dead code and the controller armed on every press; at
  6px it fired `pane_begin_drag`, and the DnD backend grab broke
  the band mid-sweep (the "hold and drag does not work" report).
  The veto now lives where the presses land: every UNSELECTED row
  vetoes (its press is a potential band; the veto flips with the
  selection in `list_set_row`, so a SELECTED row keeps the classic
  drag arming), the rows container and the scrollview chrome veto
  (a new re-callable `fdk__scrollview_veto_input` keeps the
  scrollbars' vetoes truthful as bar visibility changes with the
  scroll range), and the iconview's grid does the same.
- **Window/paint: fast highlight sweeps cost a full DnD gesture
  and an O(widgets) walk per motion event.** Each fast
  hold-and-move "began a drag" (the veto defect above) whose
  X11 source loop walks the window tree per motion
  (`xdnd_window_under`'s XQueryTree descent), and the controller
  itself re-walked the whole widget tree on EVERY motion to find
  its one armed source. The armed source is now a root-cached
  pointer (set at press, cleared at fire/release/destroy — a
  mid-press destroy retires it safely), and with the veto fixed
  the spurious gestures are gone at the source. The iconview's
  band apply also stopped invalidating the WHOLE view per motion
  pixel (flipping cells damage only themselves; the band tint
  damages old∪new rect, the List's round-9 discipline). Measured
  on the drag rig: steady-state app CPU 4.8% → 4.0% of one core;
  the fast-click sweep is now band-only work.
- **fdk-files: the right-click selected NOTHING — the context
  menu was flavored from the stale live selection.** A right-click
  on a folder with nothing selected opened the EMPTY menu; with
  something else selected, every entry acted on THAT file (the
  "it asks me if I want to delete a file I never clicked" shape).
  The engine grew the public pointer-to-row/item maps
  (`fdk_list_row_at`, `fdk_iconview_item_at` — header strip
  excluded, scroll offset included, FDK_ERR_NOT_FOUND off-rows),
  and the right-click now selects the row it points at before the
  menu opens (a right-click inside a multi-selection keeps it),
  with the menu flavored from the CLICKED item (CTX_FOLDER vs
  CTX_FILE). New menu rig (run_menurig.sh) proves it end-to-end:
  right-click beta-dir → "PHASE: sel 1 beta-dir", click the last
  entry → "PHASE: act Properties" (was "Delete permanently").
- **Public API: +2** (`fdk_list_row_at`, `fdk_iconview_item_at`),
  both additions in the widgets header's established pattern; no
  signature changes. `make test` green (764 [ok] checks incl. 4
  new tests: the veto interplay — unselected rows band, selected
  rows drag, the veto follows the flip; gutter/scrollbar presses
  never drag; the O(1) armed lookup's mid-press destroy; and the
  two geometry maps).


### Fixed — owner round 10: the wrong-row selection (and the X11 paint pace)

- **List: every list-level press selected ONE ROW LOWER than the
  pointer.** The 1.0.0 columns round inset the rows below the
  header strip but left the rubber-band math dividing raw
  list-local y by the row height — so a press on the header
  selected row 0, a press in a row's empty right half (round 9's
  gate falling through to the band) selected the row BELOW it, and
  every sweep shifted one row down. The owner's reports — "clicking
  a folder highlights the item below", the context menu acting on
  the wrong file (it flavors from the live selection) — are all
  this one offset. Fixed by routing every band↔content mapping
  through a `list_content_top()` inset: anchor capture, apply-band
  row math, band rect, paint anchor, and both auto-scroll edge
  tests. No-columns lists are byte-for-byte unchanged
  (`content_top == 0`). Round 9's own test had enshrined the bug
  (a press at visual row 0 asserting row 1); it now asserts the
  visual row, and the new round-10 regression test pins header
  clicks selecting NOTHING, right-of-text chases landing on the
  VISUAL row, sweep edges exact, and the same sweep at a NONZERO
  scroll offset (where a sign error would hide until scrolled).
- **X11: `fdk_surface_frame_ready()` was always-true.** The op
  table entry existed only on Wayland, so the canonical
  `pump; if (frame_ready && damage) paint` loop painted UNPACED on
  X11 — one full acquire/raster/present per loop pass while damage
  kept arriving (a drag damages every motion), which is the
  "one core pegged during drags" report's mechanism. X11 now ships
  `window_frame_ready`: a 17ms minimum present interval (both render
  lanes stamp `last_present_ms`), the same capacity gate Wayland's
  1.0.0 op uses (both SHM slots in flight → not ready, instead of
  blocking inside the next acquire), and the two exceptions
  (nothing presented yet; popup windows — first-present latency is
  their correctness). Measured on the drag rig (250 injected
  motions/s, fdk-files DRIG layout): steady-state app CPU 13.2% →
  4.4% of one core; selection sweeps re-verified pixel-perfect
  after. No public API change (the op slot was already in the
  table's future).

### Fixed — owner round 9: the five interaction reports (ported onto the restored lineage)

- **List: stuck rubber bands after menus close.** A dying popup
  chain swallows the POINTER_UP that would end a sweep, leaving the
  band's highlight on screen. Menu teardown now calls
  `fdk__list_cancel_bands_all_roots()` (internal seam), which ends
  every live band in every root — and clears the rows' pending_band
  arms, whose release was swallowed with it.
- **List: presses right of a row's content are empty list space.**
  A row accepts presses only over icon + name text + padding
  (columns-aware); a press beyond starts the rubber band at the
  list, exactly like the space below the rows. Round-6 press-time
  chase semantics preserved (a no-motion click at a row's y selects
  that row).
- **Menu: context menus are single-session at the engine level.**
  `fdk__menu_popup_open_full()` closes every other context session
  first; N right-clicks can no longer stack N popups.
- **Menu: the opening press's release cannot activate an item.**
  Menu views press-arm; an UNARMED POINTER_UP is swallowed
  (GTK/Qt behavior) — the "Properties acts like Delete" misfire
  class.
- **List: damage-limited band repaint.** The sweep invalidates the
  union of the old + new band rects (new internal seam
  `fdk__widget_invalidate_rect`) plus only the rows that flip;
  previously every motion pixel repainted the whole list (the
  large-directory CPU ramp).
- **Window: popup destroy damages the parent over the popup's
  anchor rect.** X11 override-redirect popups unmap off the root,
  so the parent never received the Expose for the covered region —
  a stale box where the menu was.

### Incident record — the round-9 lineage restore

A mid-session sandbox recycle produced a diverged working tree
that was committed and force-pushed over origin/main, displacing
round 8's head. The true head (9fc4fb1) was recovered from GitHub
by full SHA and restored; the displaced work lives on
backup/round-9-alien and was re-ported onto the true lineage this
round (the five fixes above). Policy from the owner, permanent:
no force-pushes — fast-forward only, divergences get reconciled.

## [Unreleased-candidate features] — 2026-11-01 (on the rc2 line)

### Changed — §23 Part A: the add-on package renamed (the owner's layout fix)

- **The folder is the add-on's own directory, not the weaker
  `shell/` layout.** What actually happened this round, honestly:
  the round began by discovering that the `shell/` directory
  added earlier duplicated a package the tree ALREADY had —
  a mature directory the tree already had, a complete v0.5.0 add-on (its own build, tests,
  packaging for five distros, the prefix-isolation proof) that a
  sloppy search had missed. The duplicate `shell/` is DELETED,
  and the real package is renamed in place:
  - the add-on directory renamed in place (git mv — history
    preserved; no rewrite of pushed history)
  - headers renamed with the package (the public header and
    the desktop header)
  - artifacts renamed with the package (library + SONAME/.a,
    the .pc file, the exports
    map, the fedora spec, the debian control files, every
    APKBUILD/PKGBUILD/template entry, all CI scripts
    (packaging/ci/*.sh, .github/workflows), the isolation proof
    and the rigs — 73 files updated, zero stale
    (bare) or `shell/` references remain (grep-verified).
  - the root Makefile's own add-on build rules are GONE (they
    built the duplicate); the add-on make targets
    now delegate to the
    package's own Makefile, which is the one true build.
  - Public API names are UNCHANGED —
    the package's API is its own namespace, and renaming
    functions would break its consumers for no layout gain.
- **Verified after the rename**: the package builds warning-clean
  and its full X11 suite passes (output info, per-output
  anchoring, RandR probing with stale-index fallback, 3-deep
  popup cascades, family sweep). FDK's own suites unaffected
  (the root rules never built them together).

### Added — the shell-surface add-on, and the GTK4/Qt6 parity audit

- **The owner's requirement**: developers must be able to build
  ANY app with FDK — shell surfaces included — and the API
  coverage against GTK4/Qt6 needed a real audit, not a feeling.
  docs/PARITY.md is that audit: category by category (797
  exported symbols, 36 widget classes, both backends), verdicts
  per category (PARITY / different-shape / PARTIAL / GAP), and a
  prioritized gap list. Headline: honest parity for the
  application core (windows, input, complex-script text,
  AT-SPI-grade accessibility, theming, D-Bus/tray/notifications/
  session, i18n); real gaps = clipboard richness, gestures,
  text-on-extension-surfaces, markup attribute depth, printing.
- **The shell-surface add-on**:
  panels, docks, notification shells, OSDs — layers with LIVE
  setters (level/anchor/margin/exclusive-zone/size/keyboard),
  output enumeration + hotplug + per-output surfaces, popup
  cascades, a notification server, a tray host. Rendering,
  events, input regions, popups, subsurfaces ride FDK's
  extension API unchanged (the docs/EXTENDING.md reference
  consumer). See the package's own README for the complete
  inventory and its five-distro packaging.

### Verified — THE "FIX RATHER THAN SUPPRESS" ROUND: every suppression entry re-tested for fixability by experiment

- **The owner's question**: leaks that CAN be fixed should be fixed,
  and only the truly unfixable stay suppressed. Every entry in
  `tests/data/lsan-gl.supp` was therefore re-examined with measured
  probes (unsuppressed, `LSAN_OPTIONS=exitcode=88`), not assertion.
- **The GLX/Mesa block is INHERENT**: a probe that opens the display
  and creates NO context still ends with 108764 B / 482 allocations
  — the pile is the per-Display driver screen state. The complete
  canonical context lifecycle on top retires ZERO bytes of it
  (identical totals), FBConfig + shader work adds only 1 tracked
  allocation, and the stack exports no public release symbol at all
  (glXReleaseThread/glXTerminate absent at runtime; the only
  candidates are private ABI — __glDispatchFini, driDestroyScreen).
  FDK's X11 GL teardown was already maximal.
- **The fontconfig block is CONTRACT-BOUND**: appending one FcFini()
  to a probe mirroring fontscan.c's exact usage retires exactly the
  320 B this block covers — but FcFini destroys process-global state
  with no refcount while the host app may share the library (the
  coexistence rule), and the remaining ~6.6 KB of init state is
  equally FcFini-gated, so no partial-fix shape exists either.
- **libdbus-1 is PULLER-SIDE** (libnvidia-eglcore, proprietary, §22)
  and **libEGL_nvidia is AUDITED AND INTERMITTENT** (§22: teardown
  audited call-by-call, its one real defect fixed and rig-guarded).
- **The verdict, in one line**: the suppression file is exactly as
  small as it can honestly be — every block now carries the
  measurement of why no FDK-side fix can retire it
  (docs/testing.md's fixability-verdict section has the full data).

### Fixed — THE EXIT-LEAK ROUND (§22): the canonical teardown tail, and the NVIDIA exit reports made clean with proof

- **The second GTX 1050 Ti exit report**: the app itself is fine
  ("exited cleanly after 645 frames", zero WARN lines), but
  LeakSanitizer's exit report names 256 B in 4 allocations with
  every frame inside `libEGL_nvidia.so.0+0xc94e1` — and ASan's fast
  unwinder cannot walk the stripped NVIDIA library, so "no FDK
  frame" was not yet proof of anything.
- **THE AUDIT (before any suppression was written)**: the lane's
  whole teardown was read call-by-call, per window AND at connection
  shutdown, including every early-return/error/downgrade path. The
  verdict: the destroy ladder was already canonical (MakeCurrent(nil)
  first, eglDestroySurface BEFORE wl_egl_window_destroy, the share
  root dying last before eglTerminate, single-window connections
  included) — but the sequence's TAIL CALL was missing everywhere:
  **`eglReleaseThread` was never resolved and never called**. That
  call is the documented way (EGL 1.5 §3.2) a thread that used the
  display retires its per-thread driver state — and per-thread state
  that only a tail call frees is EXACTLY the shape of a small
  always-there exit allocation on a driver that mallocs its thread
  record. `fdk_wayland_egl_shutdown` now resolves eglReleaseThread
  (optional, EGL 1.2+) and calls it once after eglTerminate — FDK
  touches EGL from one thread by contract, so one call retires it.
- **THE RIG LEG THAT FAILS WITHOUT IT**: the sway suite's GL-lane
  group gains the §22 teardown leg — FDK_GL_TRACE now logs the
  window ladder (MakeCurrent(nil) -> DestroySurface ->
  DestroyContext -> wl_egl_window_destroy) and the connection ladder
  (share root -> eglTerminate -> eglReleaseThread) in order, and a
  new test seam
  (`fdk__wayland_egl_test_teardown_calls`, counting shutdowns and
  reached tails) lets the leg assert counters equality. A/B-verified
  in the rig: with the tail call compiled out the leg ABORTS at
  `rel_calls == tears`; with it, green.
- **THE SANDBOX PROOFS**: the sanitized exit probe (lane engaged on
  llvmpipe, presents, clean shutdown) exits with a ZERO-byte LSan
  report — under the suppression file AND under the slow unwinder
  (`fast_unwind_on_malloc=0:malloc_context_size=40`, full stacks) —
  so FDK's own EGL teardown leaves nothing on Mesa. The negative
  control: a one-line guarded `malloc(137)` in the probe's `main`,
  run WITH the suppression file active, is still reported (exit 1,
  the frame named) — per-module suppressions cannot hide an FDK
  leak. The no-tail A/B on Mesa also exits clean: Mesa keeps no
  per-thread malloc the tail would free, which is why the 256 B is
  NVIDIA-only and only the owner's machine can observe whether the
  fixed tail already retires it.
- **THE SUPPRESSION (with its proof)**: `leak:libEGL_nvidia` joins
  `tests/data/lsan-gl.supp`, documented with the audit verdict, the
  fix, and the remaining unknown (if the 256 B disappears from an
  unsuppressed owner run, the fix closed it; if it stays, the driver
  allocates at engagement and never frees). The libdbus entry's
  comment block carries the round's other finding: the leak's stack
  runs through the SAME recurring driver site, and the owner's
  `LD_DEBUG=libs` line ("RUNPATH from file /usr/lib/libasan.so.8")
  names whose RUNPATH the loader consulted, NOT who requested the
  load — Arch's package closure rules out every candidate this
  sandbox could check (nvidia-utils, wlroots, labwc,
  libnvidia-egl-wayland, libnvidia-glcore: no libdbus anywhere), so
  the pull is a runtime dlopen inside the proprietary chain and the
  decisive owner-side command is now `LD_DEBUG=files ... | grep -B2
  -A2 libdbus` (its "needed by" names the requester). Both entries
  stay per-library: any leak whose stack contains an FDK frame fails
  the run — that is the negative control's whole point.
- **THE FOLLOW-UP'S ANSWERS (the owner's LD_DEBUG=files run, same
  day)**: the puller is NAMED — `libnvidia-eglcore.so.580.196.07`.
  The trace's "dynamically loaded by libasan.so.8" line is an
  artifact (ASan interposes dlopen, so the loader attributes the
  open to the interceptor — reproduced in-sandbox both ways), the
  NODELETE activation is the driver's RTLD_NODELETE flag, and the
  driver's failed non-handle dlsym names the requester by path in
  the loader's error. Confirmed against the exact driver binary
  from NVIDIA's installer: libnvidia-eglcore dlopens
  "libdbus-1.so.3" and dlsyms the full dbus API (no DT_NEEDED —
  why every package closure came up empty); the GLX twin
  libnvidia-glcore carries the same client. And the 256 B question
  is closed by arithmetic: the owner's post-fix unsuppressed run
  totals 1022 B in 7 allocations = 766 B libdbus + the SAME 256 B
  in 4 objects. Read together with the sign-off run (below), the two
  post-fix runs say the 256 B is INTERMITTENT run-to-run on this
  driver rather than deterministic state the fix failed to retire,
  so the `leak:libEGL_nvidia` entry is NECESSARY for the runs where
  the block appears; the fix stays (canonical sequence,
  rig-guarded). Clean-exit contract unchanged: with the suppression
  file active the run exits 0 with an empty report.
- **THE SIGN-OFF RUN (§22 closed clean)**: the owner's run with the
  suppression active — "exited cleanly after 587 frames", full
  stack engaged (GL lane, driver-probed upload path, HarfBuzz+
  FriBidi, atspi) — ended with LSan's Suppressions-used footer
  reading exactly 3 / 754 B / libdbus-1 and no leak report: the
  clean-exit contract delivered on the driver desktop. The footer's
  semantics were verified directly before recording it (a controlled
  4-allocation probe run with no file, a fully-matching file, and
  the repo's own file): the table lists ONLY matched templates,
  count/bytes sum direct+indirect, a fully-suppressed run exits 0,
  a nothing-matched run prints no footer, and any unsuppressed
  leak still exits 1 — so the footer is a per-run audit naming
  which third-party modules' state that run retired (this run:
  libdbus-1 alone — no libEGL_nvidia row, closing the intermittence
  reading above). The repo's file also parses clean and
  over-suppresses nothing (an executable-owned 137 B leak is fully
  reported through it — the negative control, re-run for this
  round), and the sandbox's canonical rig re-verified end-to-end on
  the same file: `make test-x11` (sanitized build, throwaway Xvfb,
  the Makefile's own LSAN wiring) passes with no leak report.

### Fixed — THE GL LANE'S REAL NVIDIA ANSWER: probe the driver, ladder the failures, share the group (§21, the GTX 1050 Ti follow-up)

- **The owner's report after the §20 fix**: the crash is gone
  (3327 frames, no -101), but the GPU lane still fails its FIRST
  present on the GTX 1050 Ti and the window runs on the CPU path —
  and the WARN named neither the failing call nor the error code.
- **THE SHARE GROUP (a real bug on every driver, caught by the new
  trace on its first live run)**: the quad program and VBO were
  minted under the FIRST window's context while per-window contexts
  were created with `share=NULL` — object names are per share
  group, so every LATER window's `glUseProgram` referenced a name
  invalid in its own context (`GL_INVALID_VALUE`) and silently
  downgraded to the CPU path, since the lane landed. One share-ROOT
  context (never current — surfaceless by construction) now anchors
  a group every window context joins; dead windows' textures queue
  into a graveyard the next present drains.
- **THE PROBE**: extension strings are input, not verdict. With the
  first context current, each of the four upload shapes gets a real
  4x4 upload — both BGRA internalformat spellings tried,
  spec-legal row first — plus the sub-rect update and the
  ROW_LENGTH pixelstore; the first shape whose every call drains
  clean becomes the connection's shape. This is the honest answer
  to the report's signature (both extensions advertised, the fast
  path chosen, the first present still failed):
  `internalformat=GL_RGBA` paired with `format=GL_BGRA_EXT` is in
  NO ES2 table — the EXT_texture_format_BGRA8888 row is
  BGRA/BGRA/UNSIGNED_BYTE — and Mesa tolerates the mixed pair
  while NVIDIA's strict validator rejects it.
  `glPixelStorei(GL_UNPACK_ROW_LENGTH)` is now touched ONLY when
  `GL_EXT_unpack_subimage` exists (the pname itself is
  GL_INVALID_ENUM without it — the old code set it unconditionally,
  poisoning strict drivers even on the fallback shapes).
- **THE RETRY LADDER**: a present that fails with a GL error or a
  swap failure is no longer an instant permanent downgrade — the
  ladder rotates to the next un-rejected shape, recreates the
  texture, retries once per shape; only the exhausted ladder
  downgrades (self-check mismatch, OOM and unsupported framebuffer
  stay fatal). The §20 single-owner rule is untouched: the ladder
  never attaches or commits while the EGL surface owns the
  wl_surface.
- **THE VISIBILITY**: every lane error now arrives as a NAME
  (`GL_INVALID_ENUM`, `EGL_BAD_ALLOC`, ...) with the exact call, the
  upload shape, the texture format, the rect and the stage in one
  WARN line; `FDK_GL_TRACE=1` logs every GL/EGL call of the first
  presents plus a one-per-connection engagement dump (EGL
  vendor/version/client APIs, config attributes, context API and
  version, GL strings, extension flags, wl_egl_window attached
  size); stale setup errors are drained and WARNed at every setup
  stage and immediately before the first present, so the present's
  verdict can only blame the present's own calls.
- **Test seams**: `FDK_GL_PROBE_SKIP=<n>` (reject the first n shapes
  at the probe), `FDK_GL_POISON_FIRST=1` (a guaranteed-invalid
  upload call at the first present — a REAL driver error the ladder
  must recover from). Tests: the Mesa-side proof runs eleven green
  GL-lane legs under sway/llvmpipe, including two formerly VACUOUS
  legs (the core-only leg never acquired a framebuffer before
  checking the path; the two-window leg asserted window 2's path
  only before its first present) and two that silently rode the
  FDK_GL_FORCE leaked by a neighbor.

### Fixed — the libdbus-1 exit leak is third-party, now suppressed with proof

- The owner's sanitized run ends with LeakSanitizer naming
  `libdbus-1.so.3` (754 bytes, 3 allocations, frames entirely inside
  the module). FDK's D-Bus is its own native wire client
  (src/dbus/) and dlopens no D-Bus library; the sandbox's
  sway/Mesa/fontconfig stack provably never loads libdbus either.
  The puller is a third-party library on the owner's machine;
  `tests/data/lsan-gl.supp` now carries the module-scoped
  suppression (documented with the diagnostic command that names
  the puller: `LD_DEBUG=libs`), so debug runs end clean while
  everything FDK allocates stays under strict accounting.

## [Unreleased-candidate features] — 2026-10-31 (on the rc2 line)

### Fixed — TWO PRESENTERS ON ONE wl_surface: the Wayland connection death on NVIDIA (the §20 report, re-landed)

- **The owner's report**: `text_demo "مرحبا" "नमस्ते"` on
  Arch/labwc/NVIDIA GTX 1050 Ti (proprietary driver) — the GL lane
  engaged ("GPU present lane active", "GPU lane upload path: BGRA
  texture + UNPACK_ROW_LENGTH sub-rects"), then
  "WARN GL error during GPU present — downgrading",
  "ERROR backend dispatch_pending failed (-101)", "exited cleanly
  after 0 frames".
- **The root cause**: FDK put a SECOND presenter on the toplevel
  wl_surface. The lane engages at the first framebuffer acquire —
  BEFORE the first xdg configure — and NVIDIA's EGL wraps the
  surface in the explicit-sync role at eglCreateWindowSurface time
  (the compositor advertises wp_linux_drm_syncobj_manager_v1). After
  the wrap, every attaching commit needs an acquire point; FDK's own
  wl_shm background-buffer attach at configure (and at resizes)
  carries none — protocol error 4, dead connection.
- **THE SINGLE-OWNER RULE**: while the GL lane owns a surface, its
  buffer state belongs to EGL. `attach_background_buffer()` REFUSES
  (returns without attaching) for lane-owned windows — at the
  configure seam, the resize seam, and any future caller; the
  classic wl_shm commit path (`commit_render_pending`) refuses
  defensively (one WARN + FDK_ERR_INVALID_STATE — unreachable by
  construction); `hide()` sends no NULL-buffer commit either. A
  lane-owned window maps via its own first present (the deferred EGL
  first frame at configure, or the next present). The DOWNGRADE
  ORDER is documented as load-bearing: the wl_shm slot is created
  (no attach), the EGL surface/context are destroyed, and only then
  does the classic present attach — the commit-path refusal makes
  that order mechanical. REJECTED DESIGN (recorded in
  docs/DECISIONS.md D-036): implementing linux-drm-syncobj on the
  client side.
- **The registry diagnostic**: one INFO line when the compositor
  advertises wp_linux_drm_syncobj_manager_v1 (detected by string
  match — never bound), and the lane-engagement line now states
  whether the flag was seen and why the single-owner rule matters
  there.
- **The dead-connection detail line**: context.c promises "the
  backend logged its one detail line" — the owner's run printed
  NONE. Two fixes: (1) the present path's trailing flush marked the
  connection dead QUIETLY (a hand-rolled display_dead flip), so the
  pump's circuit breaker then returned -101 without any backend
  line — every dead-connection mark now routes through the shared
  `fdk__wayland_connection_dead()` helper; (2) the flush/EPIPE
  paths can beat the compositor's wl_display.error event out of the
  socket — one guarded, never-blocking read surfaces it so the line
  still names the interface, object id and protocol error code (and
  says so honestly when nothing was recorded).
- **The rig lie**: tests/test_wayland_integration.c re-executed the
  activation child through `system("/proc/self/exe …")` — under a
  shell /proc/self/exe is the SHELL's own image, the child never
  ran, and the "[ok]" was dash reading empty stdin. fork()+execl
  now; the launcher-flow leg exercises a REAL child process (its
  focus verdict flips through the voucher, observed live).
- **The attach-count rig** (test_wayland_integration.c): re-execs
  the binary under WAYLAND_DEBUG=1 with FDK_GL_FORCE=1 and counts
  the toplevel's wl_surface.attach requests BY QUEUE. Verified both
  directions on sway + llvmpipe: with the fix stashed, exactly ONE
  Default-Queue attach lands at the first configure; with the fix,
  ZERO — N presents -> N attaches, all from the driver's queue. The
  sandbox has no DRM render node (no explicit-sync role to enforce
  the rule), so the COUNT is the proof there; the role itself needs
  the owner's hardware (TESTING-ON-DESKTOP §18/§19). The §10
  core-only leg now forces the lane so it exercises on swrast.

### Fixed — the four extreme-scale stress legs, their two defects, and the wrap pen (the re-land's completion)

- tests/test_stress.c is back as a PERMANENT `make test` member:
  the 10k-row List (append/measure/scroll/select/paint, growth
  linear), the 20k-line document (wrap exactness at depth), the
  1200 fps full-screen 1920x1080 animation with interleaved
  scrolling (1200 frames + 150 wheel events, per-frame leaks would
  LSan-fail), and the 1 MB single-line MIXED-direction text
  (shaped in ~1.5 s under ASan; the RTL fragments are load-bearing).
- **The shaped attribution was O(n_glyphs x n_clusters)** — an
  insertion sort of the distinct cluster ids (quadratic on RTL
  fragments, where harfbuzz emits DESCENDING ids) plus a per-glyph
  linear scan of the sorted array. Replaced with the O(n) boundary
  table (indexed BY the cluster id): byte-identical output by
  construction; 128 KB took ~19 s before, 1 MB now takes ~1.5 s
  under ASan.
- **The wrap engine differenced TEXT-ABSOLUTE f32 prefix sums**:
  past 2^21 px an f32 snapshot's ulp is a fraction of a pixel, and a
  20k-line document's deep lines carried that quantization into
  their advance_width (identical lines, different widths by depth).
  The shaped result now carries the UN-narrowed f64 prefix sums
  (`cp_pen_exact`); the wrap engine differences those (the legacy
  walk keeps its 1.4.x f32 metrics bit-exact). Pinned: every one of
  20k identical lines reports the identical advance.
- **The animation-handle contract now states the grace window in
  its real units**: roughly 64 pump passes (GRACE_PUMPS in
  src/widget/animation.c), ~1 s of display cadence — the header
  used to say only "roughly a second of animation traffic".

### Fixed — the selection highlight cut grapheme clusters (shaped Devanagari; the rectangle grew and spilled)

- Selecting the cluster "स्ते" drew a highlight that spilled onto
  neighbours and grew as the selection moved, and a bidi selection
  was one wrong rectangle. Two stacked causes:
- **The highlight geometry**: the Entry/TextView selection rect was
  computed from byte-PREFIX measurements ("width of text[0..lo)" to
  "width of text[0..hi)") — a prefix that CUTS a cluster shapes
  differently than the whole cluster (a half-form consonant's
  advance is not the cluster's own). New public seam
  `fdk_font_selection_spans_utf8()`: the visual spans of a logical
  byte range read straight off the shaped result — a cluster is
  selected when its byte range intersects the selection (cutting it
  selects it WHOLE), its box is its ADVANCE span (zero-width marks
  never widen it), and a bidi range yields SEVERAL spans (one per
  visual run). The Entry and TextView highlights paint exactly
  those spans; the TextView keeps the classic full-line hair for
  whole-line selections.
- **The entry's own text drawing**: it drew the text as THREE
  NUL-terminated segments split at the selection edges — a boundary
  inside a cluster made the segments reshape (a lone half-form, a
  leading virama), so the glyphs themselves MOVED when the user
  selected mid-cluster, and kerning reset at the selection edges.
  All three segments were the same color (the accent rect says
  "selected"); the entry now draws the whole line in ONE pass —
  identical ink whether or not anything is selected.
- **Pixel-level pins** (tests/test_selection_pixels.c): API-level
  (cluster snap — a mid-cluster selection equals the whole-cluster
  box; Hebrew logical->visual mapping; the bidi two-span split with
  a real gap) and paint-level (the scanned band extent equals the
  span geometry; the mid-cluster band IS the whole-cluster band;
  the TextView bidi selection paints two separated bands on one
  line). Devanagari legs skip honestly without an Indic face.

### Fixed — the tablet crash on the owner's desktop: three stacked root causes in D-3's X11 translation (the §21 report)

- **The owner's report**: the ASan+UBSan debug build of
  `text_demo "مرحبا" "नमस्ते"` on Arch/Openbox/X11/NVIDIA started
  fine — RandR 1.6, XI 2.2 touch, "X Input: 4 tablet device(s)
  detected" (a desktop with no stylus at all), GPU lane active,
  HarfBuzz+FriBidi, XIM — then died the moment the window was used:
  `x11_xi2.c:589: signed integer overflow: -708600696 * 4`, followed
  by `SEGV on unknown address` in `xi2_valuator`, with `r12 =
  0xbebebebe00000008` (ASan's uninitialized-heap fill as a pointer).
  Reproduced against Xvfb with the pre-fix code
  (scripts/xi2_crash_repro.c, the evidence rig): the same SEGV, the
  same garbage, byte for byte.
- **Root cause 1 — the mirror was missing XIDeviceEvent's `flags`
  field** (the int between `event_y` and `buttons`): every valuator
  tail field was read EIGHT BYTES EARLY. "valuators_mask_len" was
  really the low half of `buttons.mask`'s heap pointer (arbitrary
  garbage: -708600696 on the owner's machine, -1631266776 on the
  rig's); "valuators_mask" was really `{valuators.mask_len,
  uninitialized pad}` reinterpreted as a pointer — the exact
  0xbebebebe00000008 the register dump showed. The old `* 4` (the
  mask length was treated as 4-byte units where libXi hands out
  BYTES) overflowed signed int, wrapped to a huge positive, and
  defeated the bounds check — then the wild pointer dereferenced.
- **Root cause 2 — the event-type constants were off by one**: the
  protocol numbers DeviceChanged=1, KeyPress=2, KeyRelease=3,
  ButtonPress=4, ButtonRelease=5, Motion=6; the file carried 3/4/5.
  The window was selecting KeyRelease instead of Motion, and every
  button event arrived under the wrong arm.
- **Root cause 3 — the scan classified any slave pointer with >= 3
  valuator axes as a "tablet"**: wheel mice, XTEST pointers,
  touchpads — hence "4 tablet device(s)" on a stylus-less desktop,
  feeding ordinary mouse clicks into the broken reader. The 1.0.0-rc2
  rule requires STYLUS EVIDENCE: an absolute axis labeled "Abs
  Pressure", or the unlabeled-tablet shape (no relative axes, >= 3
  absolute, "Abs X"/"Abs Y" present). Every verdict now logs its
  evidence at INFO, once (see docs/DECISIONS.md D-033).
- **The fix, layered**: the corrected mirror is verified against
  the real XInput2.h AT BUILD TIME wherever the header exists
  (`__has_include` + static asserts on layout and constants — the
  discipline the file header always promised and never had; the
  missing `flags` would have been caught by exactly this check);
  the constants are the protocol's; the valuator read is bounded by
  the device's real axis count, the mask's byte length and the
  packed values indexing, all in size_t, with NaN/absurd-value
  guards; the module record is zeroed at allocation; buttons
  outside 1..255 are consumed untranslated.
- **The regression pins** (test_x11_integration.c): a
  hostile-cookie rig feeding the REAL cookie handler the crash's
  exact values (garbage mask_len + the 0xbebebebe pointer) plus
  empty/negative/INT_MAX/NULL/absurd tails, out-of-range axes,
  wrong evtypes (KeyRelease must never reach the tablet cast) and
  unknown device ids — no crash, correct semantics; a
  classification regression (the stylus-evidence rule over
  synthetic axes — the owner's wheel mouse rejected, labeled and
  unlabeled tablets accepted with exact axis numbers); and REAL
  XTEST cookies end-to-end through the dispatch loop with a tablet
  record injected for the XTEST pointer — the exact pipeline the
  crash died in, now sanitizer-clean.

### Fixed — the Wayland GPU lane's upload contract on strict-ES2 drivers (the §19 report, re-landed)

- The 1.0.0-era NVIDIA fix (GTX 1050 Ti + labwc + the proprietary
  driver: the lane engaged, the FIRST present failed with a GL
  error) was lost with the deleted repository's last commits; the
  surviving work-in-progress patch is re-landed and completed.
- **The root cause**: the upload used `GL_BGRA` +
  `GL_UNPACK_ROW_LENGTH`, which are NOT core GLES2 — BGRA needs
  `GL_EXT_texture_format_BGRA8888`, ROW_LENGTH needs
  `GL_EXT_unpack_subimage` (or ES3). Mesa's stacks expose both,
  which is why every rig passed; NVIDIA's ES2 context rejects
  them.
- **The fix**: the upload contract is decided once per connection
  off the ES2 extension string and has four shapes — BGRA +
  ROW_LENGTH (the fast path), BGRA with tightly packed sub-rects,
  a CPU-swizzled RGBA twin with ROW_LENGTH, and the twin with
  packed sub-rects (pure core GLES2 — the NVIDIA shape).
  `FDK_GL_CORE_ONLY=1` pins the fallbacks on any driver (the rig's
  deterministic leg). The teardown order and the per-draw viewport
  from the same lost fix ride along. The fallback byte math (the
  R<->B twin swizzle, the tight row packing) is pinned DRIVERLESS
  in tests/test_gl_lane_fallback.c — no EGL driver needed.

### Fixed — the shaped pen accumulated in f32 (extreme-scale drift), re-landed

- The pen that sums a line's glyph advances took one f32 rounding
  per glyph; at thousands of glyphs the drift moved measure, wrap
  and caret geometry. The accumulator is now f64 (the f32 snapshots
  the consumers see are each narrowed once, never compounded; the
  legacy walk keeps its 1.4.x bit-compatibility). Pinned by an
  extreme-scale leg (3000 glyphs, block-repetition linearity to the
  snapshot ulp) in tests/test_shaping.c.

### Fixed — the D-Bus startup notice printed three times

- "D-Bus unavailable (no endpoint accepted a connection …)" fired
  once per subsystem at startup (notifications, the tray watcher,
  the settings bridge). The verdict belongs to the machine, not the
  caller: the whole process reports it exactly once. Pinned by a
  counting-sink test (three connection attempts, one line).

### Fixed — the §10 desktop report: Arabic and Devanagari drew .notdef while Han worked (the unsorted script table)

- **The owner's test**: `text_demo "مرحبا" "नमस्ते"` under labwc —
  Han fell back to the CJK face (the log said so), Arabic and
  Devanagari drew .notdef rectangles with NO fallback log line at
  all: the resolver was never even ASKED for those scripts.
- **The root cause** (candidate 1 of the report's own list): the
  script range table in src/text/shaping.c — the table
  `sh_script_of()` binary-searches to classify every codepoint —
  was grouped by script, NOT sorted by `lo`. A binary search over a
  misordered table silently misclassifies whole blocks as COMMON
  (inheriting no script, requesting no fallback, drawing .notdef).
  The original M-2 table already lost Greek, Cyrillic, Kana and
  Hangul this way; when C-3 appended the four emoji ranges at the
  end, the shifted midpoints ate Arabic, Devanagari, Hebrew, most
  Indic, Ethiopic and more — leaving Han (and Thai, Lao, Khmer,
  a few others) working purely by the luck of the search path.
  That luck is why the shipped tests passed: the CJK fallback leg
  probed the one script that still resolved.
- **The fix, at the root**: the table is now sorted by `lo` (65
  disjoint ranges, every boundary and midpoint verified to resolve),
  with the ordering requirement documented at the table itself.
- **Failures are LOUD now** (the report's explicit ask): a script
  run that needs a fallback and gets none WARNs once per (font,
  script) with the reason — no fontconfig answer, face failed to
  load, or face does not map the probe — plus the chosen face stays
  INFO per script, and an uncovered codepoint that no script bucket
  claims WARNs once per font instead of silently tofu-ing. A
  process-lifetime self-check in `sh_probe()` ERRORs on any future
  ordering violation.
- **Candidate 3 hardened at the root**: `fdk_text_resolve_fallback_font`
  (internal) takes a skip list, and `sh_fallback_for` retries up to
  four fontconfig candidates — a ranking that hands back a
  non-covering or unloadable face now means NEXT candidate, not a
  dead script.
- **The rigs**: tests/test_shaping.c gains the six-script fallback
  matrix (Arabic, Devanagari, Hebrew, Thai, Bengali, Han) — a
  primary that covers none of the script must come back with a
  resolved fallback, no glyph 0 anywhere in the run, Arabic joining
  (contextual glyph ids differ from the isolated cmap hits) and
  Devanagari conjunct merging (fewer glyphs than codepoints); a
  machine whose fontconfig CAN answer a script but whose shape
  used no fallback is a FAILURE, not a skip — that is exactly the
  §10 signature. tests/textdemo_rig.sh (+ `make test-textdemo`)
  is §10's hand-equivalent under Xvfb: text_demo --screenshot
  analyzed shaped-vs-legacy (a broken channel renders .notdef in
  both postures — delta collapses), with the Devanagari
  shirorekha-width anchor, the Han density anchor, and a legacy
  negative control proving the detector sees the failure. Both
  rigs were verified to FAIL against the original unsorted table.
- **CI**: the ubuntu job and all five family build containers
  install the per-script Noto faces (fonts-noto-core /
  google-noto-sans-*-fonts / noto-fonts / font-noto-* /
  noto-fonts-ttf) so the matrix's legs RUN; `make test-textdemo`
  is a named CI step. Where a family's repo lacks the package the
  legs skip with a disclosed message (the CJK-font discipline).
  Two CI-environment stabilizers landed with them: the runners pin
  the default sans-serif to DejaVu Sans via /etc/fonts/local.conf
  (a base Noto face would otherwise win fontconfig's default
  ranking and shift every text-metric-dependent layout the GUI
  geometry tests are calibrated against — the color-chooser
  OK-button click is the known case; the per-script faces serve
  the FALLBACK channel, which the charset-filtered query resolves
  independently of the default family, verified), and the packaging
  scripts' new comments are apostrophe-free — the quoting bug class
  commit 2537955 fixed once already terminated five RUN blocks
  mid-install on the first push of this change (the build died at
  a missing X11/Xlib.h with the whole apt line truncated).
- **What the owner should see now**: `text_demo "مرحبا" "नमस्ते"`
  renders joined Arabic and the स्ते conjunct from the fallback
  faces, with `shaping: fallback font ... for Arabic` /
  `... for Devanagari` INFO lines naming the faces — and on a
  machine missing a script's fonts, a WARN that says exactly which
  script and which codepoint found no face.

## [Unreleased-candidate features] — 2026-10-30 (on the rc2 line)

### Added — session save/restore: the desktop remembers your app (D-5 / B-19)

- **`include/fdk/fdk_session.h` (5 new functions) +
  `fdk_window_set_command` / `fdk_window_join_session` /
  `fdk_window_leave_session` (fdk_window.h; 869 exports, +8) +
  `FDK_EVENT_SESSION_SAVE` / `FDK_EVENT_SESSION_RESTORED` (enum 58/59,
  appended — no payload member, the union is untouched)**. The B-19
  session half: the protocols by which a desktop saves an
  application's state at logout and restarts it in the same place.
- **X11 — the ICCCM lane, automatic on every FDK window, no session
  object**: `fdk_window_set_command` writes WM_COMMAND (argv form,
  type STRING, NUL-separated — the command a session manager
  re-executes at the next login; the stored copy is one flat
  allocation); every FDK window now lists WM_SAVE_YOURSELF in
  WM_PROTOCOLS, and the WM's relay dispatches
  FDK_EVENT_SESSION_SAVE — save state, update the command INSIDE
  the handler, and FDK performs the protocol's own ack after the
  handler returns: the WM_COMMAND rewrite whose PropertyNotify is
  the "state saved" signal the session manager polls for (ICCCM
  appendix C; a zero-length rewrite acks a window that never set a
  command — the spec's own two-step reply). Deliberately NOT here:
  full XSMP (SM_CLIENT_ID / WM_CLIENT_LEADER / the ICE wire) — the
  obsolete lane is what light sessions actually speak; XSMP is
  fenced as a future family (D-032).
- **Wayland — xdg-session-management-v1** (staging,
  wayland-protocols 1.48+; the generated client code vendored, zero
  new build deps): `fdk_session_open` (reason launch/recover/
  restore + the persisted id to restore — NULL mints fresh), the
  coarse notify + `fdk_session_get_state` (PENDING → CREATED [an id
  to persist] / RESTORED / REPLACED [another client took it over]),
  `fdk_session_destroy` (state PRESERVED — the relaunch path) vs
  `fdk_session_remove` (the compositor forgets), and
  `fdk_window_join_session` — pre-show joins take the protocol's
  restore path (restore_toplevel before the surface's first commit;
  FDK_EVENT_SESSION_RESTORED arrives right before the first
  configure when the compositor remembered the window), post-show
  joins the fresh-add path (the already_mapped protocol error is
  structurally impossible). Ownership: the tracker keeps a join
  table — its teardown retires every window's toplevel-session
  proxy; window destroy unregisters WITHOUT remove_toplevel (a
  dying window is not the app asking the compositor to forget).
- **The honest platform truth**: NO released compositor advertises
  the protocol yet (probed live: sway 1.10/wlroots 0.18 and
  1.12/wlroots 0.20 both lack the global) — open answers the
  documented NULL everywhere today, the translation bodies are
  seam-proven (the D-3 discipline), and X11's WM_SAVE_YOURSELF lane
  is fully rig-proven end to end. X11 also answers NULL from
  `fdk_session_open` BY DESIGN (its lane needs no object) and
  UNSUPPORTED from join/leave.
- **Tests**: X11 — five legs (the WM_PROTOCOLS volunteer bit; the
  WM_COMMAND argv round trip; the full save round trip with the
  probe playing the session manager: ClientMessage in, event
  delivered, handler's NEW argv acked via PropertyNotify — the
  dispatch-before-ack ordering; the zero-length ack; the clear).
  Wayland — the live degradation leg (open answers NULL under sway
  1.10) + the seam legs (created/restored/replaced drive the REAL
  listeners: state, id, notify count, clean teardown; the
  toplevel-restored body dispatches FDK_EVENT_SESSION_RESTORED
  through the real conn->dispatch to a real window; the join
  validation matrix incl. the popup's no-toplevel refusal — the rig
  caught the argument-vs-state check order and it was fixed).

### Added — the foreign toplevel list: the taskbar substrate, both backends (D-4)

- **`include/fdk/fdk_toplevel.h` (6 new functions, 861 exports, +6)**:
  `fdk_toplevel_list_open/_close/_count/_get_info/_activate/_close`
  — the OTHER applications' top-level windows, enumerated and live,
  with the two actions a panel takes. The model is deliberately
  COARSE (the tray-host pattern, D-014): ONE changed callback — a
  window opened, closed, retitled, changed state, focus moved — and
  the consumer re-snapshots (count + get_info, struct_size copies
  with stable ids). Degradation everywhere: a compositor without
  the protocol or a bare X server answers NULL/empty — the
  taskbar-absent branch, never an error.
- **Wayland: wlr-foreign-toplevel-management** (vendored generated
  protocol code, zero new build deps) — the manager LAZILY bound at
  list-open (the initial burst rides the bind; a registry-time bind
  would send it into a listenerless proxy — lost forever, the rig's
  lesson), handle streams folded into the coarse model (every `done`
  fires one change), the empty set fires its change from open (an
  empty desktop never sends events at all — the rig's second
  lesson, now the header's documented "may fire during open").
  activate = handle.activate(seat); close = handle.close().
- **X11: EWMH** — the root's _NET_CLIENT_LIST snapshot
  (_NET_WM_NAME/WM_NAME titles, WM_CLASS res_class as the app_id
  analog, _NET_WM_STATE bits, _NET_ACTIVE_WINDOW focus), live on
  the root's PropertyNotify (membership/focus churn), activate =
  the S-6 _NET_ACTIVE_WINDOW message shape pointed at the foreign
  window, close = _NET_CLOSE_WINDOW (the polite path, never
  XKillClient). THE honest X11 gap (D-031): title edits on OTHER
  clients' windows are not notified (clients own their PropertyNotify
  selection) — a retitle lands at the next membership/focus churn.
- **Tests**: the X11 leg is a full fake-WM round trip (the roster
  with titles/app_ids/states/focus, the activate/close client
  messages RECEIVED at the WM — with the load-bearing XSync after
  the redirect selection, the race the second run caught —
  membership churn live, single-list rule, reopen after close, the
  bare-X empty posture) and the sway section is the LIVE substrate
  (the section's own window listed with title+app_id+the activation
  bit, a second window appearing, activation verified through
  sway's own tree, the close REQUEST landing as the window's own
  close-request event, membership removal, id stability, the
  single-list rule, reopen after close).

### Added — tablets / stylus input, both backends (D-3 / B-17)

- **6 new event types + the `fdk_tablet_event` payload**
  (`FDK_EVENT_TABLET_PROXIMITY_IN/OUT/DOWN/MOTION/UP/BUTTON`,
  appended enum values 52-57 — the ABI append rule, zero new
  symbols, 855 exports unchanged): drawing-tablet input as a
  first-class family, the touch model's sibling. PROXIMITY carries
  the tool's identity (`fdk_tablet_tool_type`: pen/eraser/brush/
  pencil/airbrush/mouse/lens/totem, plus an opaque per-tool id
  that distinguishes simultaneous tools); DOWN/MOTION/UP carry the
  tip's window-local position with pressure (normalized 0..1) and
  tilt (degrees); BUTTON reports side buttons with the high-bit
  release marker. NO synthesis layer — a stylus event is never a
  pointer click.
- **Wayland: the zwp_tablet_v2 family** (generated protocol code
  vendored, zero new build deps): manager bound when offered
  (absent = no tablet events, never an error), the per-seat tablet
  seat, the full tool event stream with the protocol's frame
  batching absorbed (one compositor frame = one FDK event).
  Tablets and pads are protocol-tracked but inert — no fdk_event
  carries pad state (the D-030 honest gap).
- **X11: the valuator path through dlopen'd libXi** (the touch
  lane's load discipline): the XIQueryDevice scan finds slaves
  with a pressure axis (label-matched, positional fallbacks), and
  motion/button events from those sourceids translate with their
  valuator state (button 1 = the driver-mapped TIP -> DOWN/UP,
  buttons 2+ -> BUTTON, pressure through the 65535 clamp). Core
  pointer events are untouched — a stylus-as-pointer moves the
  cursor AND streams tablet events, X11's own model.
- **The one honest platform gap, documented**: XI2 has no
  proximity events for clients, so the X11 half synthesizes
  PROXIMITY_IN at a session's first event and delivers no OUT —
  the tool goes silent. DECISIONS D-030 records it.
- **Tests**: both suites drive the REAL translation bodies through
  the seams (sway headless binds the manager but wlroots has no
  virtual-tablet protocol; Xvfb has no tablet device and XTEST
  cannot fake valuator state) — the full event alphabet, the frame
  batching, distinct simultaneous tool ids, out-of-proximity noise
  dropped, the pressure normalization shapes — plus the live
  halves (the sway bind + tablet seat survive; the X11 device scan
  runs against a real server). Real hardware: implemented — should
  work (untested on real hardware).

### Added — subsurfaces: the FDK-owned child-surface primitive, both backends (D-2 / B-16; extension API v2)

- **`include/fdk/fdk_extend.h` (7 new functions, `FDK_EXT_API_VERSION`
  1 -> 2)**: `fdk_ext_subsurface_create` (parent = an application
  `fdk_window*`) and `fdk_ext_subsurface_create_ex` (parent = any
  extension surface — a shell-surface bar carrying an inset video
  region, an overlay on a notification card), plus
  `fdk_ext_subsurface_set_position`, `fdk_ext_subsurface_resize`,
  `fdk_ext_subsurface_set_desync`, `fdk_ext_subsurface_place_above`,
  `fdk_ext_subsurface_place_below`. The returned handle is a normal
  `fdk_ext_surface`: renders through the public `fdk_surface` path,
  receives the translated event stream, parents popups (X11),
  answers `fdk_ext_surface_native`. 855 exports, +7 (verified).
- **X11: real child-window compositing** — a child Window of the
  parent's X window (server-clipped, stacked within the parent, the
  standard FDK event mask + XI2 touch routing + XDND awareness at
  create, created UNMAPPED with the themed background and NorthWest
  bit gravity), positioned in parent PHYSICAL coordinates (logical x
  conn->scale — the HiDPI rule). Moves are server-side immediate
  (XMoveWindow/XResizeWindow + flush); stacking is the core
  protocol's sibling restack (XConfigureWindow with
  CWSibling/CWStackMode); the commit-mode vocabulary is the
  documented no-op hint. Popups anchor to subsurfaces naturally.
- **Wayland: the wl_subsurface role, DESYNC by default** — a fresh
  wl_surface given the role against the parent's surface, the
  output-enter/leave scale tracking + fractional-scale preference an
  adopted surface gets, `configured` from birth (no handshake exists
  to wait for), independent frame updates (the feature's point:
  buffer commits apply without the parent presenting). The GPU
  present lane (D-1) engages for large children like for any
  surface. Popups on subsurfaces are refused loudly and early
  (`FDK_ERR_UNSUPPORTED`) — xdg_popup cannot anchor to a
  wl_subsurface; anchor menus on the parent window.
- **The parent-managed tree-state rule, absorbed** (found live
  against sway 1.10 — the first rig run's lesson, DECISIONS D-029):
  a subsurface's position, stacking, and very PRESENCE in the
  parent's surface tree are parent-applied state per the core
  protocol, regardless of the child's sync/desync mode. A desync
  child of a quiet parent is invisible and unhittable — sway never
  answers its frame callback. FDK's create/set_position/place each
  issue an empty attach-less commit on the topmost CONFIGURED
  ancestor (pure state application, invisible to the app's frame
  pacing; an ancestor mid-handshake is left to its own commit), so
  moves are immediate exactly like X11's. The API contract documents
  the rule instead of hiding it.
- **Keyboard stays with the parent's focus, pointer goes to the
  child** (both backends, by construction): a subsurface is an input
  region, not a focus target — clicks in the region belong to the
  region (child-local coordinates), keys keep flowing to the
  toplevel. The size is client-owned (no configure handshake);
  `resize` dispatches the documented CONFIGURE event on both
  backends. Nesting is legal on both (the sweep is depth-first);
  destroy discipline is the popup contract verbatim (children must
  not outlive the parent — the parent's destroy sweeps them).
- **Tests**: the X11 suite's `test_ext_subsurface_x11` (the server's
  window tree through a second Xlib connection — real child both
  XQueryTree directions, geometry/position, over-parent pixel
  readback, move/resize + CONFIGURE, sibling restack both ways
  through the server's stack order, REAL XTEST click routed to the
  child with child-local coordinates, popup-on-sub, solo destroy +
  recursive nested sweep, parent sweep behind the counting
  protocol-error fence) and the sway suite's subsurface section
  (injected clicks routed to the child, move/resize proven by
  hit-testing the new geometry, the place_below stacking flip by
  which surface takes the click, the popup refusal, nested create,
  parent sweep protocol-clean, grim+PIL pixel readback of the desync
  compositing). Zero new build dependencies (wl_subcompositor is
  core Wayland; the X11 child windows are core Xlib).

### Added — the Wayland GPU present lane, EGL (D-1 / B-15): FDK_RENDER_PATH_GL on both backends

- **`src/platform/wayland/wayland_egl.c` + `wayland_egl.h`** (new):
  the GLX lane's exact hybrid contract carried to Wayland — CPU
  rasterization unchanged (the same rasterizer, the same bytes),
  the lane replaces only present()'s "pixels -> compositor" half:
  the damaged rects upload into a per-window GL texture
  (BGRA/UNSIGNED_BYTE — the staging buffer's little-endian XRGB8888
  bytes read in place), one fullscreen NEAREST quad composites the
  frame, `eglSwapBuffers` publishes it through a `wl_egl_window`-
  backed EGL surface. **`fdk_window_get_render_path` answers
  `FDK_RENDER_PATH_GL` on Wayland now** — the public API is
  unchanged (the lane rides the existing symbol; 848 exports,
  verified) and the X11/GLX lane is untouched by construction.
- **Zero new build dependencies** (the documented baseline —
  libwayland-client + libxkbcommon — is unchanged):
  `dlopen("libEGL.so.1")` + `dlopen("libwayland-egl.so.1")` with a
  hand-declared EGL/GLES2 subset, every entry point through
  `eglGetProcAddress` (THE documented EGL loader path; GLVND does
  not promise dlsym for core GLES symbols). Libraries load lazily
  at the FIRST framebuffer acquisition of ANY window — headless
  apps and no-window apps never touch EGL. Absent libraries are
  the documented fallback (the CPU path), never an error.
- **Policy (mirrors x11_gl.c)**: hardware renderers engage; Mesa's
  software rasterizers (llvmpipe/softpipe/swrast) refuse — the CPU
  path is faster there — unless `FDK_GL_FORCE=1` (what the test rig
  uses under swrast); `FDK_NO_GL=1` opts out entirely; windows at
  or below `FDK_WL_GL_MIN_PIXELS` (the X11 lane's value) stay on
  wl_shm before any dlopen (the panel/popup class never loads the
  GL stack).
- **The alpha decision is correctness, not style**: the staging
  buffer is XRGB8888, so its top byte — the byte a BGRA upload
  reads as ALPHA — is ZERO. The quad's fragment shader PINS alpha
  1.0 and the config prefers an alpha-less EGLConfig (an XRGB8888
  swap buffer, opaque by protocol exactly like the wl_shm path's);
  a passthrough shader would hand the compositor a fully
  transparent window. Caught in pre-merge review — the GLX lane's
  "the X byte rides along" reasoning does not cross to Wayland's
  ARGB compositing.
- **First-frame self-check**: `glReadPixels` of the drawn back
  buffer, probed against the staging buffer
  (RGBA/UNSIGNED_BYTE — the only readback GLES2 guarantees for the
  default framebuffer; the GLX lane's BGRA read is a desktop-GL
  guarantee that does not cross over). A mismatch is a REAL
  failure: the lane refuses for the connection AND the process
  (one warning, logged once — the x11_gl.c verdict discipline),
  and `FDK_GL_FORCE` explicitly overrides the process verdict on a
  fresh connection. `FDK_GL_SELFCHECK_POISON` is the test seam;
  `fdk__wayland_egl_test_reset()` clears the verdicts between test
  groups.
- **Failure = downgrade, never a broken frame**: any lane error
  (GL error, lost context, the `FDK_GL_FAIL_AT` poison) salvages
  the current staging frame into a fresh wl_shm slot, re-presents
  it immediately through the CPU path, and the window stays there
  permanently (`fdk__wayland_surface_downgrade_from_lane` — the
  `fdk__x11_surface_downgrade_from_lane` mirror). A present that
  arrives before the window's first xdg configure rides
  `lane_frame_deferred` and the configure handler flushes it — the
  CPU path's deferred-first-frame contract, mirrored, so a
  single-present application still maps with its first frame.
- **Tests** (`tests/test_wayland_integration.c`, the x11_gl
  group's shape): the policy matrix (FDK_NO_GL strictly CPU; small
  windows CPU by the size gate — deterministic on every machine;
  large default OBSERVED, renderer-driven); present + protocol
  health under FDK_GL_FORCE (two windows on one connection, full
  and partial-damage frames, interleaved presents, connection
  healthy after every one); the downgrade leg (poisoned present #2
  salvaged through wl_shm, permanent, frames keep flowing); the
  self-check real-failure leg (per-connection refusal, one warning
  per connection, the fresh-connection FORCE override, the reset
  seam). Client-side pixel fidelity is pinned by the self-check
  itself; the compositor-side half has no XGetImage equivalent a
  client may call on Wayland, so it is pinned by the rig's
  screenshot smoke (byte-exact red/green/yellow probes through
  grim under sway headless — passed) and the desktop sign-off
  recipe in TESTING-ON-DESKTOP.md.
- **Docs**: docs/rendering.md's GPU-lane section now covers both
  backends; docs/dependencies.md's runtime-optional table gains
  libEGL.so.1 + libwayland-egl.so.1; fdk_window.h's render-path
  doc and README updated. Real hardware GPUs: implemented — should
  work (untested on real hardware; the rig exercises the lane
  through Mesa's swrast path under FDK_GL_FORCE).

## [Unreleased-candidate features] — 2026-10-29 (on the rc2 line)

### Fixed — an add-on passive-grab test: the XTEST key count raced the server's read order

- `xtest_super_digit` injected the Super+digit keys through one
  connection and counted the passive grab's KeyPress deliveries on
  the fake WM's connection after a single `XSync` — but that round
  trip only orders the WATCHER's own request stream; the server
  reads its sockets in its own order, and on a slow container the
  XTEST requests can be processed microseconds after the count
  already ran and read zero (the debian CI container lost exactly
  that race — a run-to-run flake, green the round before). The count
  now polls, bounded to a second and exiting the moment the press
  arrives; a healthy grab fires in microseconds, so the retry budget
  is three orders of magnitude past any real latency.

### Fixed — the void install-test's hermetic set: libXrandr was missing from the clean container

- The last red job of the repair series: five of six green, and
  void's clean-container install-test died at
  `fdk-theme: error while loading shared libraries: libXrandr.so.2`.
  The hermetic pre-install set (libX11, libXext, zlib, libxkbcommon,
  wayland — the 0d9d4de lesson: xbps resolves declared
  shlib-requires only against installed providers) predated M-6;
  `libXrandr` joins it, in FDK's own install-tests.
  The other families resolve the new dependency through their
  package managers (the deb Depends row, rpm's auto-requires,
  pacman's depends=) and passed.

### Fixed — the X11 test link: the XShape readback legs need -lXext on the container path

- The ubuntu job went green with the bus-present fixes, which let
  the four distro containers get far enough to hit the next layer:
  the X11 test suite's S-4 input-region legs call
  `XShapeGetRectangles` (shape.h — a libXext symbol), and the test
  link line carried no `-lXext`. The sibling-tree default FDK_LIBS
  includes it for the static `libfdk.a`, which masked the gap on
  the development sandbox and the ubuntu runner — but the container
  pipelines link against the INSTALLED `libfdk.so`, whose .pc hands
  out `-lfdk` alone by design ("a dynamically linked application
  needs NONE of them"), and there the linker's verdict was the
  honest one: `DSO missing from command line`. The dependency is
  the test code's OWN, so it now rides X11_TEST_DEPS (xtst + xext,
  with the same fallback spelling) — the line whose comment exists
  exactly to disclose each test-link dependency.

### Fixed — the bus-present host: the pump, three test races, and the font ladder (the CI runners' session bus)

- **`fdk_pump_events` no longer returns early on FDK's own D-Bus
  traffic.** The native client's sockets ride the extension fd-source
  machinery, and a fired source counted as pump activity — so on a
  host with a session bus (every CI runner; the development sandbox
  has none, which is why the local battery was green while CI was
  not), one round of AT-SPI bridge handshake returned the pump out
  from under an X event still in flight. Sources now split:
  application add-on sources keep the counted contract
  (fdk_extend.h), while FDK-internal ones (the D-Bus client,
  `fdk__extfd_add_internal`) run their callbacks and are otherwise
  the pump's keep-waiting class — the same rule the backend's own
  MIT-SHM completions already follow.
- **Three test races only a bus-present host could lose.** The ext
  `notify_configure` leg asserted the configure was the LAST event
  on the surface (a trailing Expose from the map legitimately
  follows it once bus traffic shifts delivery across pump
  boundaries; the assert is now ordering-safe — the configure WAS
  delivered, with the reported size). The animated-Picture GUI leg
  relied on its one pre-loop paint landing after the first frame
  tick, instead of following the documented `pump; paint;` cadence
  for timer-driven damage on an application window (the loop now
  paints when the pump reports fired timers — deterministic on
  either side of the map-event race). The file-dialog GUI legs drove
  a real FDK dialog window that portal mode never hands out (C-1's
  contract: OK with a NULL out-handle; the legs now pin
  `$FDK_FILE_DIALOG_PORTAL=0` — the portal path's own coverage is
  the mock-portal battery in `make test-dbus`).
- **test_a11y's font ladder missed three distro layouts.** The C-2
  extents legs load a system font by path; the candidates list had
  Debian's `truetype/dejavu` and liberation only, so Fedora
  (flat `dejavu/`), Alpine (flat `dejavu/`) and Arch (`TTF/`)
  ran the geometry legs fontless and failed. The ladder is now the
  calendar suite's five-path list, and the extents seam skips
  honestly (with a reason) when no system font exists anywhere.
- Verification: the ubuntu and debian failures both reproduce under
  `dbus-run-session` locally and now pass; two consecutive
  dbus-mode `make test-x11` runs green; the full battery (make test,
  FDK_DBUS_TEST_REQUIRE=1 test-dbus ALL PASS, test-x11, sanitize-0,
  release + examples + verify-exports 848, the sway suite)
  re-ran green after the changes.

### Fixed — the RENDER cursor lane: the build dep that wasn't, and the wire value it hid (S-5's two defects)

- **The CI-red streak ends (15 consecutive pushes).** S-5 added
  `#include <X11/extensions/Xrender.h>` to `src/platform/x11/x11_platform.h`
  — a header no CI runner and no packaging container installs
  (libXrender is dlopen'd; the dev package was never a declared
  dependency, because the lane's own design says it must not be).
  Every job in every run since has failed at the same compile line.
  The include is gone: the three wire types the lane needs are
  spelled in the platform header (`typedef XID Picture` — Xrender.h's
  own — plus the opaque `void*` handles, the x11_xi2.c posture), and
  `x11_cursor.c` includes `X11/Xutil.h` directly for the image calls
  the removed header used to carry transitively. The build now
  honors S-5's own claim: dlopen'd RENDER, no new build or CI
  dependency.
- **The bug the include was hiding — `PictStandardARGB32` was
  spelled 4.** The RENDER protocol's enum is ARGB32=0, RGB24=1, A8=2,
  A4=3, A1=4; 4 is A1. Every theme-first named cursor was built from
  a depth-1 format against a depth-32 pixmap — an async BadMatch on
  every `XRenderCreatePicture`, then garbage pictures and garbage
  cursors cached as real ones. The hermetic X11 legs could not see
  it: Xlib generates XIDs client-side, so the non-None asserts passed
  on the phantom handles. A probe against Xvfb pins the pair
  (find_std(0) answers a depth-32 format; find_std(4) a depth-1 one
  plus BadMatch), and the constant is now 0 with the reference
  values in the comment. On a real desktop this was "themed cursors
  are black silhouettes or the plain arrow" — invisible to any
  protocol-level test, exactly the class of defect the project's
  spell-the-wire-values discipline exists to prevent.
- **The regression fence**: the named-cursor X11 leg now runs behind
  a counting Xlib error handler with an XSync at the end — the
  server's verdict is part of the test. With the A1 value the fence
  fails on ~20 async errors; with the fix, zero. Verified both ways
  before landing.
- **M-6's undeclared link dependency: `libXrandr` everywhere.** The
  RandR output model links `-lXrandr` directly (a real build dep,
  unlike the dlopen'd lanes), but no CI environment declared it: the
  ubuntu runner's apt list, and the alpine/debian/fedora/void
  containers' install sets all missed the dev package (arch had it
  transitively). The deb family additionally had a hard failure
  waiting past the compile: the NEEDED-soname map in
  `build-deb.sh` has no `libXrandr.so.2` row, and its catch-all is a
  deliberate BUILD FAIL. Declared now: ci.yml, all five packaging
  scripts, APKBUILD makedepends, PKGBUILD depends, rpm
  BuildRequires, void template, the two deb soname maps, and the
  packaging metadata in full. docs/build.md and docs/packaging.md carry the
  requirement; docs/dependencies.md gained the missing libXext/
  libXrandr rows and the 1.0.0 dlopen-lane table, and its stale
  no-bus section is reconciled to the shipped native-client posture
  (never any D-Bus client library — the wire protocol is spoken
  in-house).

### Added — the AT-SPI Text interface + relations export (C-2 / B-10, level 2)

- **Screen readers can read and drive text.** The accessibility
  bridge now serves `org.freedesktop.atspi.Text` on every
  text-bearing widget (Entry, TextView, Label, SpinButton): the full
  method surface — text content and runs at character/word/line
  granularity, the caret (read AND set), the selection, per-character
  extents in desktop coordinates, and the point hit-test (the same
  walk a mouse click runs).
- **AT-SPI offsets are codepoints; FDK's are bytes** — every method
  converts both ways, pinned by the multibyte rig leg ("héllo"
  answers 5 characters over a 6-byte buffer).
- **The text signals**: `TextCaretMoved` and `TextSelectionChanged`
  fire on every caret/selection move (whatever moved it — keys,
  mouse, IME, programmatic, the screen reader itself); `TextChanged`
  fires on edits.
- **`GetRelationSet`**: the internal relations export to AT-SPI's
  relation numbers (pinned against at-spi2-core's own constants) —
  a Label's `LABEL_FOR` an Entry, discoverable from either side.
- One internal notification kind appended to the public a11y event
  enum (`FDK_A11Y_CARET_MOVED`); no other public API change (848
  exports). Tests: 3 new live-bus legs + 2 headless legs.
  docs/DECISIONS.md D-027.

### Added — color glyphs: emoji in every family the desktop meets (C-3 / B-02)

- **Color emoji render** in `fdk_surface_draw_utf8` and every widget
  that draws text: a glyph the FACE marks as color (COLR table entry,
  CBDT bitmap strike, or sbix strike) rasterizes as a straight-RGBA8
  bitmap carrying its own colors instead of a tinted alpha mask.
  Three wire families, all parsed natively from the public OpenType
  specifications: **COLRv0 + CPAL** (layer lists — Twemoji-COLR and
  the older Noto builds), **COLRv1 + CPAL** (the full paint graph:
  solid/linear/radial/sweep fills, every transform flavor including
  rotate/skew, the 28 W3C blend modes, PaintColrGlyph reuse, clip
  boxes), and **CBDT/CBLC + sbix** (bitmap strikes — PNG payloads
  decoded through the same stb_image path the clipboard rides,
  log-ratio strike selection, box-filter downscale, bilinear
  upscale).
- **`fonts-noto-color-emoji` loads and draws.** That font — the
  default emoji font across the Linux desktop — has no outline
  tables at all (10.5 MB of PNG strikes, no glyf), which the font
  loader previously rejected outright. The loader now recognizes the
  bitmap-only posture, hand-fills the glyph/cmap/metrics machinery,
  and serves every glyph from its strikes.
- **Emoji fallback routing**: the shaping pipeline's script table
  grows the emoji bucket (Extended_Pictographic ranges, fontconfig
  `und-zsye` + a charset probe), so an emoji in a plain text run
  pulls the emoji font the way CJK pulls a CJK font. Palette index
  0xFFFF (foreground) renders opaque black; variable color fonts
  render at the default instance.
- **`$FDK_NO_COLOR_GLYPHS=1`** disables the color path for outline
  faces (the escape hatch: the reference outline draws mono, tinted
  with the run color). Bitmap-only faces ignore it — their strikes
  are the only ink there is.
- Public API: **unchanged** (848 exports) — the feature is internal
  to the text pipeline by design. Tests: `tests/test_emoji.c` against
  fontTools-generated fixtures (the reference implementation wrote
  every wire byte the C parser reads), plus a smoke leg against the
  real Noto Color Emoji. docs/DECISIONS.md D-026.

### Added — basic gestures: pinch + swipe (C-6 / B-17)

- **`FDK_EVENT_GESTURE_PINCH`** (fdk_event.h): while exactly two
  touch slots are active, every span change emits a pinch with
  `scale` (the cumulative fingers-spread ratio since the two-finger
  posture began — 1.0 at its start), `delta` (the incremental ratio
  since the previous event), and `focus` (the centroid). Pinch
  coexists with the two-finger scroll — they read different geometry
  (span vs centroid), exactly GTK's posture. A finger swap
  mid-gesture re-baselines the scale.
- **`FDK_EVENT_GESTURE_SWIPE`**: a one-shot verdict when the last
  finger of an N>=2 gesture lifts — every lift within
  `FDK_GESTURE_SWIPE_MS` of the first, EVERY finger's dominant-axis
  travel beyond `FDK_GESTURE_SWIPE_MIN_PX` (the per-finger minimum:
  two lazy fingers do not sum into a swipe), opposing travels
  rejecting; `fingers` carries the gesture's peak membership and
  `direction` the compass (FDK_SWIPE_LEFT/RIGHT/UP/DOWN). A fast
  fling whose span also changed has already fired its pinches — both
  firing is by design.
- The recognition lives in the PURE core touch engine (identical on
  both backends by construction; no backend code moved — wl_touch
  and XI2 already deliver the raw slots). Tests: six engine legs
  (span arithmetic + re-baseline numbers, both swipe directions, the
  four rejects, the CANCEL wipe); the classic tap/scroll tests
  re-asserted type-filtered (the pinch coexistence).
  docs/DECISIONS.md D-025.

### Added — the shell add-on's runtime re-anchoring + auto-hide (C-5 / A-05 / A-08)

- **The layer's placement is LIVE** (add-on 0.5.0):
  the live setters — anchor / margin / exclusive-zone /
  desired-size / keyboard-mode / level —
  change a SHOWN surface without destroying it — Wayland re-issues
  the double-buffered zwlr requests and commits (the surface is
  never destroyed; the next configure confirms); X11 recomputes the
  anchoring math and moves/resizes the live window with its size
  hints and struts (a zone change re-reserves immediately).
  `set_level` is the role swap on Wayland (destroy + re-create over
  the same `wl_surface`, one unconfigure/configure cycle) and the
  `_NET_WM_STATE` exchange on X11.
- **Auto-hide, the reveal-strip model**:
  `set_auto_hide(reveal_px, dwell_ms)` parks the
  layer off its primary anchored edge leaving `reveal_px` on screen
  and WITHDRAWS the exclusive zone (other windows take the space);
  the pointer entering the strip (or the layer) reveals, leaving
  arms a dwell timer whose expiry hides; `reveal` /
  `hide_now` drive the states from application code. The slide is
  backend-native: a position offset on X11 (margins only push
  inward), a negative margin on Wayland (zwlr margins are signed).
- Tests: 9 new X11 legs (re-anchor geometry/strut readback; the
  hidden slide at y=-37 with the size kept and the strut withdrawn;
  reveal; a REAL XTEST motion into the 3px strip revealing; leave +
  dwell hiding; disarm) + 4 new sway legs (configure-through for
  margin/zone/size; the anchor flip; the role swap; the auto-hide
  cycle). The rig caught two real defects: the role-swap WIRE ORDER
  (the new role marshalled before the old destroy is a fatal
  "Cannot reassign role" error) and the margin-trap (margins cannot
  push a window off-screen on X11 — the slide became a native
  offset). docs/DECISIONS.md D-024.

### Added — the AT-SPI2 accessibility bridge, level 1 (S-8 / B-10)

- **OS screen readers can finally see FDK apps.** The internal a11y
  tree (rich since 1.4.x — roles, states, actions, virtual children)
  now has its WIRE: an AT-SPI2 bridge on the native D-Bus client
  that serves the whole widget tree on the accessibility bus and
  emits the events screen readers listen for. Orca, at-spi2 based
  automation, and accessibility stacks enumerate FDK windows like any
  other application's.
- The full level-1 surface: `Accessible` (AtspiRole-numbered roles,
  the 64-bit state set, tree enumeration with virtual children —
  list ROWS are addressable objects), `Component` (extents/position/
  size in desktop coordinates), `Action` (`DoAction(0)` is the
  screen reader's "click" — the same programmatic path a keyboard
  activation takes), and the `Name`/`Description`/`Parent`
  properties. Events: `Focus` + `StateChanged` on focus moves,
  `PropertyChange`/`ChildrenChanged` for tree edits.
- The two-bus handshake with a RETRY posture (a screen reader
  toggled on mid-session still finds running apps; failures retry on
  a 2 s throttle, never degrade permanently). `$FDK_ATSPI=0`
  disables; discovery rides the announcement signal the at-spi2
  registry itself uses.
- Tests: five live-bus legs against a mock AT-SPI stack (the mock
  org.a11y.Bus, the discovery, the tree walk, the properties/state/
  extents, the focus event, and the action round trip). The rig
  caught the premature-degradation bug (the bridge gave up before
  the mock owned the name) — the retry posture is the fix, and it is
  the right behavior for real desktops too. 848 exports (the tree
  was already public; the bridge is internal). docs/DECISIONS.md
  D-023; owner steps TESTING-ON-DESKTOP.md §8.

### Added — the xdg-desktop-portal FileChooser (C-1 / B-09)

- **The desktop's own file picker, when the desktop has one**:
  `fdk_dialog_open_file` / `fdk_dialog_save_file` now consult
  `org.freedesktop.portal.FileChooser` on the session bus FIRST and
  fall back to the toolkit's own browser on ANY failure (no bus, no
  portal, refused call, timeout) — the application always gets a
  picker; on a portal desktop it gets the system-styled,
  sandbox-aware, recents-integrated one. `$FDK_FILE_DIALOG_PORTAL=0`
  disables; `=1` forces (a missing portal then fails honestly —
  `FDK_ERR_PLATFORM` / the ERROR outcome — instead of falling back).
- The full kind contract rides the call: directory/multiple from the
  dialog kind, `current_folder` (the spec's byte-array form) from
  start_dir, `current_name` for SAVE, the filters glob list as
  portal `(sa(us))` filters, the title verbatim. The spec's
  race-free ordering throughout: handle_token + the predictable
  request path, the Response subscription armed before the call.
- The result model is unchanged — the same callback, the same
  `fdk_file_dialog_result` with portal URIs percent-decoded to POSIX
  paths. Response 0 = ACCEPTED, 1 = CANCELLED, a portal refusal in
  forced mode = ERROR.
- Tests: five live-bus legs against a mock FileChooser portal (the
  call-shape asserts, the decoded paths, the dismissed response, the
  folder kind, the auto-mode refusal -> the own browser's real
  window, the forced-mode refusal -> ERROR). Two real defects the
  rig caught and closed: the fallback's re-entry would start a NEW
  portal flight whose state reset freed the strings the fallback was
  reading (a UAF — now guarded), and the modal parent is nulled at
  fallback time (it may be destroyed by then). 847 -> 848 exports.
  docs/DECISIONS.md D-022; owner steps TESTING-ON-DESKTOP.md §7.

### Added — drag icons + widget-level drop targets (S-9 / B-13)

- **`fdk_drag_begin_ex`** (fdk_dnd.h): `fdk_drag_begin` with a drag
  ICON — width x height ARGB8888 pixels (the `fdk_window_set_icon`
  packing), clamped to 256, riding the drag itself. Wayland: the
  protocol's own icon surface in `wl_data_device.start_drag`
  (compositor-positioned, alpha-blended). X11, where XDND has no
  icon protocol: a small override-redirect window tracking the
  pointer at the GTK hotspot offset for the drag's life, shaped by
  the icon's alpha channel so transparent icons are not black boxes.
- **`fdk_widget_set_drop_target` + `fdk_widget_get_drop_target` +
  `fdk_widget_set_drag_callback`** (fdk_dnd.h): widget-level drop
  targets. The window's drag routing hit-tests the tree and walks up
  from the deepest hit widget — the first registered target owns the
  position (a registered CONTAINER receives drops anywhere in its
  subtree); the scoped callback sees ENTER / MOTION / LEAVE / DROP
  with synthesized boundary transitions. The window-level callback
  still sees every drag event (additive, not a replacement).
- **Per-position acceptance on both backends**: the XDND Status
  reply and the `wl_data_offer.accept` refresh re-evaluate at every
  position, so the source's cursor feedback reflects the real answer
  under the pointer (GTK's behavior). Windows that never register
  widget targets behave exactly as before.
- Fixed (found by the S-9 rigs): SAME-CONNECTION drags — an app
  dragging between its own windows — now work on Wayland (the drop
  read serves the dragging client's own `::send` through the
  canonical dispatch, with a fence against mid-read teardown);
  every Wayland drop read used to sit out its full 250 ms budget
  because our own pipe write-end made EOF impossible (closed at the
  receive request now); the X11 DRAG_DROP event carried (0,0) as its
  position since v1 (now the last XdndPosition's coordinates).
- Tests: 7 new X11 legs (the icon window's existence, hotspot
  offset, alpha-derived shape, tracking, and destruction through a
  real drag; the zone's scoped delivery; the fallback region's
  acceptance; boundary transitions) + 2 new sway legs (the
  per-position zone proof; the FDK-source drag with icon end to
  end). 843 -> 847 exports. docs/DECISIONS.md D-021; owner steps
  TESTING-ON-DESKTOP.md §6.

### Added — window activation + startup notification (S-6 / B-04)

- **`fdk_window_activate`** (fdk_window.h): ask the compositor/WM to
  bring a window to the user — focus, raise, unminimize as the
  platform allows (GTK's "present" verb). Wayland: xdg-activation-v1
  through a compositor-minted token (or the launch voucher below);
  X11: the EWMH `_NET_ACTIVE_WINDOW` message carrying the last USER
  event timestamp (tracked live in the dispatch pump — the number a
  WM weighs for focus-stealing prevention), with an honest raise +
  focus fallback under bare X. A request, not a guarantee — by
  protocol, not policy.
- **`fdk_window_activation_token`** (fdk_window.h): the LAUNCHER
  half — mint a token tied to this window and hand it to a spawned
  child through its environment (`XDG_ACTIVATION_TOKEN` on Wayland,
  `DESKTOP_STARTUP_ID` on X11) so the child's first window arrives
  pre-vouched. The Wayland mint rides a PRIVATE event queue +
  `wl_display_roundtrip_queue` (reentrancy-safe from inside any
  event handler); the X11 mint is a spec-format startup-notification
  id plus the chunked "new:" state on the root window — the full SN
  feedback loop CLIENT-SIDE, no D-Bus, no library.
- The launched side needs no code: both variables are captured once
  at `fdk_init` and unset immediately (the protocols' hygiene rule —
  grandchildren never inherit a stale token); X11 windows created
  under the id carry `_NET_STARTUP_ID`, and the first show emits the
  matching "remove:" state (the launch feedback's honest end, once
  per launch).
- Tests: six new X11 legs (capture discipline, property round-trip,
  chunked-wire reassembly, unique re-mints, the one-shot remove:,
  the bare-X XGetInputFocus proof) + the fake-WM
  `_NET_ACTIVE_WINDOW` legs with a real XTEST click's timestamp; five
  new sway legs (capture-unset, the private-queue mint, the
  injected-click focus state, the bogus voucher REFUSED — focus-
  stealing prevention IS the assertion, and the full two-process
  launcher flow: held-button serial, mint, re-exec'd child, sway
  flips focus through the voucher). 841 -> 843 exports.
  docs/DECISIONS.md D-020; owner steps TESTING-ON-DESKTOP.md §5.

### Added — animated GIF images (C-4 / B-14)

- **`fdk_gif`** (fdk_surface.h): a spec-native animated GIF loader
  — the full GIF87a/89a parser, the LZW variant (code sizes 3..12,
  clear/EOI, the KwKwK case), per-frame color tables, transparency,
  the disposal methods, interlacing, and the NETSCAPE loop count,
  all bounded per docs/security.md (canvas 1..16384, <= 4096
  frames, LZW output capped at exactly w*h). PIL save_all
  embedded-stream quirk (a second GIF89a header mid-file, no
  trailer) is resynced like browsers do. 825 -> 841 exports.
- **`fdk_gif_player`**: one-frame-at-a-time compositing onto an
  app-owned ARGB8888 surface — the previous frame disposal applied
  first (method 2 clears to TRANSPARENT, the interoperable reading;
  method 3 restores the saved canvas). The clock is the app own: a
  timer, a game loop, anything.
- **`fdk_picture_set_animated_file`**: the zero-code consumer — a
  repeating timer re-paced to each frame own delay (with the
  100 ms compatibility floor for 0/<20 ms values), invalidation-
  driven repaints, `fdk_picture_set_playing` to pause on the
  current frame, static sets dropping the animation and its clock.
  Detached trees show frame 0 and never tick (the Spinner rule).
- Tests: tests/test_gif.c — the disposal matrix against PIL REAL
  encoder output (keep / clear-to-transparent / restore-to-
  previous, loop wrap, reset), the interlace four-pass pin, exact
  delays + the floor, the loader error matrix, and the Picture
  headless half; the X11 GUI leg proves the timer drives real
  repaints through a live window and pausing freezes them.
  Fixtures + their generators: tests/test_gif_fixtures.h,
  tests/data/gifgen/.

### Added — the calendar and the date/time pickers (S-10 / B-18)

- **`fdk_calendar`**: the month grid (the GtkCalendar counterpart) —
  month/year header with chevron navigation from the locale names
  table (15 languages, English fallback), a Monday-lead weekday row,
  six week rows with adjacent-month days in the disabled color,
  selection highlight, an app-injected "today" accent underline, and
  full keyboard walking (arrows cross month edges, PageUp/PageDown
  step months with day clamping, Home jumps to today, End to the
  month's last day). No clock reads — see docs/DECISIONS.md D-018.
- **`fdk_date_picker`**: a formatted field (fdk_format_date, locale
  + style held in the widget) that opens a POPUP calendar at the
  field's edge (flipping above when the owner clips it); picking a
  day applies, fires on_changed once, and closes; Esc and outside
  click dismiss unchanged; destroying the picker while open is
  guarded (the combo's discipline). Starts DATELESS (an em dash).
- **`fdk_time_picker`**: the segmented stepper — [HH] : [MM]
  [: SS] [AM/PM], one active segment (click or Left/Right), chevron
  zones and Up/Down stepping with wraparound (12h hours walk
  1..12 keeping the daypart; AM/PM toggles), PageUp/PageDown step
  minutes/seconds by ten and hours by six. Seconds arrive with
  FDK_TIME_MEDIUM; the 12/24-hour cycle follows the locale unless
  forced.
- New a11y roles CALENDAR and DATE_EDITOR (append-only); the
  calendar's 42 day cells enumerate as virtual children with
  ACTIVATE performing the selection; the pickers report the value
  interface (day-number space / seconds-of-day) with formatted
  value_text. 804 -> 825 exports.
- Tests: tests/test_calendar.c (model validation, the grid math
  pinned through the a11y virtual children across normal/leap/
  edge months, click + keyboard semantics, pixel pins for the
  selection blend and today underline) + the X11 GUI leg (popup
  lifecycle, day pick, Esc dismissal, the destroyed-while-open
  guard through a REAL XTEST outside click) + the sway leg (REAL
  seat clicks opening the popup and picking the day through the
  compositor, the orphan-popup dismissal).
- Level-1 fences named in fdk_widgets.h: no typed-text parsing
  (no parse API exists), no week numbers, no time zones (the i18n
  module has none, deliberately).

### Added — input regions: partial click-through (S-4 / A-06 / A-07)

- **`fdk_window_set_input_region`** and
  **`fdk_ext_surface_set_input_region`**: pointer input restricted
  to up to 64 window-local logical rectangles; clicks outside the
  region fall through to whatever is beneath. NULL/0 clears (full
  input); a zero-area rect is the empty region (fully
  click-through). 802 -> 804 exports.
- X11: XShape input rects (logical scaled to physical); the clear
  sets a full-window rect (libXext has no NULL-region idiom).
- Wayland: wl_surface.set_input_region — surface-local is already
  the logical space; committed immediately when configured, riding
  the first commit otherwise.
- Proven end to end: XShape protocol readback + REAL XTEST
  click-through both ways (X11); the injector pointer's
  hole/region/leave/clear routing (sway); the add-on twin through
  its own suite. The substrate C-5's auto-hide reveal strips
  build on.

### Added — the full named cursor set (S-5 / B-12)

- **`fdk_cursor_shape` grows from 2 to 22 shapes** (append-only;
  values 0 and 1 unchanged): TEXT, CROSSHAIR, WAIT, PROGRESS, MOVE,
  COPY, ALIAS, NO_DROP, NOT_ALLOWED, GRAB, GRABBING, ALL_SCROLL,
  COL_RESIZE, ROW_RESIZE, ZOOM_IN, ZOOM_OUT, HELP, CELL,
  CONTEXT_MENU, VERTICAL_TEXT — the cursor-shape-v1 semantic space,
  the names CSS/GTK4/Qt6 use. No new symbols (the set rides the
  existing `fdk_window_set_cursor`); 802 exports unchanged.
- **A widget hover layer**: without an application request, hovering
  an Entry / TextView / Spin (or an editable Combo's entry) shows
  the I-beam automatically; an explicit app request still owns the
  whole window; the resize compass stays last.
- **Wayland**: cursor-shape-v1 when the compositor offers it (the
  sway suite pins the whole enum table on the wire); the hand-parsed
  XCursor theme fallback otherwise — now in the shared
  src/platform/cursor_theme.c both backends load through.
- **X11**: theme images through the dlopen'd libXrender.so.1 RENDER
  lane (no new build dependency) for the shapes the core cursor font
  lacks, the nearest core glyph otherwise, the documented
  substitution table in docs/platform-input.md; a cursor the
  platform cannot draw honestly keeps the default arrow.
- **The shell add-on (0.4.0)** mirrors the set for
  panels (TEXT over a search entry, GRAB over drag handles, WAIT
  while app lists load).

### Added — X11 multi-monitor: the RandR output model (M-6 / B-05)

- **The core's internal table** (`src/platform/x11/x11_randr.c`):
  per-CRTC geometry probed at connect (reading-order sorted),
  RRNotify-masked on the root, refreshed in the dispatch loop. The
  bare-X maximize/fullscreen fallbacks now fill the CONTAINING
  monitor (the window's center) instead of the whole screen. No
  public API — the Wayland core's no-output-API parity (D-015).
- **The shell add-on's own X connection** (its event channel, the
  X11 twin of its own wl_registry): XRRSelectInput + the extension
  loop's fd source; the output table with real connector names,
  per-CRTC geometry and mm sizes; ADDED/CHANGED/REMOVED events
  with gone-records enumeration (the contract the Wayland side
  defines); per-output layer anchoring — output_index N anchors
  against THAT monitor, offset into root coordinates; a stale index
  degrades to the whole screen, never a crash.
- **The fallback is the pin**: no RandR = one output, the screen,
  real metrics, no events — byte-identical to the pre-M-6 behavior
  (every unchanged single-monitor rig re-proves it). Xvfb's fixed
  single output ("screen") pins the probed table; the multi-output
  origin math runs through an internal test seam (a synthetic
  two-monitor table, window geometry read back through the
  protocol). Struts stay root-relative — the documented EWMH
  per-monitor interpretation limit.

## [Unreleased-candidate features] — 2026-10-29 (on the rc2 line)

### Added — the desktop host models: the T-B side 2 split (add-on 0.3.0)

- **The add-on's desktop header (its public API)**: the
  StatusNotifierWatcher host model and the
  org.freedesktop.Notifications server model — DATA AND EVENTS ONLY. Snapshot APIs
  (struct_size structs with caller buffers), one preorder menu walk,
  and action entry points (Activate / SecondaryActivate / Scroll,
  menu open/close/event, action_invoke / close). Coarse events:
  ITEMS_CHANGED / ITEM_STATE / MENU_CHANGED / HOST_STATE and ADDED /
  REPLACED / CLOSED / SERVER_STATE. The name etiquette (refuse /
  --replace / --wait; the RequestName-EXISTS refusal; dormant after
  refusal — never a queue-lurker) with serving() as the query.
- **The add-on's tray_model.c / notify_model.c**: the
  protocol halves extracted from the reference UIs — the watcher
  service, both registration dialects, per-item property/pixmap/
  tooltip/menu state, the dbusmenu client (two-out-arg GetLayout,
  AboutToShow, NO_REPLY_EXPECTED Events, the stale-safe mid-open
  discard, the load-time prefetch); the Notifications server with
  the flood gate, live-set cap, expiration timers, and the record
  store. The rigs' trace vocabulary moved house with the code (the
  models emit the same lines under FDK_TRAYHOST_TRACE /
  FDK_NOTIFYD_TRACE).
- **`examples/trayhost.c` / `examples/notifyd.c` rewritten as thin
  UIs** over the models — rendering and input only. The UNCHANGED
  rigs re-proved both pairs end to end (notifyd 68, trayhost 103
  checks, all three phases each): the split's
  behavior-preservation proof.
- **The add-on's test_desktop_model.c (its test-desktop target)**: the
  models headlessly on a private
  dbus-run-session bus — an in-test mock item and client; snapshot
  fields byte-exact, the pixmap copy, the menu round, interaction
  forwarding, vanished-item drops, hostile/duplicate registrations,
  the etiquette matrix including dormant-after-vacate, Notify ids/
  replaces/actions/expiry/flood/live-set, and both signals out.

## [Unreleased-candidate features] — 2026-10-28 (on the rc2 line)

### Added — idle/screensaver inhibition (S-3 / B-11)

- **`fdk_idle.h` (public API, append-only)**:
  `fdk_idle_inhibit` / `fdk_idle_uninhibit` /
  `fdk_idle_inhibitor_status` — one inhibitor handle, whatever the
  desktop speaks. Wayland engages the compositor's own
  `zwp_idle_inhibit_manager_v1` (surface-scoped through a new
  window-scoped platform op; the engagement is window-owned — a
  window dying while inhibited releases its inhibitor). Everywhere
  else (X11, Wayland compositors without the protocol) the
  `org.freedesktop.ScreenSaver` D-Bus service carries it:
  `Inhibit(app, reason)` returns a cookie, `UnInhibit(cookie)`
  releases it.
- **The deferred-release lifecycle**: the D-Bus half is async — the
  cookie may arrive after the application already released the
  inhibitor. Cancelling would orphan the inhibition on the desktop
  (the call was already sent), so the release is remembered and the
  matching `UnInhibit` goes out the moment the cookie lands; the
  handle's memory is owned by a three-slot reference count (user
  handle, bus handshake, pending call) so the late reply never
  touches freed memory.
- **Tests**: the live-bus rig's mock
  `org.freedesktop.ScreenSaver` pins the cookie round trip, the
  deferred release (release-before-cookie still un-inhibits — no
  orphan), and the service-refusal degradation; the sway rig pins
  the compositor inhibitor (engage, release, and
  destroy-while-inhibited, all protocol-clean against the real
  wlroots implementation); the X11 rig pins the no-bus fallback
  shape (clean UNSUPPORTED degradation, early-release-clean under
  ASan).
- **Named limit**: GNOME's `org.gnome.ScreenSaver` variant is not
  probed (on GNOME Wayland the compositor protocol path engages
  anyway).

### Added — IME composition, both backends (M-1 / B-01)

- **The IME event family (public API, append-only)**:
  `FDK_EVENT_IME_PREEDIT` / `FDK_EVENT_IME_COMMIT` (event types
  50/51) with the `fdk_ime_event` payload — UTF-8 `text` (owned
  by FDK for the callback duration only, the drag-payload rule)
  and the preedit `cursor` as a UTF-8 byte offset. The same pair
  arrives as widget events (`FDK_WIDGET_IME_PREEDIT/COMMIT`,
  delivered to the focused widget only, like keys): the toolkit's
  Entry and TextView consume both — composition renders inline at
  the caret, commit splices at the caret through the single
  mutation primitive (undo/on_changed/a11y ride it) and replaces
  any selection. Custom text surfaces receive the window-level
  events instead. A single-codepoint commit ALSO arrives as the
  KEY_DOWN codepoint (compose keys keep working); multi-codepoint
  commits arrive only as IME_COMMIT.
- **Wayland**: `zwp_text_input_v3` (optional global — absent
  compositor support means keys still work, no composition). The
  v3 state machine is honored exactly: preedit/commit latch and
  apply at `done` (a commit clears the preedit first); enter/leave
  gate enable/disable; surrounding text (focused text widget's
  text + caret, bytes) is pushed on focus gain and after IME
  routing. Client-side self-heal for the wlroots window-destroy
  gap (focus dropped without `leave` leaves the v3 input enabled —
  and enable-on-enabled is a state no-op, so the IM would never
  re-activate): a stale session is closed before the next enable.
  A window dying mid-composition can no longer leak its batch into
  a freed window (the destroy-path guard).
- **X11**: per-window XICs now request `XIMPreeditCallbacks |
  XIMStatusNothing` first (the old PreeditNothing posture remains
  the fallback when the input method refuses callbacks). The
  composition buffer is maintained by a pure UTF-8 splice
  (XIM's character indices against byte offsets — the translation
  XIM clients historically corrupt; headless-tested including the
  grow-and-retry and hostile-draw resets); commits flow
  Xutf8LookupString with the multi-codepoint case dispatching
  IME_COMMIT and swallowing the key event.
- **Tests**: tests/test_ime.c (the buffer-math battery — the CJK
  and 4-byte-emoji character-index shapes, refusal paths, the
  pinyin-style composition cycle; widget routing — consumption,
  caret splice, selection replace, undoability, read-only/
  disabled/no-focus fall-through; surrounding-text context
  queries). The sway rig drives the REAL protocol both ways: a
  second connection binds `zwp_input_method_manager_v2` (the
  fcitx5 role) and pushes preedit revisions and commits through
  sway's relay — asserted at the window callback (custom-surface
  seam), in the Entry (splice + on_changed), and back at the
  input method (surrounding text + caret). The XIM callback
  wiring itself is owner-verified on a real desktop
  (docs/TESTING-ON-DESKTOP.md §4) — no XIM server exists in the
  sandbox, which the buffer math being pure-and-pinned accounts
  for.

### Added — touch basics: slots, tap clicks, two-finger scroll (M-3 / B-03)

- **The touch event family (public API, append-only)**:
  `FDK_EVENT_TOUCH_DOWN/UP/MOTION/CANCEL` — one slot per contact
  (`fdk_touch_event.id`), positions window-local logical. FDK
  synthesizes on top: a TAP (single slot, within
  `FDK_TOUCH_TAP_MS`/`FDK_TOUCH_TAP_SLOP`) emits a full left click
  pair (widget applications work on touchscreens unchanged), and
  two active slots turn the centroid's motion into
  `FDK_EVENT_POINTER_SCROLL` with natural-scrolling deltas. A
  multi-gesture latch kills the lifted-last-finger phantom click;
  CANCEL wipes every slot without synthesizing anything.
- **Wayland**: wl_touch bound from the seat capabilities; the
  id→window map rides the connection (UP/MOTION carry only ids).
- **X11**: XI2 touch (XI 2.2+) through dlopen'd libXi — the GL
  lane's load discipline: no build/link dependency, absent libXi
  or old XI simply means no touch events ($FDK_NO_XI2 opts out);
  GenericEvent cookies translated through the dispatch loop;
  XI 2.2's emulation rule keeps the synthesized tap the only click
  a touch produces.
- **Tests**: the full policy battery runs headless against the
  engine (tests/test_touch.c — tap/slop/timeout/drag/natural
  deltas/re-arm/phantom-latch/cancel/protocol noise); both
  backends' REAL translate paths are driven through the `test_touch`
  ops seam (the test_key discipline — sway's seat has no touch
  device and Xvfb cannot inject touch); the XI2 module itself
  loads and negotiates 2.2 on the rig's Xvfb.

### Added — window identity: icons, PID, host, role (M-4 / B-04)

- **`fdk_window_set_icon()` (public API)**: a window's icon — the
  image taskbars, docks, and window switchers show for it — as
  width x height ARGB8888 pixels (the toolkit's native `fdk_u32`
  packing, identical bytes to `_NET_WM_ICON`). X11 writes the EWMH
  property (byte-exact round-trip pinned by the integration test);
  Wayland speaks `xdg-toplevel-icon-v1` where the compositor offers
  it (KDE Plasma 6.3+; non-square sources are letterboxed onto the
  protocol's required square buffer), and where it does not the call
  is the documented honest no-op — one INFO line, no error, because
  Wayland offers no icon fallback path. `NULL`/non-positive
  dimensions clear the icon on both backends.
- **`fdk_window_set_role()` (public API)**: the ICCCM
  `WM_WINDOW_ROLE` string (X11; no Wayland protocol exists — the
  documented no-op). Windows now carry create-time identity the
  desktop reads: `_NET_WM_PID`, `WM_CLIENT_MACHINE`, and a default
  role (`main`/`popup`); WM_CLASS was already set from the
  app_id. On Wayland the app_id remains the whole identity story
  by protocol design.
- **Tests**: the X11 integration suite reads every identity
  property back from the server through a second raw-Xlib
  connection (pid equality, hostname equality, role default +
  override + revert, byte-exact icon round-trip including replace
  and clear); the Wayland suite proves set + clear keep the
  connection healthy in both compositor worlds (manager bound or
  absent) and the surface path never trips on icon state.

### Added — CFF outlines: OpenType 'OTTO' faces render (the Arch tofu report)

- **The native Type 2 module (core, `src/text/cff.c`)**: CFF-flavored
  OpenType fonts — Noto Sans CJK, URW base35, most .otf files — now
  load and render instead of being rejected as silent-tofu risks.
  Both dialects: `CFF ` including CID-keyed FDArray/FDSelect, and
  `CFF2` at the default instance. Written from the public Adobe
  specifications with real bounds, an operator budget, depth caps
  and stack limits (D-011; provenance in docs/THIRD-PARTY.md).
- **One rasterizer, both flavors**: CFF geometry reaches the SAME
  `stbtt_Rasterize` call the glyf path uses, as `stbtt_vertex`
  arrays — no look or metric drift between flavors. HarfBuzz glyph
  extents route through the native ink box; advances stay with
  hmtx; `maxp numGlyphs` must equal the CharStrings INDEX count or
  the face is rejected.
- **Hardened load order**: the native parser gates 'OTTO' faces
  BEFORE stb's own CFF initialization runs (stb trusts a fabricated
  512 MiB extent and asserts on malformed bytes — the fuzz battery
  found it live). Fallback candidates may now be either sfnt flavor;
  a loaded fallback that does not cover the segment's probe
  codepoint is discarded before caching.
- **tests/test_cff.c**: dual-decoder vertex agreement against stb's
  own Type 2 path over real URW + Noto faces, a crafted seac/subr/
  cyclic-subr font (real foundry fonts no longer carry seac), loader
  rejection (numGlyphs cross-check, hostile extents, truncation),
  and a deterministic mutation fuzz over the CFF table region.


### Added — complex-script text shaping, level 1 (M-2 / B-02)

- **The shaped pipeline (core, `src/text/shaping.c`)**: text now
  renders through the full complex-script stack when HarfBuzz and
  FriBidi are loadable at run time — dlopen'd, never build
  dependencies, exactly the GLX/fontconfig posture. FriBidi resolves
  the Unicode bidi algorithm (Arabic, Hebrew, Persian, Urdu render in
  VISUAL order instead of backwards; brackets mirror); HarfBuzz
  shapes through the font's real GSUB/GPOS tables (joining forms,
  ligatures, mark attachment with x/y offsets, real kerning) fed by
  custom font funcs backed by stb_truetype itself (26.6 pre-scaled,
  so shaped and legacy advances agree by construction); fontconfig
  supplies a per-script fallback font, charset-filtered on a probe
  codepoint, so CJK next to Latin picks the right font per segment.
  `$FDK_SHAPING` pins the posture (0 legacy, 1 require, unset auto);
  without the libraries the 1.4.x codepoint-at-a-time walk runs with
  IDENTICAL metrics — pinned by a test that re-execs itself under
  the other posture.
- **Every text consumer reads shaped placements**: measure, draw,
  span draws with decorations, markup labels, TextView, wrap,
  ellipsize, and the Entry caret + click hit-test (carets and clicks
  can no longer disagree with the ink). One rounding rule survives:
  the pen floors per glyph, rounds once per reported width.
- **Public surface (+2, 797 exports)**:
  `fdk_text_shaping_available()` (the pipeline's verdict — surface
  it or ignore it, the API is identical either way) and
  `fdk_font_has_glyph()` (coverage probe).
- **tests/test_shaping.c**: RTL visual order, mixed bidi, fallback
  selection, cluster/byte mapping, caret monotonicity, Arabic wrap +
  ellipsize agreement, draw/measure agreement on a trilingual
  string, the fallback lifecycle (dies with its primary, ASan
  clean), and the cross-posture Latin pin. CI runs both postures
  (the ubuntu job's auto probe + an explicit `FDK_SHAPING=0` leg;
  every distro container gains harfbuzz + fribidi + a CJK font).
- **Packaging**: the optional runtimes are declared the `libgl1`
  way — deb `Recommends: libharfbuzz0b, libfribidi0`, rpm
  `Recommends:`, arch `optdepends` — never hard dependencies.

### Fixed

- **The live desktop-bus posture of the TASK B live tests**: both
  `test_notify_client` and `test_tray_item` now follow
  `test_dbus_live.c`'s skip discipline — no display backend, a stale
  bus address, or a REAL notification daemon / StatusNotifierWatcher
  already owning the bus name each skips loudly (exit 0, guidance
  included) instead of aborting on an assertion, and the mock never
  queues behind (DO_NOT_QUEUE) a real desktop's watcher — running
  the binaries by hand on a live desktop can no longer crash the
  suite OR vandalize the user's tray by stealing the watcher name.


### Added — the app-facing notification + tray API (TASK B side 1)

- **`fdk_notify.h` (core)**: the app side of
  org.freedesktop.Notifications on the native D-Bus client —
  send (summary/body markup/actions/replaces/timeout/icon as
  name, path, or raw image bytes), withdraw, and
  NotificationClosed + ActionInvoked events carrying the
  note_user the send took. The daemon adoption flow (name watch,
  capabilities + identity probe), the NO_BUS/NO_DAEMON states
  reported through the API, re-adoption on daemon restart, and
  teardown-inside-its-own-event are proven by
  tests/test_notify_client.c against an in-test mock daemon on a
  REAL bus (every marshaled field validated at the mock, the
  image-data bytes read back flat).
- **`fdk_tray.h` (core)**: the app side of the tray — a
  StatusNotifierItem export (both interface spellings, the New* +
  PropertiesChanged dual emission) with a complete DBusMenu
  server (labels, separators, check/radio with sibling clearing,
  any-depth submenus, enabled/disabled, live LayoutUpdated, the
  AboutToShow dirty handshake, Event→click callbacks). KDE-first
  / ayatana-fallback watcher registration with re-registration
  through watcher restarts, proven by tests/test_tray_item.c
  against an in-test mock watcher/host.
- **The byte-array bulk form** in the D-Bus value model:
  `fdk_dbus_value_new_byte_array` / `_array_flat_bytes` — one
  flat allocation instead of n byte nodes, marshaled as one
  block, produced by the UNMARSHALER too (every 'ay' on the wire
  is flat). A 256x256 icon stopped being 256K heap nodes on the
  UI thread; notifyd's and trayhost's byte walks take the flat
  path first (both rigs re-run ALL PASS). Exports 758 → 795.

### Added — the UI font size (TASK C)

- **`fdk_ui_font_size` / `fdk_ui_scale` / `fdk_ui_font_px`**
  (core, fdk_theme.h; src/theme/ui_metrics.c): one process-wide
  answer to "what size is the UI text?", resolved like the color
  scheme — `$FDK_UI_FONT_SIZE`/`$FDK_UI_SCALE` overrides, then
  FDK's settings store (`ui.font_size`/`ui.scale`), then the
  desktop's statement (XSETTINGS `Gtk/FontName` on X11; the
  portal's `font-name` and `text-scaling-factor` — followed LIVE
  unless overridden), then the 16 px / 1.0 defaults. The shells
  scale every dimension off it (card, pads, icon box, buttons,
  bar height, cell, menu rows) with `--font-size` pinning; core
  chrome re-derives its historical sizes at the 14 px baseline
  (ratios preserved; `FDK_UI_FONT_SIZE=14` = the old pixels).
  tests/test_ui_metrics.c plus the rigs' XS sweeps (12/16/24/32
  through the ENV channel: glyph-free side pads, on-screen
  menus, trace math) all green.

### Added — the StatusNotifier tray host (S-1 / A-11)

- **`trayhost` (a shell add-on example, S-1 / A-11)**: a complete
  org.kde.StatusNotifierWatcher host on FDK's native D-Bus client,
  rendering the registered StatusNotifierItems as a floating
  top-right TOP-layer strip on both backends. The full protocol
  surface: both registration dialects (the KDE bus-name form and
  the Ayatana object-path form), the four watcher properties
  through org.freedesktop.DBus.Properties (Get/GetAll/
  PropertiesChanged emission), the host self-registration over
  the real bus path, item loading with the
  org.kde→org.ayatana→org.freedesktop interface fallback chain,
  the a(iiay) ARGB32 network-byte-order pixmap decode with
  best-size selection, the (sa(iiay)(ss)ss) ToolTip parse, the
  New* signal family (scoped to the item's unique bus name — the
  documented well-known-sender match-rule trap), and vanished-item
  drops via NameOwnerChanged (hostile clients never unregister
  politely). The full com.canonical.dbusmenu client: GetLayout's
  two-out-argument reply parse (u revision + the bare root
  struct), AboutToShow handshakes, Event(clicked/opened/closed)
  with NO_REPLY_EXPECTED, mnemonic-underscore stripping,
  separators/checkmarks/radios/disabled rows, cascading submenus
  that flip at the screen edge, and mid-open refreshes that are
  discarded stale-safe (the open rows point into the old tree).
  Rendering honesty: Passive items fade to alpha 0.45 (never
  hidden), NeedsAttention swaps to the attention pixmap plus an
  accent marker strip, left/middle/right clicks drive
  Activate/SecondaryActivate/the menu. Hostile-input posture and
  the polite bus-name etiquette match notifyd's. Env trace:
  FDK_TRAYHOST_TRACE=1. Rig: the add-on's test-tray rig (62
  checks; D-007 records the shape decisions).

### Added — the notification daemon (S-2 / A-12) and its substrate

- **`fdk_icon_theme_surface(name, size)` (FDK core, public API —
  the 758th export)**: the icon-theme engine the file dialogs use,
  exposed as an owned-clone lookup for applications that draw their
  own chrome. Resolves one icon name through the full live-theme
  policy (XSETTINGS, the GTK/KDE/XFCE channels, Inherits, hicolor,
  pixmaps), rasterizes themed SVG at the requested size, tints
  `-symbolic` names, and hands back a fresh ARGB8888 surface; NULL
  on a miss is the caller's cue to draw its own glyph.
- **`notifyd` (a shell add-on example, S-2 / A-12)**: a complete
  org.freedesktop.Notifications server on FDK's native D-Bus client,
  displaying cards as overlay layers on both backends. The full
  specification surface (Notify with replaces_id/actions/hints,
  expiry semantics including critical-never-expires, CloseNotification
  with correct reasons, ActionInvoked, honest GetCapabilities,
  GetServerInformation, Introspect), a render-only markup sanitizer
  (links drawn, never followed; `<img>` stripped, src never opened),
  size/rate caps with every call still answered, the polite
  bus-name etiquette (one clear line naming an incumbent through its
  own GetServerInformation; `--replace` / `--wait` for the takeover
  dance), per-card layers sized exactly to the card (the honest
  click-through shape until input regions land), stacking that
  self-heals across output changes, and an env-gated trace
  (FDK_NOTIFYD_TRACE=1). The rig
  (the add-on's test-notifyd rig) proves it against a real
  dbus-daemon with libdbus and GLib as cross-implementation callers,
  plus pixel, XTEST-click, and EWMH legs.
- **`docs/TESTING-ON-DESKTOP.md`**: the numbered owner-verification
  checklist for everything only a real desktop can prove.

### Fixed

- **LSan vs fontconfig's cold cache**: a fresh CI runner's first
  fontconfig query writes its on-disk cache through the FcAtomic
  machinery, whose small bookkeeping pools (320 B) sit outside
  fontscan.c's `__lsan_disable` bracket and are held for the process
  lifetime by design — nondeterministic by cache temperature. Now
  suppressed by module in tests/data/lsan-gl.supp (the GL-lane
  pattern), so sanitized runs stay strict for everything FDK owns.

## [Unreleased-candidate fixes] — 2026-10-27 (on the rc2 line)

### Fixed — CI and packaging truth

- **The five-family packaging red at 0.2.0**: the shell add-on's Makefile
  carried its own hand-maintained version (0.1.0) next to the
  header's single source of truth (0.2.0) — the staged library was
  the staged add-on library and the generated `.pc` said `Version:
  0.1.0`, and every distro install-test caught the mismatch. The
  Makefile now derives the version from the header (FDK's own
  established pattern).
- **The alpine CI flake**: readbacks ride a second X connection and
  could beat the just-flushed present to the server, reading the
  PRE-paint frame (the empty search entry — pure fill — exactly the
  asserted-against pixel). `x11_frame_settle()` now guards all 18
  paint-then-readback sites in the X11 suite.
- **The void mirror outage**: `repo-default.voidlinux.org`'s
  round-robin was unreachable for hours; the single same-host retry
  could not recover. Real mirror rotation now runs before every
  remote install (build + both install-test containers).

### Fixed — D-Bus accessors (found by the alt-client test port)

- `fdk_dbus_message_signature()` reported the frozen received-message
  field for locally BUILT messages — an app that appended a body read
  back `""`. It now reports the builder's live signature (what
  serialize() puts on the wire).
- `fdk_dbus_message_read_args()` consulted only the received-message
  body store — reading back a locally built message silently returned
  "no arguments". It now reads the builder's buffers.
- `dbus_internal.h` unconditionally redefined `FDK_LOG_TAG`,
  re-attributing a caller's own log tag (the test binary's
  `dbus-test` became `dbus` on every line). The including
  translation unit owns its tag now.

### Added — the cross-implementation proof

`make test-dbus` gains the libdbus leg: dbus-send calls the service
FDK serves and libdbus's own parser must accept FDK's reply bytes
and print them (ported from the retired alt client line; see
DECISIONS D-004). Plus the append all-or-nothing, reply/error
routing, and error-name discipline sections in the headless battery.

### The repository

- A `release` branch existed in the project's former repository (a
  fast-forward-only pointer at approved, tagged, CI-green versions —
  documented then in docs/RELEASING.md; created at the v1.0.0 tag
  commit), with CI running on both `main` and `release`. It is gone:
  the project returned to in-development status (2026-10-31) with
  one branch, `main`, zero tags, and no published releases.

## [1.0.0-rc2] — 2026-10-26 (development candidate — no tag, no release)

### Added — the native D-Bus client (`include/fdk/fdk_dbus.h`)

The architectural unlock of the rest of the roadmap: a complete
D-Bus client implemented FROM THE PUBLIC SPECIFICATION in raw C
(src/dbus/), taking **no D-Bus library dependency of any kind** —
no libdbus, no sd-bus, no GLib — and no new link-time or run-time
dependency at all. Everything rides FDK's own event loop; every
byte from the socket is treated as untrusted input.

- **Wire format**: full marshaling and unmarshaling of every basic
  and container type (alignment, both endiannesses, nesting depth
  ≤ 64, 8 MiB message cap), with the byte layout PINNED against
  captured reference-implementation output in the tests (the
  169-byte GetNameOwner frame, variant bodies, int64 arrays,
  empty-array padding).
- **Transports + auth**: session/system bus resolution
  (DBUS_*_BUS_ADDRESS, XDG_RUNTIME_DIR, ';' lists),
  unix/path, unix/abstract, unix:dir, tcp:, nonce-tcp (percent
  decoding); SASL EXTERNAL with the ANONYMOUS fallback; non-blocking,
  CLOEXEC sockets; a no-bus environment fails fast with ONE log
  line (the degradation contract every feature above this layer
  keys off).
- **Calls, signals, names**: async method calls with timeouts (at
  most once; late replies to canceled calls dropped), match rules
  (AddMatch refcounted, client-side fail-open evaluation incl.
  argN), name ownership (RequestName + unicast NameAcquired/
  NameLost), name watching with transition reconciliation
  (GetNameOwner + NameOwnerChanged), properties (Get/Set + typed
  PropertiesChanged subscription), introspection (a lenient XML
  parser serving a full node model), and serving incoming method
  calls (with the UnknownMethod auto-reply posture).
- **The reentrancy discipline**: callbacks may call any fdk_dbus
  function — including closing the connection whose callback is
  running; a DEAD connection stays a valid handle until the app
  closes it (every call answers a clean error, never a dangling
  dereference); closes from inside a callback free at the
  outermost wrapper.
- **Hardening**: fuzz tests in plain `make test` (all truncation
  prefixes + 20k seeded mutations under ASan/UBSan), grammar
  validators (names/paths/strict UTF-8), uninvited SCM_RIGHTS
  descriptors closed on arrival.
- **Test surface**: tests/test_dbus.c (headless wire suite) and
  tests/test_dbus_live.c (`make test-dbus` — a REAL dbus-daemon via
  dbus-run-session; the ubuntu CI job makes skips fatal). 115 new
  exported symbols (756 total), append-only; SONAME stays
  libfdk.so.1.

### Added — extension API

- `fdk_ext_loop_update_fd()` — retarget a live fd source's watch
  mask (the POLLOUT busy-wait trap's answer; the D-Bus client is
  the first consumer). FDK_EXT_API_VERSION stays 1: a purely
  additive function, no ops-table growth, old add-on binaries keep
  working.

## [1.0.0-rc1] — 2026-10-25 (development candidate — no tag, no release)

### Added — the extension API (`include/fdk/fdk_extend.h`)

The point of this release: FDK stays simple and becomes the root
other libraries and packages build on. Add-on packages can now be
written against a small, documented, stable, versioned public
surface instead of FDK's internals. Full documentation:
docs/EXTENDING.md.

- **Extension API versioning**: `FDK_EXT_API_VERSION` (compile-time)
  and `fdk_ext_api_version()` (runtime) — add-ons declare what they
  need and check what they got; additions bump the number,
  removals/renames are forbidden (same append-only discipline as the
  core ABI policy).
- **Surface kinds**: `fdk_ext_register_kind()` +
  `fdk_ext_surface_ops` (create / destroy / popup_attached) — an
  add-on registers a named kind of surface FDK knows nothing about,
  creates the native surface itself (X window, or wl_surface plus
  whatever protocol role it speaks), and hands it to FDK, which
  adopts it: event routing, the full `fdk_surface` render path
  (framebuffer, damage tracking, present, frame pacing), and popup
  parenting all work on extension surfaces.
- **Event loop integration**: `fdk_ext_loop_add_fd()` /
  `fdk_ext_loop_remove_fd()` — poll fd sources integrated into
  `fdk_pump_events()` with the timer discipline (reentrancy-safe,
  snapshot + deferred free). Timers ride the existing public
  `fdk_timer_add/remove/reset` API — one primitive, not two.
- **Documented native accessors**: `fdk_ext_get_native_display()`
  (the `Display*` / `struct wl_display *` the context is using),
  `fdk_ext_surface_native()` (the surface's `Window` XID or
  `wl_surface *`), `fdk_ext_surface_get_surface()` (the public
  `fdk_surface *` render target). No private struct is ever
  reachable.
- **Popups parented to extension surfaces**:
  `fdk_ext_popup_create()` — parent-relative positioning, input
  grab, outside-click and Escape dismissal (delivered as
  `FDK_EVENT_WINDOW_CLOSE_REQUEST`), on both backends; on Wayland the
  kind's `popup_attached` hook lets the add-on parent the xdg_popup
  to its own role object (the role's `get_popup` pattern).
- **Backend + capability queries**: `fdk_ext_active_backend()`
  (Wayland or X11/XLibre) and `fdk_ext_has_capability()` (SHM
  rendering, popup grabs, EWMH dock hints, compositor frame pacing).

### Added — the shell add-on, the first add-on package (separate directory,
             own build, own version — ready to move to its own repo)

- Shell-surface-style layers behind one API: layers (background /
  bottom / top / overlay), edge anchors, margins, exclusive zone,
  keyboard interactivity modes, per-output selection (Wayland).
- Wayland backend: the shell-surface protocol v4 via wayland-scanner
  generated bindings (vendored protocol XML).
- X11/XLibre backend: `_NET_WM_WINDOW_TYPE_DOCK` +
  `_NET_WM_STRUT_PARTIAL` struts — the same layering semantics the
  EWMH world offers, behind the same API.
- Desktop integration on X11 (found live on a real NVIDIA desktop):
  layers are STICKY across workspaces (`_NET_WM_DESKTOP` =
  0xFFFFFFFF + `_NET_WM_STATE_STICKY`, set as pre-map properties
  and re-asserted as root client messages after mapping — the
  Wayland behavior, arranged through EWMH); TOP/OVERLAY layers
  carry `_NET_WM_STATE_ABOVE`, OVERLAY adds SKIP_TASKBAR/SKIP_PAGER
  (the strongest EWMH stacking behavior; the fullscreen-over-docks
  protocol ceiling and what a custom WM would need to honor a true
  overlay layer are documented in the add-on's README); and the demo
  bar repaints on `FDK_EVENT_WINDOW_EXPOSE` — the content-loss
  contract that fixes the ghost-bar bug (a desktop round-trip left
  the bar showing whatever was underneath).
- The add-on's shared library links FDK's shared library (a real
  `NEEDED: libfdk.so.1` entry — the whole chain resolves through
  the loader and the distro dependency tooling), and its release
  config drops the sanitizer runtimes like FDK's does.
- Popups built on the Part-1 popup API; a demo bar (top-docked,
  one working dropdown); Xvfb strut/dock verification test and a
  prefix-isolation build test proving the add-on compiles against
  FDK's installed public headers and library ONLY.
- The add-on packages in all five distribution families
  (deb/rpm/arch/apk/xbps), each declaring a real, versioned
  dependency on the FDK runtime pinned at the extension-API floor;
  every family's install-test proves the dependency is enforced by
  the native package manager (installing the add-on alone is
  REFUSED), and the build itself compiles against the INSTALLED FDK
  packages — the isolation proof re-run inside every packaging
  container (the add-on's own packaging directory).

### Packaging / build

- The shared library file becomes the numeric `libfdk.so.1.0.0`
  (pre-release suffixes never appear in the .so file name — the rc
  lives in the version string, pkg-config, and package metadata);
  the SONAME symlink stays `libfdk.so.1` (the additions are
  ABI-append-only — no existing public symbol, type, or struct
  layout changed).
- New public header installed: `include/fdk/fdk_extend.h`; the
  umbrella `fdk.h` includes it.

### Fixed — the rc version vs. the five package grammars

Two distinct rc-version defects, both found live by CI on the
candidate's first pushes:

1. **Package metadata**: the raw `1.0.0-rc1` string is illegal or
   wrong in four of the five package version grammars (rpm rejects
   the hyphen outright; apk reserves it for pkgrel; arch's `pkgver`
   forbids it; xbps forbids it AND the ambiguity broke repository
   lookups; deb accepted it but ordered the rc ABOVE the future
   final). The pipeline now translates the suffix once, at the
   metadata layer (`fdk_packver` in `packaging/ci/common.sh`):
   `1.0.0~rc1` for deb/rpm, `1.0.0_rc1` for apk, `1.0.0rc1` for
   arch/xbps. Plain releases pass through unchanged.

2. **The .so file name**: the real file was installed as
   `libfdk.so.1.0.0-rc1`, and Alpine's abuild derives the apk
   `so:`provides version from the .so FILENAME tail
   (`sover=${file##*.so.}`) — producing
   `provides = so:libfdk.so.1=1.0.0-rc1`, which `apk index`
   rejects with "package file format error" (reproduced locally
   with apk-tools 1.0.0; even a hyphenless `1.0.0rc1` tail is
   rejected — only numeric tails are valid). The Makefile now names
   the real file with the numeric `FDK_SO_VERSION`
   (`libfdk.so.1.0.0`) — the GNU convention; pre-release suffixes
   never appear in the .so file name.

The dividing line in both fixes: pkg-config `Version` and the smoke
test keep the raw string the library itself reports, while the .so
file name is numeric — the file name and the version string are
different things, and each stays what its grammar requires.
Also fixed a third, independent CI flake the second push exposed:
the HiDPI GUI test's private 192-dpi Xvfb used a hard-coded `:98`
inside the `make test-x11` harness's own display range
(90 + pid%400), and on collision the test unlinked the SUITE
server's socket, its probe then reached the 96-dpi suite server
over the TCP fallback, and the scale assertion aborted the debian
job (1-in-400 per run, PID-gated). The private display now lives
below every harness range, is checked free before spawning, and is
verified OURS by its 800x600 geometry — a foreign server is never
touched (docs/testing.md's 1.0.0-rc1 lesson).
Documented in `docs/packaging.md`. Also corrected three stale "ten
shipped theme faces" package descriptions the one-face alignment
had missed (deb control, rpm spec, xbps descriptions now say the
default theme, faded-dream).

### Fixed — the GPU present lane's self-check verdicts (found live
                on an NVIDIA GTX 1050 Ti desktop)

The lane's first-frame self-verification had three defects, all
visible as repeated warnings on a real desktop (every dropdown
click re-logged the whole failure sequence, once per new popup
window):

1. **Not-viewable reads were treated as GPU failures.** A popup
   whose first present raced its own map got `XGetImage` →
   BadMatch — a property of the WINDOW STATE, not the driver. The
   check now gates on viewability first (`map_state ==
   IsViewable`, the programmatic MapNotify/Expose equivalent) and
   clamps its probe region into the server-reported window bounds;
   a not-viewable or unreadable window is INCONCLUSIVE and retried
   at the next completed present, never a downgrade.
2. **Present-latency timing was treated as a driver limitation.**
   The first read after `glXSwapBuffers` can still show the
   window's pre-frame background (DRI3/Present schedules the flip
   at vblank; compositing WMs re-composite on their own clock) —
   the live report's "server (000000) vs CPU (color)" mismatch was
   exactly the bar window's BlackPixel background. A bounded retry
   budget (~3 vblanks) now separates the timing case (a retry
   matches: PASS, logged once at INFO) from the real limitation
   (every retry mismatches: FAIL).
3. **The verdict was per-window.** Each new popup re-engaged the
   lane, re-probed, re-failed, and re-logged — the repeated
   warnings, and the XID churn (window + GLX context + GC + two
   SHM segments per click). The verdict is now decided once per
   connection (the driver-wide truth): PASS and later windows skip
   the probe entirely; a REAL failure refuses the lane for the
   whole process (the same env-knob-keyed policy cache the
   renderer verdict uses) and logs exactly one warning.
   `FDK_GL_SELFCHECK_POISON=1` is the new test seam (the
   FDK_GL_FAIL_AT shape) and `fdk__x11_gl_test_reset()` clears the
   stamp between test groups.

### Fixed — the second live-desktop round (2026-10-26; labwc/Wayland + Openbox/NVIDIA X11)

Six findings from the owner's machines, each reproduced headlessly
before the fix and measured before/after where a number exists:

1. **Wayland: the buffer-in-flight freeze (W1)** — the release wait
   in `get_framebuffer` used the DEFAULT-queue
   `wl_display_prepare_read()`, which refuses while ANY default-queue
   event is pending — under pointer motion that is always true, so
   the wait degenerated into a pure 100ms sleep: releases on the
   socket were never read, every acquire refused, the dispatch rate
   collapsed to ~10 events/s against ~1000/s arriving, and wlroots
   killed the connection when the server-side buffer overflowed. The
   wait is now queue-scoped (`prepare_read_queue` on the release
   queue): a release on the wire lands within one poll slice, and a
   pending release event is dispatched immediately instead of slept
   on. Complements: `wl_display_flush` after every commit (a present
   between pumps no longer waits a full pump cadence to reach the
   compositor), the frame request marshalled BEFORE the commit it
   paces, an fd-lifetime flush in `create_shm_buffer`/cursor upload
   (libwayland resolves fd arguments at FLUSH time by number — pools
   created in one un-flushed burst aliased the newest memfd
   server-side, so whole paint cycles silently never appeared), a
   frame-callback pacing gate in the dispatch tail (hover repaints
   can no longer commit faster than the compositor consumes), and the
   release budget tightened 100ms -> 25ms. Verified by flood rigs on
   headless sway AND labwc (~1000 injected events/s for 45s, popup
   held open, max-rate variant): zero "in flight" warnings, zero
   connection deaths, buffers created == destroyed (no leak), the
   bar alive and selecting items afterwards.
2. **Wayland: the cursor never answered enter (W2)** — a client
   that answers no `wl_pointer.enter` keeps whatever shape the
   compositor last showed anywhere (the report: the resize cursor
   "sticking" over the bar until the pointer left). Every enter now
   re-applies the window's stored shape citing the ENTER serial:
   cursor-shape-v1's `set_shape` when the compositor offers it
   (sway 1.10 does), else `wl_pointer.set_cursor` with the
   XCursor-theme image. New public surface at every layer —
   `fdk_window_set_cursor()`/`fdk_cursor_shape` (core),
   `fdk_ext_surface_set_cursor()` (extension), and
   `fdk_ext_surface_set_cursor()` (extension) — two shapes:
   the arrow and the pointing hand (the demo bar shows the hand over
   its button). Rig-verified at the protocol level (every enter
   serial cited by a `set_shape`) and the pixel level (hand over the
   button, arrow elsewhere, hand again on motionless re-entry).
3. **The hover that lit the whole bar (W3)** — the DEMO's bug, not
   the pointer path's: the bar repainted its entire background in a
   hover tint when any widget was hovered (the bar's own
   `hover_button ? BAR_BG_HOVER : BAR_BG`). Only the button chip
   carries hover now; ENTER seeds hover with its carried position
   (re-entry onto the button highlights without any motion) and
   LEAVE clears it. Pixel-verified in all four states.
4. **X11: every app paid the input-method startup (X1)** —
   `XOpenIM` ran at connect for every application; with a slow or
   dead IM daemon configured that is SECONDS of startup for
   pointer-only apps. The IM and per-window XICs are now lazy
   (first FocusIn/KeyPress), `FDK_STARTUP_TRACE=1` phases the whole
   startup (connect legs, EWMH, theme, first fontconfig query, GL
   self-check, first present) with per-phase and total milliseconds,
   and `FDK_LOG_LEVEL` (read at library load) answers field reports
   without recompiling. Measured with a 700ms XOpenIM stall injected
   via LD_PRELOAD: eager init 714ms / total 769ms -> lazy init 3ms /
   total 54ms (the stall now costs a pointer-only app nothing); the
   healthy-IM case improved 16ms -> 4ms of init. The typing tests
   (real key entry through the lazy XIC) all still pass. One
   regression FOUND AND FIXED by this change: window-create's XID
   was previously flushed to the server only by the XIC round-trip —
   a second connection could observe BadDrawable racing an un-flushed
   create (reproduced minimal; an explicit `XSync` barrier now owns
   the contract).
5. **X11: the dropdown highlight trailing the pointer (X2)** — two
   confirmed contributors. (a) The dispatch loop translated every
   queued MotionNotify one at a time with a repaint each — now
   redundant motions for the same window collapse to the latest
   (buttons/keys/crossings keep exact order). (b) Every popup built
   its OWN GLXContext on the GPU present lane — the panel/popup
   class (<= 131072 px) now stays on the CPU path by policy (no
   context, no texture upload, no vsync-scheduled swap; a
   small-windows-only process never even dlopens libGL;
   `FDK_GL_FORCE=1` overrides, and the byte-identical equivalence
   proof still runs through it). New measurement:
   `FDK_POPUP_TRACE=1` reports `[popup] motion->present N ms` at
   `fdk_surface_present` — the convergence of every present path, so
   extension-API popups (which paint directly) are measured exactly
   like widget-tree windows. Under Xvfb/llvmpipe flood (max wire
   rate, hover forced to flip every event): median 0-1ms, p90 1ms,
   max 32-38ms in both lanes. On the reporter's NVIDIA hardware the
   expected field signature: no more "GPU present lane active" per
   click, and the same sub-frame latencies.
6. **The popup trace's "(destroy)" (X3)** — close-requests now carry
   their reason (outside click / Escape / compositor dismissal —
   the generators stamp a single-slot channel; was "(unknown)" for
   the X11 outside-click and the Wayland dismissal), and destroys
   report the best-known reason, including "in-bounds button N ms
   ago — likely item selected". All six paths verified live on both
   backends.

`scripts/w1_stress.sh`, `w2_verify.sh`, `w3_verify.sh`,
`x2_measure.sh` (with the XTest driver) are the reusable rigs.

The round's own push then exposed its build defect, closed before
the next: the cross-package prerequisites that keep sibling-tree
builds honest (an FDK rebuild must invalidate the add-on's binaries —
the rigs' three-patch-old bar) had landed as UNCONDITIONAL make
prerequisites on `../build/libfdk.a`/`../build/libfdk.so`, and the
pristine add-on-only tarball families (rpm/alpine/arch) died at
`No rule to make target '../build/libfdk.so'` — their link inputs
point at the installed FDK and their trees carry no sibling checkout
at all (debian/void survived only because their build contexts still
contain one). The prerequisites are now gated on the EXPANDED link
inputs (deferred so they judge the caller's final values), and the
isolation proof grew the pristine-tree leg — the package copied
ALONE into a scratch directory, the containers' exact condition —
that would have caught it before CI did.

KNOWN FLAKE (once, not reproduced in 5 runs including under load):
the add-on's passive-grab test asserted `presses >= 1` zero-times
during a rig-teardown-heavy window — watched for in CI, no code
path connects it to this round.

### Fixed — the live-desktop bar findings (2026-10-26; sway/labwc + Openbox)

Four root causes found by running the add-on demo bar on real
desktops, each reproduced in this environment first (headless sway
AND labwc 0.8.3 for the Wayland legs; a real Openbox 1.0.0 under
Xvfb for the X11 legs) and each pinned by a permanent test:

1. **Wayland: the layer surface's startup sequence** — the bar
   painted immediately after `show()`, so FDK's present attached and
   committed a buffer BEFORE the layer surface's first configure,
   and wlroots killed the whole connection
   (`zwlr_layer_surface_v1: error 2: layer_surface has never been
   configured`). Extension surfaces now ride the same
   first-configure deferral xdg windows always had: presents before
   the role's configure are deferred, and
   `fdk_ext_surface_notify_configure()` (the add-on's post-ack
   report) commits the deferred first frame — the protocol rule is
   enforced by FDK, not left to each add-on. Pinned by the
   eager-paint test, the bar rig's WAYLAND_DEBUG trace assertions,
   and alive/pixels legs against both compositors.

2. **The fatal-dispatch busy-loop** — after that connection died,
   the app-owned pump loop spun at 100% CPU logging the same
   "backend dispatch_pending failed" line ~2.2 million times in 3
   seconds (the backend's circuit breaker logged once; the core's
   own error report did not). The core now reports a fatal dispatch
   ONCE per code, and identical repeats sleep out the caller's
   remaining timeout before returning the error — an ignoring loop
   paces itself instead of spinning. The demo bar now exits
   non-zero on a fatal pump. Pinned by the bar rig's
   compositor-kill leg (self-exit within 5s, non-zero, one line,
   bounded log). The one-time Wayland death line now carries the
   protocol error's interface, object id and code from libwayland.

3. **X11: popup keyboard grabs vs the WM's passive grabs** — an
   active XGrabKeyboard deactivates passive grabs, so Openbox's
   Super+number workspace switching silently stopped working while a
   dropdown was open (recovered when it closed). Popups now take a
   POINTER-ONLY grab and route keys through focus (XSetInputFocus,
   saved and restored, error-swallowed); grab result codes are
   handled explicitly (GrabFrozen/AlreadyGrabbed retried once,
   NotViewable reported); every dismissal path stamps its reason; a
   dispatch-tail watchdog releases any grab whose popup is no longer
   viewable; and a _NET_CURRENT_DESKTOP switch dismisses open
   popups. The GPU self-check's retry budget moved from inline
   XSync+sleep rounds (~20 ms of blocked loop while a grab was
   held) to timer-based attempts fired from the dispatch tail (the
   pump wakes for them through the backend's new
   `next_wakeup_ms`). Pinned by the in-test EWMH WM's passive-grab
   rig (XTest-synthesized Super+1 received by the WM with the popup
   open and after dismissal), the popup pixel-stays test
   (root-region readback), and the desktop-change dismissal test.

4. **X11: the layer's self-positioning** — a dock-typed window with
   no position hints is "unplaced", and Openbox ran its placement
   policy: the panel centered mid-screen (confirmed in its debug
   log). The add-on now sets USPosition/USSize WM_NORMAL_HINTS with
   the anchoring math; popups raise on map (an override-redirect
   window created under a raised window stays under it otherwise).
   Pinned by reading the hints back server-side.

`FDK_POPUP_TRACE=1` documents the popup lifecycle for real-desktop
diagnosis: create/map/raise, grab verdicts + focus takeover, first
present (with the lane), unmap/destroy, and the REASON each release
happened with the grab-held duration.

### Fixed — the shell add-on's packaging matrix's first live CI pass

Six defect classes, all packaging/build-environment script defects
(none in library code), all invisible to the sandbox battery — three
of the first four because the sandbox cannot run the native packagers
(no rpmbuild/abuild/xbps-create/pacman, uid 1001, no docker), the
fourth because the local deb verification extracts packages instead
of installing them; the fifth and sixth fell the same way (the .pc
defect needed a clean-container consumer, the XTEST header a builder
without it — this sandbox has carried the Xtst dev package all
along):

1. **The deb/rpm install-test smoke needed X11 dev headers as TEST
   INFRASTRUCTURE** (it reads server-side truth through a second
   Xlib connection; the .pc chain deliberately carries no `x11`
   Requires). The clean install containers now pre-install
   `libx11-dev` / `libX11-devel` — the same class as their
   gcc/pkg-config/xvfb, and what the alpine/void/arch families
   already declared. CI's own logs proved the fix sufficient: the
   identical smoke passed inside the deb build container, where the
   headers existed.
2. **FDK's own rpm install-test selected `dist/*.rpm`**, sweeping
   the add-on's packages into a test whose removal step then got
   (correctly) refused — a working dependency blocking removal of
   its base. It now selects FDK's own rpms by name; arch's
   equivalent `head -1` sort-order landmine is now a deterministic
   `fdk-[0-9]*` glob.
3. **The add-on apk build hand-fed a bare sha512 into the
   APKBUILD** — abuild's checksum array must carry the FILENAME
   (`abuild checksum` writes the correct form; FDK's own apk
   pipeline already used exactly that sequence). The rewrite also
   fixed the artifact collection (abuild writes to REPODEST, not
   the build dir) and dropped the `abuild -r -F` cargo cult.
4. **The add-on xbps build checked `dist/` while xbps-create
   wrote into the current directory** — both packages had built
   successfully; the collection line FDK's own void build carries
   (`mv ./*.xbps dist/`) was missing.

5. **The generated add-on .pc carried the library's platform link
   deps in Libs** (`pkg-config --libs x11 wayland-client` appended —
   every consumer forced to install the X11 AND Wayland dev
   packages; the clean-container smokes died at
   `ld: cannot find -lwayland-client`; build-machine -L paths baked
   into the artifact) **and never defined its own variables**
   (`${libdir}`/`${includedir}` undefined — empty expansion that
   only worked because /usr/include and the default linker path
   need no flags). The .pc now matches fdk.pc's contract: variables
   defined, the add-on's own `Libs` + `Requires: fdk`, platform flags
   in `Libs.private`. The smoke's own second Xlib connection now
   carries `-lX11` on its link line (modern ld refuses to resolve a
   binary's own symbols through a shared library's transitive
   NEEDED — "DSO missing from command line"), and pkg-config
   discovery failures print the real diagnostic instead of
   swallowing it.

The same run closed void: the corrected .pc made the build-phase
smoke discover and link the installed add-on (the old
"cannot find" never recurred to fire its new diagnostic), and the
remaining void failure was the install-test's missing runtime
shlibs — xbps resolves a package's declared shlib-requires only
against INSTALLED providers, so the combined-set transaction
aborted on fdk-devel's libXext.so.6/libxkbcommon.so.0; the
install-test now pre-installs the same runtime shlibs FDK's own
void install-test carries.

Also: the add-on distro build scripts restored the explaining
comment for FDK's `LSAN_OPTIONS=exitcode=0` distro-container leak
policy (the copy had dropped it), the inert copies of that flag
where the sanitizer toolchain is off (alpine/void, musl/minimal)
were removed, and the el10-leg Xvfb gating in the rpm/alpine/void
install-tests now follows the helper's disclosed-skip contract
(`if fdk_start_xvfb; then … else disclose`) instead of hard-failing
on images with no Xorg server.

A fourth push (the live-desktop bar fixes, 2026-10-26) added the
sixth class: **the XTEST dev header is test infrastructure too — for
the BUILDERS this time.** The add-on's new passive-grab passthrough
test synthesizes Super+number keys through the XTEST extension, and
five of six CI jobs died at the same line — `tests/test_x11.c:
fatal error: X11/extensions/XTest.h: No such file or directory` —
because no builder pre-installed the Xtst dev package (`libxtst-dev`
deb/apk, `libXtst-devel` rpm/void, `libxtst` arch). The rpm job
"passed" only through its disclosed el10 Xvfb skip: the suite never
compiled where the header was missing. All six install lists (CI's
ubuntu apt + each family's build container) now declare it. The
dependency is honest, not incidental: the core suite dodged XTEST
for years with XWarpPointer (pointer placement) and XSendEvent
(delivery), but a passive grab activates only on synthesized input
— a KEY event — which XSendEvent's `send_event` delivery can never
trigger; the test that proves the WM's Super bindings survive an
open popup has no cheaper input channel. The same run's void job
died one step earlier — a reposync connection timeout on the
round-robin mirror (a mirror outage, not a code defect); the
critical xbps line now retries once, the same tolerance the later
lines always carried.

### Unreleased-candidate notes

- No git tag, no GitHub release for rc1 (owner's instruction). The
  v1.0.0-era release material belonged to the project's former
  repository; with the 2026-10-31 return to in-development status it
  no longer exists — nothing anywhere is published today. CI still
  builds and install-tests all five package families on every push
  to main — that is pipeline validation, not a release, and it is
  exactly how the rc-version defect above was caught.

## [1.0.0] — 2026-10-12 (historical: belonged to the former repository;
                re-cut same day as the one-face alignment)

The 1.0.0-era version record. It was the first public release of
the project's former repository; with the 2026-10-31 return to
in-development status there are no published releases of any
version — this entry remains as the engineering record of what the
1.0.0 tree contained. The versioned-SONAME scheme activated with
it (`libfdk.so.1`). Highlights of the surface as shipped:

- Dual-backend core (Wayland via xdg-shell; X11/XLibre), software
  renderer (exact + antialiased rasterizer, damage-tracked
  presentation, MIT-SHM / wl_shm transport, optional GLX present
  lane), retained widget tree + box layout + text (stb_truetype) +
  theming (the built-in default plus `.fdk` face files), clipboard
  (text + PNG images, PRIMARY selection), drag and drop, dialogs,
  i18n, a11y narration, undo, prefs, animation, the fdk-theme /
  fdk-prefs / fdk-set CLI tools.
- Ships exactly ONE theme — `faded-dream`, the packaged copy of the
  built-in default/fallback. The next faces are `.FDKThemes`
  drop-ins users install (`~/.FDKThemes` / system data dirs), never
  package content; the Makefile's `SHIPPED_THEMES` is pinned to the
  single file so a stray face cannot ride into a release.
- Five package families built and install-tested in CI (deb, rpm,
  pacman, apk, xbps), source archive + SHA256SUMS — the v1.0.0-era
  publishing of these artifacts on the former repository's release
  page no longer exists; the recipes remain, CI-validated on every
  push.
