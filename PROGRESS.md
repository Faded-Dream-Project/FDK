# FDK — Progress Ledger (READ THIS FIRST)

**This file is the anti-duplication contract.** Before implementing
*anything*, check the "Implemented" inventory below and grep the public
headers — if a feature is listed here as SHIPPED, it exists in the tree;
do NOT re-implement, re-add files, or duplicate APIs for it. After
shipping any new feature, update this file **in the same commit**.
Full narrative history lives in `docs/roadmap.md` (the phase-by-phase
account and the GTK/Qt feature-parity ledger); this file is the fast,
authoritative index of that ledger.

Last audited: 2026-10-10, at milestone **1.4.16** (the three
faces: Mono Chromatic, Pink Rave, and Faded Dream as the new
built-in default — the maintainer's theming-ability test, shipped
as three .fdk files in the custom `.FDKThemes/` folder with the
TITLEBAR token family that themes the decoration band and the
`button_shape` metric that turns every button circular), building
on 1.4.15 (the chrome uniformity tail: the toolkit's own dialogs
wear the FDK title bar), 1.4.14 (the live settings desktop —
per-app themes, LIVE re-theming when the command runs, the X11
INCR-drop fix, the FDK title bar on every example, the
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

## Current state (verified this audit)

- **HEAD:** milestone **1.4.16** (the three faces — the theming
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
- **Public API:** **617 exported symbols** (616 +
  fdk_widget_set_background_token), `make verify-exports` OK in
  debug AND release
- **Tests:** headless suite all-pass — **564 [ok] checks** (incl.
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
  X11 integration suite all-pass (real Xvfb, real input, incl.
  the modern-batch, HiDPI (private 192-dpi server), iconview GUI,
  file-dialog icon-mode, textview GUI, clipboard-image, the
  1.4.12 INCR read/serve/image groups, the 1.4.14 INCR-DROP
  + settings-live-follow groups against the external rigs, and
  the 1.4.16 titlebar-theming group — Mono Chromatic's black
  band over white content, Pink Rave's hot-pink band over plum,
  the built-in's fallback gray, each on the server's own pixels);
  38 test files (39 .c in tests/ counting bench.c, which is the
  perf harness, not a suite)
- **Scale:** ~58k lines of C in `src/` (+ generated Wayland
  protocols), 22 public headers, 12 examples

## Implemented — DO NOT re-add any of these

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
- DnD: both directions (X11 + Wayland), `fdk_drag_begin`; **the
  1.4.14 INCR-drop fix** — X11 drops now read through the shared
  requestor-parameterized selection engine (atomic, multi-part,
  AND incremental transfers; what GTK/Qt file managers do for
  larger uri-lists — previously the drop read the INCR seed as
  garbage and vanished after a "drop it" hover), with format
  fallback (uri-list refused -> UTF-8 text) and a 600 ms
  convert budget
- Focus traversal, double/triple-click, shift-click range select
- **Window states**: maximize/minimize/restore (Phase 8) +
  **FULLSCREEN (1.4.10)** on both backends — EWMH client messages
  with the bare-X fallback; xdg_toplevel requests with the
  configure states array as truth
- **HiDPI on Wayland: SHIPPED** — fractional-scale-v1 listener,
  viewport source rectangles, `fdk_window_get_scale`
- **HiDPI on X11: SHIPPED (1.4.5)** — Xft.dpi / screen-metric
  detection, integer scales, physical windows, divided input

### Infrastructure (SHIPPED)

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
swept out). What remains is
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
