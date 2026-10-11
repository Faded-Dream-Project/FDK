# FDK — Progress Ledger (READ THIS FIRST)

**This file is the anti-duplication contract.** Before implementing
*anything*, check the "Implemented" inventory below and grep the public
headers — if a feature is listed here as SHIPPED, it exists in the tree;
do NOT re-implement, re-add files, or duplicate APIs for it. After
shipping any new feature, update this file **in the same commit**.
Full narrative history lives in `docs/roadmap.md` (the phase-by-phase
account and the GTK/Qt feature-parity ledger); this file is the fast,
authoritative index of that ledger.

Last audited: 2026-10-11, at milestone **1.5.3** (the picker's
polish pass — the maintainer's third report ["black boxes or bars
around folder icons, worst in the grid, present in the list; the
active desktop theme still not detected without an override; the
sidebar too"] resolved into two root causes, both fixed internally
[public API and its 621 exports unchanged]: (1) icon COMPOSITING
violated the blit contract on the exact-size fast paths
[fdk_surface_blit_transformed's identity and integer-translation
paths routed ARGB8888 icon sources through fdk_surface_blit — the
documented OPAQUE raw copy — so every icon decoded at exactly its
cell size (16-px list rows, 48-px grid cells, the sidebar's places)
painted its transparent margins as opaque BLACK; scaled blits
composited correctly, which is exactly why 1.5.2's verifications
missed it — the fixture icons were full-bleed squares and the
template matches sampled only the reference's opaque pixels; now
ARGB sources composite source-over on both fast paths, XRGB sources
keep the raw copy; the same defect class swept through every
consumer of the primitive: the About dialog's logo (its own plain-
blit call site) and the public Picture widget (inherits the fix)];
regression guards: a render-level transparent-margin test at
identity AND translation, transparent-margined fixture icons +
margin ring checks in the picker rigs, and the SVG grid proof now
sampling the cairosvg reference's TRANSPARENT region; (2) theme
detection now hears the LIVE desktop through XSETTINGS
[Net/IconThemeName read from the settings manager via one
short-lived X connection — pure Xlib, no D-Bus, the no-bus policy
intact; the channel the desktop's own GTK apps obey on X11/XLibre:
xfsettingsd, gsd-under-XWayland, mate/cinnamon, xsettingsd; the
wire format decoded byte-for-byte from a REAL xsettingsd 1.0.2 and
the reader verified end-to-end against that daemon including a
live theme change; precedence: FDK_ICON_THEME > XSETTINGS > user
gtk ini > xfconf xsettings.xml > kdeglobals > classic icon-theme
file > SYSTEM gtk ini (XDG_CONFIG_DIRS + /etc + /usr/share) >
default/index.theme > hicolor; pure-Wayland sessions have no
manager and fall to the file channels — proven under sway with
DISPLAY unset]; tests: headless detection groups for the new file
channels, an X11 XSETTINGS group with an in-repo spec-conformant
fake manager (both byte orders, int/string/color skip, precedence,
fallbacks), and both picker rigs re-verified pixel-level + VLM on
X11 AND Wayland), building on **1.5.2** (the exhaustive
pre-1.0 audit pass — the maintainer's second desktop report
["only recognizable folder icons; text files indistinguishable"]
traced to THREE root causes, all fixed internally [public API and
its 621 exports unchanged]: (1) theme detection missed the system
default channel [the icon search path's own default/index.theme
Inherits= line — the channel stock GNOME/XFCE systems publish; the
engine fell to hicolor on stock systems and every file type dropped
to the procedural glyphs, the reported symptom exactly], (2) PNG-
only lookups missed every SVG-only size [Adwaita's entire 48-px
scalable tree — now rasterized at the REQUESTED size through the
vendored nanosvg (third_party/nanosvg/, zlib license, the stb
precedent; librsvg rejected with its Cairo/GLib stack), same-dir
PNG preferred, undrawable SVGs are honest misses], (3) a broken
asset could shadow the whole chain behind it [the new decode-
refusal set: exists-but-will-not-raster paths are refused, probes
skip them, the search restarts — the shadowing regression test
pins an empty SVG in the own theme losing to the parent's valid
PNG]; verification is pixel-level AND independent [cairosvg/cairo
renders the reference from Adwaita's own SVG; the screenshot grid
must match within AA tolerance; VLM review of both views: "no
defects found"]; the audit also fixed make install PREFIX=X
installing a stale fdk.pc [prefix-stamped regeneration; caught by
the new external-application test — a standalone app compiled
through pkg-config against the INSTALLED tree, run under Xvfb with
the file dialog + icon engine live]; repo-wide TODO/stub/no-op
sweep clean, both-backend batteries green, sway/X11 example rigs
refreshed for the 1.4.16 theme colors and 1.4.17 scrolled catalog;
full narrative: docs/roadmap.md "1.5.2"), building on **1.5.1**
(the picker
stabilization — the file/folder pickers wear the desktop's icon
theme through a new internal freedesktop icon-theme engine
[src/theme/icon_theme.c: XDG discovery, index.theme parsing with
real-theme directory subpaths, inheritance, size matching, PNG-only
raster, symbolic tinting, 3-layer caches reset per dialog]; List and
IconView grew borrowed raster-icon seams (fdk__list_row_set_icon_
image / fdk__iconview_item_set_icon_image / fdk__row_icon_paint_
image); every picker row now classifies by content [folders, images,
audio, video, archives, documents, fonts, source, executables —
extension table + exec-bit split]; tests/test_icon_theme.c pins the
engine headlessly with solid-color fixture themes; the X11 suite
gained three file-dialog-icons groups reading the X SERVER's own
pixels [per-row identity at 16 and 48 px + theme-switch cache drop];
real-theme verification against the system's Adwaita caught two
spec bugs [directory names contain slashes; hicolor's 11-KiB
Directories line] and produced the docs/screenshots gallery pair;
also fixed: FDK_ROW_ICON_RECENT silently reading as NONE, the
dialog icon-grid's one-column layout [fdk__iconview_reflow], and
the engine's uninitialized chain-slot read [found by a 30-run
sweep]), building on **1.5.0** (the
release-engineering pass — RETRACTED same-day, see below; the
packaging/CI infrastructure below remains on main as dormant
release tooling): the version decision itself [1.0.0 adopted, then
**retracted the same day** — the premature v1.0.0 tag and its
never-published draft release deleted; version back to 0.0.1 per
docs/versioning.md's retraction record; SONAME scheme back to
dormant, the Makefile installs the single unversioned libfdk.so;
the packaging scripts gained a pre-1.0 PACKAGING-SKIP guard so CI
stays green AND honest; ABI unchanged at 621 exports]; the
packaging tree
[packaging/: dpkg-deb + rpmbuild + makepkg + abuild + xbps-create,
five families, no libfdk.a anywhere per the license's 4(d)]; GitHub
Actions [ci.yml push/PR validation, reusable distro-packages.yml
docker matrix, release.yml tag → draft release via GITHUB_TOKEN
with minimal permissions and SHA-pinned actions]; docs/packaging.md
the full packaging+release reference; the Debian and RPM pipelines
validated END-TO-END locally [build, package, extraction-based
graphical smoke under Xvfb], musl compile probe 80/80 — then the CI iterations themselves took the matrix to fully green: all five distro pipelines + the ubuntu home-ground job pass end-to-end on main [build, native package, clean-container install-test, graphical smoke, removal] — all before the retraction; the v1.0.0 tag's DRAFT release (four validated families + source + SHA256SUMS, void joining on main after the tag) was deleted as part of it), building on
**1.4.19** (the drop that
landed — the maintainer's live report that real-file-manager drops
hover fine but never complete; three XDND receiver bugs found and
pinned: the XdndStatus action atom was always None, which
action-strict sources [Qt family, several XWayland bridges] read as
a refusal and cancel the drop — now answers XdndActionCopy, and
XdndFinished carries the performed action too [v5]; a failed payload
transfer silently vanished with NO event — now delivered as an
empty-payload DRAG_DROP, the contract Wayland always had; the
XdndTypeList read capped at 64 atoms with the remainder ignored —
now slab-read with a containment cap; drop-fetch budget 600->1000ms;
the capabilities example now prints every decoded payload entry
including folders; verified by a new GTK-faithful rig [8-type
TypeList with uri-list at index 4, real timestamps, strict and
refusing source modes] across a 6-mode x 2-lane app-level matrix and
three new suite groups), building on **1.4.18** (the GPU present
lane — the maintainer's hardware-acceleration ask, verified-absent
first, then implemented as a hybrid: CPU rasterization unchanged and
byte-identical, GLX presentation runtime-loaded via dlopen with a
hand-declared API subset [no new build deps], policy-gated [hardware
renderers only; llvmpipe/softpipe/swrast refused because the SHM path
is faster there; FDK_NO_GL / FDK_GL_FORCE knobs], self-verifying on
its first present, permanently downgrading with full frame salvage on
any failure, observable via fdk_window_get_render_path; the whole
160-group X11 suite additionally ran three times with FDK_GL_FORCE=1
so every widget/dialog/DnD test went through the lane, and a new
examples rig proves byte-identity at the app level), building on
**1.4.17** (the smooth
pass: the maintainer's polishing report — every themed control now
paints through the antialiased primitives so corners and circle
indicators read smooth; the hover-fade use-after-free fixed with
the done-callback handle clear every other animation owner already
had; example 04 opens screen-sized and scrolls its whole catalog
through the new `fdk_scrollview_set_vertical_only` recipe; the five
data-only faces from the previous task are now pinned in the
shipped-theme suite), building on **1.4.16** (the three faces:
Mono Chromatic, Pink Rave, and Faded Dream as the new built-in
default — the maintainer's theming-ability test, shipped as three
.fdk files in the custom `.FDKThemes/` folder with the TITLEBAR
token family that themes the decoration band and the `button_shape`
metric that turns every button circular; plus FIVE MORE faces
shipped data-only at the same audit: Catppuccin Mocha, Catppuccin
Latte, Nordic Frost, AMOLED Void, Solar Flare — ten stems total in
.FDKThemes/), 1.4.15 (the chrome uniformity tail: the toolkit's
own dialogs wear the FDK title bar), 1.4.14 (the live settings
desktop — per-app themes, LIVE re-theming when the command runs,
the X11 INCR-drop fix, the FDK title bar on every example, the
file-picker size retune, the developer theme whitelist) and the
1.4.12 full in-depth code review — every claim below was
re-verified against the tree, not copied from docs. THIS AUDIT'S
OWN FIRST CATCHES: 1.4.15's "X11 suite all-pass" claim was stale —
the decorated dialogs had shifted every dialog test's child
navigation one slot (the band became root child 0) and the suite
had not actually been run; worse, all five custom dialog-body
arrange hooks (message/prompt, About, font chooser, color chooser,
file picker) were double-offsetting their children by the band
height — every dialog's content rode 28px too low since 1.4.15.
Both fixed and pinned at 1.4.16; the tree is always the truth,
including over this file. THE AUDIT'S OWN FIRST CATCH: the ledger's "themed
tooltips" candidate was stale (the 1.3.2 tooltip has been fully
themed since birth — tokens, parser, Modern retune, tests); the tree
is always the truth, including over this file.

