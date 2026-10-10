# FDK — Progress Ledger (READ THIS FIRST)

**This file is the anti-duplication contract.** Before implementing
*anything*, check the "Implemented" inventory below and grep the public
headers — if a feature is listed here as SHIPPED, it exists in the tree;
do NOT re-implement, re-add files, or duplicate APIs for it. After
shipping any new feature, update this file **in the same commit**.
Full narrative history lives in `docs/roadmap.md` (the phase-by-phase
account and the GTK/Qt feature-parity ledger); this file is the fast,
authoritative index of that ledger.

Last audited: 2026-10-10, at milestone **1.4.9** (the image
milestone: the PNG codec, the Picture widget, the clipboard's
image surface), by a full in-depth code review — every claim below was
re-verified against the tree, not copied from docs. THE AUDIT'S
OWN FIRST CATCH: the ledger's "themed tooltips" candidate was
stale (the 1.3.2 tooltip has been fully themed since birth —
tokens, parser, Modern retune, tests); the tree is always the
truth, including over this file.

## Current state (verified this audit)

- **HEAD:** milestone **1.4.9** (the PNG codec + the Picture
  widget + the clipboard image surface; 1.4.8 shipped the TextView
  multi-line editor)
- **Build:** green in debug (ASan+UBSan) and release, X11 + Wayland
  backends both linked (Wayland protocols wired: xdg-shell,
  xdg-decoration, viewporter, **fractional-scale**, primary-selection)
- **Public API:** **595 exported symbols**, `make verify-exports` OK in
  debug AND release
- **Tests:** headless suite all-pass (incl. the 1.4.4 + iconview
  suites, the eight 1.4.8 textview groups, and the 1.4.9 png/
  picture suite); X11 integration suite all-pass — **147 [ok]**
  (real Xvfb, real input, incl. the modern-batch, HiDPI (private
  192-dpi server), iconview GUI, file-dialog icon-mode, textview
  GUI, and clipboard-image groups); 34 test files
- **Scale:** ~56k lines of C in `src/` (+ generated Wayland
  protocols), 22 public headers, 12 examples

## Implemented — DO NOT re-add any of these

### Widget catalog (all SHIPPED, class names from `src/widget/*.c`)

button, toggle, checkbox, radio, label, entry, progress (determinate +
busy), separator, frame, notebook, list (multi-select, rubber-band +
auto-scroll, row icons), tree (row icons; multi-select + rubber band
+ auto-scroll), scrollview + scrollbar (smooth wheel), menu /
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
toolbar, tooltip, spinner, link-button (role on button), paned,
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
reads through the surface abstraction), **color button** (the
color-well swatch that opens the ColorChooser — 1.4.4; alpha
checkerboard, heap-token lifetime guard), path-bar composite,
window decorations (server-side deco bar). Entry icon slots
(leading/trailing glyphs + custom painters + press callbacks,
1.4.4). List activate-on-single-click (1.4.4 — the places
sidebar rhythm).

### Dialogs & pickers (SHIPPED 1.4.3 — choosers.c)

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
- DnD: both directions (X11 + Wayland), `fdk_drag_begin`
- Focus traversal, double/triple-click, shift-click range select
- **HiDPI on Wayland: SHIPPED** — fractional-scale-v1 listener,
  viewport source rectangles, `fdk_window_get_scale`
- **HiDPI on X11: SHIPPED (1.4.5)** — Xft.dpi / screen-metric
  detection, integer scales, physical windows, divided input

### Infrastructure (SHIPPED)

- Theme engine: `.fdk` format, **28 color tokens + 14 metrics**
  (`FDK_TK_*` / `FDK_TM_*`), runtime switching, Modern + legacy
  recipes; button roles (suggested/destructive/link), placeholder
  text, hover fades (1.4.1/1.4.2)
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
- Shortcuts/accelerators/mnemonics (1.3.3 / 1.3.6)
- Versioned shared-library export surface (version script +
  verify-exports gate)

## What's left (the authoritative backlog)

### NEXT — milestone 1.4.9+ (the "serious toolkit" audit, value order)

The widget families are shipped across the board (the TextView
closed the last hole in 1.4.8); the remaining work is what a REAL
application still cannot build on FDK today:

1. **Label markup** — rich spans (bold/italic/color, maybe size)
   in labels/buttons/tooltips: a tiny attribute-run layer over
   the shaping engine, no HTML/XML parser.
2. **Window state surfaces** — fullscreen/maximize/minimize
   requests + state queries on both backends (EWMH + xdg_toplevel).
3. **IME completion surface** — LATER by the no-bus policy until a
   protocol joins third_party/wayland-protocols
4. **INCR transfers** — the X11 clipboard's 4 MiB atomic cap
   lifter (screenshots from other tools may exceed it)

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
make                 # debug build, ASan+UBSan
make test            # headless suite
make test-x11        # X11 integration (auto-Xvfb)
make release && make verify-exports   # 531 symbols, both configs
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