Same-date **v1.0.0-prep documentation pass** (docs/branding only —
zero toolkit code touched): the Faded Dream moon logo replaced
`examples/data/fdk_logo.png` (SAME 96x96 RGBA format and size —
examples 02 and 04 decode it at that size; do not resize the asset
without checking their panel layout); the README was rewritten
concise with the Faded Dream Project banner and badge row; `LICENSE`
was rewritten as the FDK Proprietary License v1.0 (restrictive on the
toolkit, applications expressly excluded — its Section 5);
`docs/screenshots.md` now indexes the captured gallery;
`docs/licensing-policy.md` updated to match.

Same-date **v1.0.0 release-engineering pass** (build system +
packaging + CI + docs): public version 0.0.1 → 1.0.0 (fdk_version.h
+ its test pin; the Makefile now derives SONAME libfdk.so.1 and
installs the standard versioned layout libfdk.so.1.0.0 + symlinks;
fdk.pc/install paths unchanged in spirit — LIBDIR override already
existed); packaging/ tree with per-distro native recipes (debian
control+dpkg-deb, fedora spec, arch PKGBUILD, alpine APKBUILD, void
xbps) + ci/ orchestrators + common/smoke.c public-API smoke app;
.github/workflows/ (ci, distro-packages, release); docs/packaging.md
new, versioning.md rewritten as the 1.0.0 decision record, abi-
policy/build/licensing-policy/README synced. THE CONFIG-STAMP
FOOTGUN pinned: `make install` after `make release` silently
rebuilds debug+ASan into the payload — all recipes pin
FDK_CONFIG=release on install (caught live by the Debian NEEDED
scan failing on libasan.so.8).

Same-date **license finalization pass** (docs/licensing only — again
zero toolkit code touched): the license's open decisions were closed
by the Licensor — governing law and venue set (laws of Romania,
exclusive venue Bucharest, with the EU consumer-forum savings);
Section 4(d) rewritten from runtime-accompaniment to the
**separate-runtime model** (applications are distributed WITHOUT the
FDK runtime — no bundling, embedding, static linking, or packaging of
it without further written authorization; end users install the
runtime themselves from an authorized channel); the draft banner and
every [REVIEW] flag resolved into operative text; Section 5 gained
5(f) stating the model restricts the toolkit, never the application;
the OPEN REVIEW ITEMS list replaced by a NOTICE recording the
counsel-free adoption; README, `docs/licensing-policy.md`, and the
roadmap's Phase 1 line synced to match. Rights holder unchanged:
FemBoyGamerTechGuy, by established handle.

Same-date **v1.0.0 retraction** (release-state only): the v1.0.0
release-engineering pass was undone hours after it ran — manual
testing showed the file/folder pickers were not release-ready
(uniform page glyphs for every file, no system icon-theme
consultation, visible picker defects). The premature `v1.0.0` tag
was deleted from the remote, the never-published draft GitHub
Release (9 assets: four distro families + source + SHA256SUMS)
was deleted with it, the public version returned to **0.0.1**
(docs/versioning.md carries the full retraction record), the
SONAME scheme returned to its documented dormant state (the
Makefile builds and installs the single unversioned libfdk.so),
and the packaging scripts gained the pre-1.0 PACKAGING-SKIP guard.
Nothing else was rewritten: the license finalization, the
packaging tree, the CI/release workflows, and all source commits
remain on main. The picker + icon-theme overhaul that motivated
the retraction is the next ledger entry.

## Current state (verified this audit)

- **HEAD:** milestone **1.4.19** (the drop that landed — the XDND
  receiver-faithfulness pass: XdndStatus now answers the action atom
  so action-strict sources [Qt family, several XWayland bridges]
  commit their drops, a failed payload transfer delivers an
  empty-payload DRAG_DROP instead of silently vanishing, the
  XdndTypeList slab-read replaced the 64-atom truncation; the full
  narrative is docs/roadmap.md "1.4.19 — the drop that landed").
  Before it: **1.4.18** (the GPU present lane — see above;
  the full narrative is docs/roadmap.md "1.4.18 — the GPU present
  lane (hybrid rendering)"; the one-sentence architecture: FDK is now
  a hybrid renderer — every pixel rasterized on the CPU exactly as
  before, presentation on the GPU where one exists. Verified
  GPU-accelerated operations: the damaged-rect texture upload, the
  fullscreen composite, and the buffer swap — i.e. the entire
  pixels-to-screen half of every frame). Before it: **1.4.17** (the
  smooth pass — see above;
  the full narrative is docs/roadmap.md "1.4.17 — the smooth
  pass"). Before it: 1.4.16 (the three faces — the theming
  ability test the maintainer asked for: **Faded Dream** is the new
  built-in default (the calm gray-with-violet retune; the packaged
  copy ships as .FDKThemes/faded-dream.fdk and the pre-1.4.16
  "FDK Modern" values survive as a documented recipe), **Mono
  Chromatic** (black-and-white discipline: white world, black
  speech, a near-black titlebar family, CIRCLE buttons) and **Pink
  Rave** (the all-pink club floor: plum surfaces, hot-pink accent,
  a hot-pink band) ship in the source tree's **`.FDKThemes/`**
  folder — the custom folder, also live as the per-user
  `~/.FDKThemes` (outranking every system location) and the
  canonical `<datadir>/fdk/.FDKThemes` install target (the legacy
  `fdk/themes` still scanned). The engine grew the **TITLEBAR
  token family** (five FALLBACK tokens: unset keys read through to
  control/text/border, so pre-1.4.16 themes keep their exact band;
  the band's fill, text, rule, and button hover/pressed now come
  from the theme — the title label follows TITLEBAR_TEXT on every
  switch), the **`button_shape` metric** (0 rounded / 1 circle /
  2 square — applied to the catalog Button, MenuButton, AND the
  decoration band's window buttons), and
  **`fdk_widget_set_background_token`** (the token-following
  background: a widget whose fill resolves a theme token at PAINT
  time, so app surfaces track every theme switch live — every
  example's panels now ride it, and the examples' hardcoded root
  backgrounds are gone so all twelve demo apps re-face under every
  theme). Also this milestone: the 1.4.15 dialog double-offset bug
  fixed in all five custom body-arrange hooks + the stale dialog
  X11 groups repaired and re-run for real. 1.4.15 the chrome
  uniformity tail; 1.4.14 the live settings desktop; 1.4.13 the
  command-line face; 1.4.12 INCR clipboard transfers; 1.4.11
  markup; 1.4.10 fullscreen; 1.4.9 PNG codec + Picture +
  clipboard images)
- **Build:** green in debug (ASan+UBSan) and release, X11 + Wayland
  backends both linked (Wayland protocols wired: xdg-shell,
  xdg-decoration, viewporter, **fractional-scale**,
  primary-selection)
- **Public API:** **621 exported symbols** (620 +
  fdk_surface_draw_rounded_rect_aa +
  fdk_scrollview_set_vertical_only/get_vertical_only +
  fdk_window_get_render_path),
  `make verify-exports` OK in debug AND release
- **Tests:** headless suite all-pass — **570 [ok] checks** (incl.
  the 1.4.4 + iconview suites, the eight 1.4.8 textview groups,
  the 1.4.9 png/picture suite, the 1.4.11 markup suite, the
  1.4.13 theme-discovery + CLI-tools suites, the 1.4.14
  per-app-priority / whitelist / live-recheck / fdk-set groups,
  and the 1.4.16 additions: the titlebar-fallback, button-shape
  (arithmetic + pixel proof), widget-background-token groups in
  test_theme, the ~/.FDKThemes + XDG-canonical-vs-legacy
  discovery groups, and the whole test_shipped_themes suite —
  the five faces pinned: faded-dream byte-equal to the built-in,
  mono's black band + circle buttons, rave's hot-pink band);
  X11 integration suite all-pass (**160 [ok] groups**, real Xvfb,
  real input, incl. the four 1.4.18 GPU-lane groups — policy matrix,
  byte-identity of GPU vs CPU presents, the poisoned-present
  downgrade salvage, resize-under-the-lane — plus the whole suite
  green three times over with FDK_GL_FORCE=1; the MIT-SHM
  double-buffer test is lane-aware),
  the modern-batch, HiDPI (private 192-dpi server), iconview GUI,
  file-dialog icon-mode, textview GUI, clipboard-image, the
  1.4.12 INCR read/serve/image groups, the 1.4.14 INCR-DROP
  + settings-live-follow groups against the external rigs, and
  the 1.4.16 titlebar-theming group — Mono Chromatic's black
  band over white content, Pink Rave's hot-pink band over plum,
  the built-in's fallback gray, each on the server's own pixels);
  39 test files (40 .c in tests/ counting bench.c, which is the
  perf harness, not a suite)
- **Scale:** ~58k lines of C in `src/` (+ generated Wayland
  protocols), 22 public headers, 12 examples

## Implemented — DO NOT re-add any of these

### Branding, docs, and release assets (v1.0.0-prep pass)

- **Faded Dream logo** — `examples/data/fdk_logo.png`, 96x96 RGBA:
  crescent moon centered over a night sky fading soft purple (lower)
  to deep dark blue (upper), rounded badge, transparent outside,
  no text/UI elements. Consumed by examples 02 (image panel) and
  04 (picture fits) and displayed at the top of README.md. Do NOT
  create a second/competing logo asset; the generator lives outside
  the repository (scripts/make_fdk_logo.py in the worklog's
  workspace).
- **README.md** — the concise front page (banner, badges, features,
  build/install, first-window example, examples table, themes,
  screenshots, docs table, principles, license notice). The long
  per-phase narrative it replaced lives on in `docs/roadmap.md`,
  `PROGRESS.md`, and `docs/screenshots.md` — linked, not deleted.
- **LICENSE** — the FDK Proprietary License v1.0, finalized: restrictive
  on the toolkit (no redistribute/modify/sublicense/publish/exploit
  without written authorization), with an explicit
  application-developer rights section (Section 5), the
  separate-runtime distribution model (Section 4d — applications ship
  WITHOUT the FDK runtime; end users install it separately), and
  Romanian governing law with Bucharest venue (Section 14). NOT an
  open-source license.
- **docs/screenshots.md** — the gallery index for every capture in
  `docs/screenshots/`.
- **packaging/** — the ONE packaging tree: debian/ (control
  templates + build-deb.sh + install-test.sh), fedora/ (fdk.spec +
  scripts), arch/ (PKGBUILD + scripts), alpine/ (APKBUILD +
  scripts), void/ (xbps template + scripts), ci/ (host-side docker
  orchestrators per distro + release-notes.sh + common.sh),
  common/smoke.c (the public-API package smoke test). Do not create
  packaging metadata anywhere else.
- **.github/workflows/** — ci.yml (push/PR validation), distro-
  packages.yml (the reusable docker packaging matrix), release.yml
  (v-tag → draft GitHub Release via the ephemeral GITHUB_TOKEN,
  minimal permissions, SHA-pinned actions).
- **docs/packaging.md** — the packaging/release reference (install
  per distro, reproduce, runbook, troubleshooting, signing posture).

### Widget catalog (all SHIPPED, class names from `src/widget/*.c`)

button (with **MARKUP since 1.4.11** — fdk_button_set_markup:
styled label widths, run colors, decoration bars; the role
machinery is markup-agnostic), toggle, checkbox, radio, label
(with **MARKUP since 1.4.11** — rich spans in all three display
modes: NOWRAP measures styled, WRAP breaks by styled widths,
ELLIPSIZE cuts by styled metrics; get_text reports the PLAIN
text), entry, progress (determinate +
busy), separator, frame, notebook, list (multi-select, rubber-band +
auto-scroll, row icons), tree (row icons; multi-select + rubber band
+ auto-scroll), scrollview + scrollbar (smooth wheel; 1.4.17:
`fdk_scrollview_set_vertical_only` — content arranged at the
viewport's width, horizontal range collapsed, the recipe for a
scrolling whole-window catalog), menu /
menu-bar / context menu (accelerators + mnemonics), combo (editable),
slider (marks/ticks + labels; vertical orientation), **iconview**
(the 1.4.5 item grid: glyph cells in a re-flowing column field,
the List's selection model, grid keyboard nav, batch fills; the
1.4.6 rubber band + themed cell metrics; the 1.4.7 band
edge-chasing auto-scroll BOTH axes + cell double-click
activation),
spinbutton,
**picture** (1.4.9 — the image display: an owned surface under
NONE/CONTAIN/COVER/FILL fit policies, scaled paints on the
transformed-blit engine, the IMAGE a11y role),
**textview** (1.4.8 — the multi-line editor: word-wrap through the
break engine + no-wrap mode, caret/selection across visual lines
with the goal column, PageUp/PageDown, paragraph triple-click,
the Entry's whole splice/undo/clipboard/PRIMARY/preedit
discipline, the full a11y text interface under TEXT_VIEW),
toolbar, tooltip (with **MARKUP since 1.4.11** —
fdk_widget_set_tooltip_markup: the tip wraps by styled widths and
paints run colors + decoration bars; get_tooltip reports the
PLAIN text), spinner, link-button (role on button), paned,
expander, statusbar, stack + stackswitcher, revealer (slide modes;
crossfade via the paint group), levelbar, canvas (drawing area),
search entry (preset; debounced search-changed; BUILT on the
entry's icon slots since 1.4.4), **menu button**
(attached-popup hybrid — the hamburger), dialog (+ modal run),
**file dialog** (OPEN/SAVE, places sidebar, filters, breadcrumb path
bar, Ctrl+L location entry — modernized 1.4.0; XDG recently-used
place + accept-time recording since 1.4.4; **icon mode since
1.4.7** — options.view seeds the IconView browsing surface, the
toolbar's Icons checkbox flips it live, and every accept path
reads through the surface abstraction; **the 1.4.14 size retune**
— 866x614 window, 15px face, 34px rows, 184px places, 640x420
list: the "small picker, small text" report closed), **color button** (the
color-well swatch that opens the ColorChooser — 1.4.4; alpha
checkerboard, heap-token lifetime guard), path-bar composite,
window decorations (the FDK-drawn title band — themed fill, title,
  **its every pixel theme data since 1.4.16: the TITLEBAR token
  family (fallback semantics) paints the band, rule, glyphs, and
  title label, and the window buttons follow the theme's
  button_shape story (circle themes get circular window
  buttons)**,
close/min/max, band drag, FDK-driven resize; **worn by EVERY
example since 1.4.14** — example_window.h decorates the standard
window, 06/07/09/10 decorate their own, 02 stays bare because its
subject is raw-surface rendering that never paints the tree). Entry icon slots
(leading/trailing glyphs + custom painters + press callbacks,
1.4.4). List activate-on-single-click (1.4.4 — the places
sidebar rhythm).

### Dialogs & pickers (SHIPPED 1.4.3 — choosers.c; icons 1.5.1)

- **FilePicker / FolderPicker iconography (1.5.1)** — every row and
  grid cell wears the DESKTOP's icons via the internal icon-theme
  engine (src/theme/icon_theme.c): user-theme detection (GTK/KDE
  settings + the system default/index.theme channel + $FDK_ICON_THEME
  override, no bus), inheritance chains,
  Fixed/Scalable/Threshold size matching with a relaxed fallback,
  pixmaps last resort, per-type classification (extension table +
  executable-bit split), symbolic tinting, three cache layers plus
  the decode-refusal set, reset per dialog. PNG preferred per
  directory, SVG rasterized at the requested size via the vendored
  nanosvg (undrawable SVGs and XPM are honest misses), procedural
  glyphs as the floor. List rows 16 px, grid cells 48 px.
  Fixed on the way: the RECENT glyph's silent drop in
  fdk_list_row_set_icon's switch; the dialog icon-grid's stuck-at-
  one-column layout (fdk__iconview_reflow, the 1.4.7 lesson's third
  instance); the chain walk's uninitialized-slot read.

- **AboutDialog** — logo/name/version/comments/copyright, LINK
  website row (activating fires a callback with the URL — FDK never
  execs anything), scrolling license block
- **FontChooser** — face list from `fdk_font_enumerate`, size
  spinner (6..96), live preview reloading via `fdk_font_load_face`
- **ColorChooser** — the HSV wheel on the canvas: per-pixel hue ring
  + saturation/value triangle (barycentric), drag hit-testing on
  both regions, marker dots, #rrggbb entry, current/initial swatches

### Platform / input (SHIPPED)

- X11 backend: full window lifecycle, EWMH/ICCCM, XIM **full-Unicode**
  input, resize edges, decorations, **HiDPI** (1.4.5: Xft.dpi /
  screen-metric detection at connect, integer scale [1,3],
  physical-sized windows + divided input — the same core
  compositing the Wayland side rides)
- Wayland backend: xdg-shell lifecycle, libxkbcommon keymap, client
  key repeat (1.3.5), output hot-plug/unplug, compositor-death
  resilience, cursor shapes
- Clipboard: text + URI formats, **PRIMARY selection** (1.3.4)
- DnD: both directions (X11 + Wayland), `fdk_drag_begin`; **the
  1.4.14 INCR-drop fix** — X11 drops now read through the shared
  requestor-parameterized selection engine (atomic, multi-part,
  AND incremental transfers; what GTK/Qt file managers do for
  larger uri-lists — previously the drop read the INCR seed as
  garbage and vanished after a "drop it" hover), with format
  fallback (uri-list refused -> UTF-8 text) and a 1000 ms
  convert budget; **the 1.4.19 receiver-faithfulness pass** —
  XdndStatus answers the v2+ action atom (XdndActionCopy; action-
  strict sources like Qt read a None action as a refusal and cancel
  the drop: hover highlights, release vanishes — the maintainer's
  live report), XdndFinished carries the performed action (v5), a
  failed transfer delivers an EMPTY-payload DRAG_DROP instead of
  vanishing silently (Wayland's contract, now X11's too), the
  XdndTypeList read is slab-bounded past 64 atoms (1024 cap), and
  the GTK-faithful rig + three suite groups pin all of it
- Focus traversal, double/triple-click, shift-click range select
- **Window states**: maximize/minimize/restore (Phase 8) +
  **FULLSCREEN (1.4.10)** on both backends — EWMH client messages
  with the bare-X fallback; xdg_toplevel requests with the
  configure states array as truth
- **HiDPI on Wayland: SHIPPED** — fractional-scale-v1 listener,
  viewport source rectangles, `fdk_window_get_scale`
- **HiDPI on X11: SHIPPED (1.4.5)** — Xft.dpi / screen-metric
  detection, integer scales, physical windows, divided input
- **GPU present lane: SHIPPED (1.4.18, X11)** — hybrid rendering:
  CPU rasterization unchanged (byte-identical, pinned by tests),
  GLX presentation via runtime-loaded libGL (dlopen, hand-declared
  API subset, no new build deps); hardware-renderer policy with
  software refusal (llvmpipe/softpipe/swrast), FDK_NO_GL /
  FDK_GL_FORCE / FDK_GL_FAIL_AT knobs; first-present server-side
  self-verification; permanent downgrade with full frame salvage;
  `fdk_window_get_render_path()` introspection (621st export);
  `tests/data/lsan-gl.supp` for Mesa's exit-time allocations. See
  docs/rendering.md "The GPU present lane (1.4.18)". The Wayland/EGL
  sibling is DESIGNED (same shape) but deliberately unshipped — no
  EGL runtime to verify against in this tree.

### Infrastructure (SHIPPED)

- **The AA chrome pass (1.4.17)**: every themed control paints
  through the antialiased primitives (fill/draw_rounded_rect_aa,
  fill_circle_aa — 57 call sites across the widget layer); interior
  pixels byte-identical to the crisp path, only the curved fringes
  blend; **fdk_surface_draw_rounded_rect_aa** completes the public
  AA family (1-px stroke through the edge rows' pixel centers —
  radius 0 is byte-identical to draw_rect, straight sides exactly
  the crisp outline's pixels, corners ramp)
- **Hover-fade handle lifetime (1.4.17)**: the shared
  fdk__hover_fade_arm's done callback clears the stored flight
  handle on completion AND cancel — the reaped-zombie
  use-after-free the settings demo found under ASan; the
  regression test pumps past the grace and arms again
- Theme engine: `.fdk` format, **33 color tokens + 15 metrics**
  (`FDK_TK_*` / `FDK_TM_*`), runtime switching, the Faded Dream /
  Modern / v1 recipes; button roles (suggested/destructive/link),
  placeholder text, hover fades (1.4.1/1.4.2). **The 1.4.16
  additions: the TITLEBAR family** (titlebar_background/text/
  border/button_hover/button_pressed — FALLBACK tokens: unset keys
  read through to control/text/border so pre-1.4.16 partial themes
  keep their exact band; the deco band, its rule, its glyphs, and
  the title label all resolve them; titlebar_button_* carry the
  band buttons' hover/pressed) **and the button_shape metric**
  (0 rounded / 1 circle / 2 square, applied by the catalog Button,
  MenuButton, and the deco band's window buttons via the shared
  fdk__button_shape_radius); **the built-in default is Faded
  Dream** (gray ramp #232329/#33333C + the violet accent family
  #8F79D9 — the packaged copy ships as faded-dream.fdk and the
  test suite diffs them byte-for-byte); **the override mask**
  (colors_set[] behind the fallback) also fixed a latent UB: the
  parser's duplicate-detection bitmask crossed 32 tokens
- **fdk_widget_set_background_token (1.4.16)**: the token-following
  background — a widget's fill resolves a theme token at PAINT
  time, tracking every theme switch live with no hook (explicit
  set_background still wins and cancels the mode); the examples'
  structural panels ride it
- **The three faces + the .FDKThemes folder (1.4.16)**: Mono
  Chromatic (white world / black speech / near-black titlebar /
  circle buttons), Pink Rave (plum floor / rose ramp / hot-pink
  accent and band), Faded Dream (the default's packaged copy);
  shipped in the source tree's `.FDKThemes/` (daylight + matrix
  alongside), installed to `<prefix>/share/fdk/.FDKThemes/`,
  discovered on the grown search path
  ($FDK_THEME_DIR > **$HOME/.FDKThemes** > $XDG_DATA_HOME/fdk/
  .FDKThemes > the legacy fdk/themes > each XDG_DATA_DIRS entry's
  pair) — test_shipped_themes pins all five
- **Theme discovery + the settings engine + the CLI (1.4.13,
  grown into its final shape 1.4.14)**: fdk_theme_find /
  fdk_theme_available_* over the search path above
  (where `make install` puts the shipped five faces),
  fdk_theme_file_path, the settings ENGINE (src/theme/settings.c:
  $FDK_THEME > the app's own <app_id>.prefs > the global fdk.prefs
  > built-in; the one-shot lazy boot; **the inotify live follow** —
  the pump polls the watch fd, re-resolves on watched-file changes,
  and repaints fdk_run()-shaped apps itself; explicit set_default
  opts the process out AND stands the watch down; fail-soft
  throughout), **fdk_theme_set_allowed_themes** (the developer
  whitelist: stems or internal names, first-allowed fallback,
  immediate re-clamp, settings-only — code is always honored), and
  the **fdk-theme / fdk-prefs / fdk-set** command-line tools
  (tools/, installed to $(PREFIX)/bin; fdk-set theme set NAME
  [--app APP] writes the per-app override) — docs/cli.md is the
  reference
- Animation: easing library (**11 functions**), animator, smooth
  scroll, menu/combo fades, revealer slides, expander door
- **Paint group (1.4.3): per-widget subtree opacity** — the ARGB
  offscreen + global-alpha source-over blit engine (the crossfade
  rides it; nested groups composite naturally)
- **Font surface (1.4.3): `fdk_font_enumerate`** (name-table parse,
  TTC faces, family/style/path sorted) + **`fdk_font_load_face`**
- Accessibility: a11y tree (**41 roles**), narrator, announcements,
  per-widget value interfaces
- i18n: catalog, plurals, dates, locale
- Undo/redo: generic undo stack + entry integration
- Preferences: `.fdk-prefs` persistence, atomic saves (1.3.7)
- Text: fontconfig scan, stb_truetype shaping, AA rendering,
  ellipsize, line breaking
- **Images (1.4.9)**: stb_image decode (file + memory), the PNG
  ENCODER over zlib (fdk_surface_save_png), the Picture widget,
  clipboard image/png (X11 atomic property + Wayland data-device
  pipe, one-content-at-a-time)
- **Rich text / markup (1.4.11)**: fdk_span (byte range +
  absolute style + optional color + underline/strikethrough), the
  span twins of measure/draw/break/ellipsize (zero spans == the
  classic behavior), fdk_markup_parse — a TAG SCANNER (<b> <i>
  <u> <s> <color=#rrggbb[aa]> + the six classic entities; nesting
  merges; unknown tags, stray '<', and unmatched closes stay
  LITERAL; unclosed tags close implicitly), the STYLE-VARIANT-
  keyed glyph cache (regular + bold rasters coexist in one font
  object), decoration bars in run colors, kerning resets at
  style-run boundaries. NO size markup (a font is one face at
  one pixel size — the documented tradeoff)
- Shortcuts/accelerators/mnemonics (1.3.3 / 1.3.6)
- Versioned shared-library export surface (version script +
  verify-exports gate)

## What's left (the authoritative backlog)

### NEXT — nothing

The backlog is EMPTY. The widget families shipped across the
board; rich text shipped in 1.4.11; INCR transfers shipped in
1.4.12; the command-line face shipped in 1.4.13; the live settings
desktop shipped in 1.4.14 (per-app themes, live re-theming, the
INCR-drop fix, the whitelist — the maintainer's own usage reports
were the backlog, and every item closed); the three faces shipped
in 1.4.16 (Mono Chromatic, Pink Rave, Faded Dream as the default,
the .FDKThemes folder, the titlebar family, button shapes, the
token-following background — the theming-ability test the
maintainer asked for, and with it the 1.4.15 dialog-offset bug
swept out); the five extra faces shipped data-only on top of it
(Catppuccin Mocha/Latte, Nordic Frost, AMOLED Void, Solar Flare);
and 1.4.17 closed the maintainer's polishing report in full (the
AA chrome pass over every themed control, the hover-fade
use-after-free, example 04's screen-sized scrolling catalog, the
ten-face enumeration pins). What remains is
LATER-by-policy (IME completion surface
waits for a protocol joining third_party/wayland-protocols) and
OUT-by-policy (the deliberate non-goals below) — the line where
adding anything more would make FDK something other than a
toolkit.

### LATER (real, not next)

- IME completion surface (preedit is display-only today)

### OUT (deliberate policy — do not chase)

GLArea (software renderer is the product) · CSS-like styling (`.fdk`
is the surface) · icon-theme loading (vector glyphs + app surfaces) ·
printing · a11y bus bridges (in-process narrator) · touch/gestures ·
Assistant/wizard · D-Bus anything.

## Verification protocol (run before claiming anything is done)

```sh
source /home/z/my-project/scripts/fdk-env.sh && cd $FDK_ROOT
make                 # debug build, ASan+UBSan (libs + the CLI tools)
make test            # headless suite
make test-x11        # X11 integration (auto-Xvfb)
make release && make verify-exports   # 612 symbols, both configs
```

Full battery (before each milestone commit): interop rig, X11 + sway
example rigs, both tooltip rigs, compositor-death rig — see
`docs/testing.md` and the session rigs in `/home/z/my-project/scripts/`.

## Protocol for future work

1. **Before** writing any widget/feature: read this file, then
   `rg -n '<name>' include/fdk/ src/widget/` to confirm absence.
2. **After** shipping: append to "Implemented", strike the backlog
   item, bump the milestone, update `docs/roadmap.md` narrative +
   ledger, and commit it all together.
3. Never trust a session hand-off summary over the tree — `git log
   --oneline -3` and this file are the truth.
