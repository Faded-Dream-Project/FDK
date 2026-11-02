# §31 continuation — UPDATE (owner round 6: DnD self-drop, drag leaks, multi-select freeze)

## Remote proof
origin/main == this round's code commit + docs commit (see git log). Working tree = clean at that commit.

## What round 6 fixed (ALL ENGINE-LEVEL, no app hardcoding)
1. **X11 self-drop deadlock**: the drop's selection read (wait_selection_notify_on) waited for
   SelectionNotify but never ANSWERED the SelectionRequest our own drag source received on the
   clip_helper — same-process drops timed out at 1000ms and delivered an empty payload (his two
   "owner did not answer" WARNs). FIX: the wait loop now peels SelectionRequests off the wire and
   serves them (fdk_x11_dnd_serve_selection → clipboard fallback), XPutBackEvent for everything
   else. THE E2E XTEST PROOF IS STILL PENDING (rig issue, see below).
2. **Drag payload leaks (both backends)**: x11 dnd_source_finish and wayland drag_source_report
   now free text/uri_payload at drag END (before: freed only at teardown; a second drag
   overwrote the pointers — his 256-byte LSan report). Code-verified; 2-drag LSan gate pending.
3. **Multi-select freeze/never-works**: THREE interacting causes, all engine:
   a. The rubber band only started on EMPTY-space presses; a full directory is wall-to-wall rows
      → unreachable. FIX: row_handle_event now holds a plain press on an UNSELECTED row as a
      "pending band"; motion past 6px slop converts it to the list's band (row keeps the tree
      grab and forwards motions/release/grab-broken, re-localized); release inside slop = the
      classic click-select. PROVEN: new headless test (test_row_press_band) + E2E rig: S1 sweep
      selects 5 rows (12336 selection px vs 2700 on HEAD).
   b. The widget drag-source controller armed at press BEFORE delivery, so a band press ALSO
      fired a drag at 6px → the X11 drag's grab ate the release → banding + 100Hz autoscroll
      timer stuck → next press dead = THE FREEZE. FIX: new public API fdk_widget_set_drag_veto()
      (a widget owning press gestures vetoes arm-at-press); List sets it automatically in
      MULTIPLE mode; drag-from-content now starts only from rows the app arms via the new
      fdk_list_row_set_drag_armed (fdk-files arms SELECTED rows on every selection change — the
      desktop-standard press-selected-row-and-move gesture).
   c. No notification when a drag stole the pointer: new public event FDK_WIDGET_GRAB_BROKEN,
      delivered by fdk_drag_begin_ex to the tree's grabbed widget (root->last_pointer_pos added);
      List ends the band + timer on it. HEAD E2E proof: after a drag attempt, S3's press did
      nothing (frozen, 2916→2916 px); FIXED: S3 selects fresh (12336→2789 px = responsive).
4. Exports: 998 → 1001 (+set_drag_veto +get_drag_veto +fdk_list_row_set_drag_armed). make test
   ALL GREEN including the new band test (note: the suite's lists need a paint pass for layout —
   the new test does fdk_widget_tree_paint first; the OLD band test worked because 0x0 scroll
   made presses hit the list directly — it never exercised row hit-testing).

## NOT FINISHED (next round's first jobs)
1. **E2E self-drop XTest proof**: the stepped-motion rig (scripts/repro_s4.sh) produced NO dnd
   trace at all (not even the release line) — the rig or window-finder is flaky, the fix itself
   is compiled and unit-sound. Debug the rig: check WID, check the app is pumping, one motion
   per driver call with w:40 spacing, then verify folder receives the file + no WARN lines.
2. **2-drag LSan gate** for the leak fix (run the s4 rig twice + exit, expect NO 256-byte
   dnd_uri leak).
3. **FIX C — X11 boot slowness**: NOT STARTED. His log: ~2s gap before "GPU present lane
   active" (=GLX/NVIDIA context creation) + ~1s to self-check pass. Plan: strace -ttt -e
   trace=openat,poll,recvfrom,sendto the boot, diff against wayland; suspects: dlopen of
   libGLX_nvidia + first glXMakeCurrent + any XSync roundtrips in x11_gl bring-up; the
   self-check itself is 9ms timer-paced (NOT the stall).
4. The icons-mode smoke + both-theme self-quits were NOT re-run after round-6 engine changes
   (make test covers the widget layer; re-run them next round before closing).

## Rig index: scripts/repro_r6.sh (band+freeze E2E), scripts/repro_s4.sh (self-drop, flaky),
scripts/xtest_driver (built, needs LD_LIBRARY_PATH=/tmp/x11dev2/root/usr/lib/x86_64-linux-gnu),
scripts/x11_find_window.py (ctypes XQueryTree finder).

# §31 continuation — UPDATE (owner round 5: the round-4 registry's use-after-frees)

## Remote proof
origin/main == `82435e2` ("the round-5 use-after-frees"). Working tree = that commit, clean.

## Round 5's two fixes (both bugs were MINE, introduced in round 4)
1. **ROW-ICON OWNERSHIP** — list/iconview rows BORROW images; my round-4 registry freed
   the clones per pane_build_view. A rebuild of ONE pane (pane_add / split view) freed
   what the OTHER pane still painted. Repro: `--smoke` on HEAD (the split round) → ASAN
   heap-use-after-free in row_paint. Fix: `icons_get_file()` in the app icon cache
   (resolves via the round-4 public engine API, caches by TYPE = directory/extension);
   cache owns surfaces for the process lifetime. Registry deleted.
2. **STALE THUMBNAIL DELIVERY** — the icons-mode smoke then died in thumb_ready_cb: a
   worker thumbnail delivered into a destroyed iconview (smoke navigates mid-flight; the
   request captured the widget pointer). Fix: `tab.view_gen` bumped by every
   pane_build_view; requests carry (tab, gen, iv) in a heap thumb_target; deliveries
   drop on stale generation BEFORE touching widgets. `thumbs_get` grew `out_submitted`
   so the caller knows if the worker owns its user pointer.

## Rig note (do NOT re-add): single-pane self-quit rigs CANNOT see either bug — the
smoke rig (`--smoke`) is the regression test that catches both (detail mode = bug 1,
icons mode = bug 2). The temp FDK_TEST_VIEW_ICONS override was reverted; to test icons
mode use the prefs quirk knowledge: view.mode is saved but never applied on boot (app
quirk, line ~641 discards the read) — the menu is the only way in the shipped app.

## Verification state: smoke (detail+icons) → PHASE:quit zero ASAN; make test GREEN;
exports 998; Papirus self-quit 22/22, Adwaita 19/21 (the 2 = names Debian's Adwaita
lacks); icons-mode screenshot 6830 saturated px (#5294e2 cluster); leaks = libGLX only.

## Owner re-test: ./build/fdk-files — boot, split view (F9 or the split toolbar button),
switch to icons view, navigate around while thumbnails load, close. No ASAN abort; both
panes keep themed icons through each other's rebuilds.

# §31 continuation — UPDATE (owner round 4: the Adwaita crash + invisible file-row icons + the per-entry API)

## Remote proof
origin/main == `b21d109` ("the round-4 trio"). Working tree = that commit, clean.

## Round 4's three fixes
1. **CRASH (ASAN stack-use-after-scope, icons.c:233)** — round-3's plain-before-symbolic
   lookup declared `sym` inside the fallback `if`; `which` borrowed it past the block and
   was strdup'd at the cache store. Fires ONLY when the plain name misses: Papirus ships
   plains (rig never fell in), the owner's Adwaita is symbolic-only (every boot). `sym`
   now lives at iteration scope.
2. **FILE-ROW ICONS** — the bound list carried text only; no icons in any view on any
   theme (his words: "the folder dont even have a folder icon at all"). The list branch
   decorates after bind_model on every rebuild path (navigate/back/fwd/up/inotify/filter
   all funnel through pane_build_view); iconview resolves the same way. Surfaces are
   caller-owned clones tracked in a registry, freed when the view rebuilds.
3. **PUBLIC API** — `fdk_icon_theme_file_surface(dir, name, is_dir, size)` in
   fdk_theme.h: the file dialog's per-entry resolution exported (extension table, folder
   chains, one-stat executable split). Answers whatever the INSTALLED theme ships.

## Theme-genericity (the "not just one theme" concern — it never was theme-specific,
   but now it is PROVEN on two): Adwaita via gtk-ini 18/20 system hits (2 misses are
   names Debian's Adwaita doesn't ship — verified against its file tree); Papirus via
   ~/.icons+default/index.theme 20/20. Both self-quit cleanly; leak report has zero
   engine frames (libGLX_mesa's 108124-byte globals only, unchanged since round 2).

## 16px Papirus note (do not "fix" this): Papirus's small-size SVGs are currentColor +
.ColorScheme-Text (#444444) BY DESIGN — the colored artwork is 32px+ (icon view shows
color; list rows show the theme's own monochrome small set, same as GTK managers).

## Rig map: /tmp/hisbox (Papirus shape), /tmp/adwaitabox (gtk-ini shape), both with
populated HOME (Documents/Downloads/…, .txt/.c/.png/.zip). X11 dev at /tmp/x11dev2
(.pc prefixes repointed); wayland via scripts/restore_wayland_toolchain.sh; make test
needs FDK_ENABLE_WAYLAND=1 + `make tools` FIRST (test_cli_tools asserts fdk-theme).

## Owner re-test: ./build/fdk-files →
- no ASAN abort; sidebar icons themed; FILE rows now carry icons (16px themed set);
  icon view (View menu) shows colored 32px themed artwork
- on quit: `icons: N system hits / M lookups` (Adwaita box: N≈18, M=20 is a PASS —
  view-compact/view-hidden are not in Debian's Adwaita)

# §31 continuation — UPDATE (owner round 3: "icons even in the side bar aren't using system themes")

## Remote proof
origin/main == `77be5c7` ("icons/sidebar: the app never asked the theme — now every icon surface does").

## The finding that mattered
His round-3 log carried our round-2 verdict line: `icons: theme
'Papirus-Nordic' via search-path default` — DETECTION WORKS. The rig
repro with the REAL Papirus package in his exact box shape resolved
12/12 toolbar names SYSTEM tier. **The engine was never broken — the
app never asked.** The places sidebar, devices, bookmarks, trash and
icon-view rows drew procedural `FDK_ROW_ICON_*` glyphs / plain
buttons; no `icons_get` existed for any of them. FDK's own file
dialog had the themed-raster pattern since 1.5.x, but the seam was
engine-internal so an app COULDN'T copy it.

## What changed
1. `fdk_list_row_set_icon_surface()` is now public API (wraps the
   internal seam). The app's places/mounts/bookmarks rows carry real
   freedesktop names and resolve 16px themed rasters, glyph floor
   kept. Icon-view file rows resolve themed type icons (folder,
   image-x-generic, ...). His-box rig: **20/20 system hits**.
2. Toolbar lookup is PLAIN-FIRST, symbolic only as fallback (the old
   symbolic-first order tinted everything flat monochrome — "looks
   like not my theme" while hitting it every time). NOTE: Papirus's
   16px ACTION icons are near-monochrome by design (currentColor +
   .ColorScheme-Text); the colored ones are the places/folder icons,
   which now render colored (pixel-verified on the rig).
3. Self-diagnosing logs: WARN when the detected theme has no
   index.theme on the search path (dangling Inherits — the case that
   silently served hicolor), and a shutdown one-liner
   `icons: N system hits / M lookups` via public `fdk_log`.

## Rig notes (post-sandbox-recycle rebuild)
- X11 dev prefix now /tmp/x11dev2 (debs + .pc prefixes repointed);
  scripts/hisbox_run.sh reproduces the owner's box shape; real
  Papirus from apt (22.6 MB) parked at /tmp/pkg/papirus-root.
- Wayland toolchain restored via scripts/restore_wayland_toolchain.sh
  (make test needs FDK_ENABLE_WAYLAND=1 + `make tools` first —
  test_cli_tools asserts the fdk-theme binary exists).
- A GLM-authored divergent line was found on the local tree
  (c1f27e9 + dirty wayland_egl.c): preserved on branch `glm-line`
  + stash; DO NOT git add -A on this tree — untracked fdk-shell/
  leftovers from that line will be swallowed.

## Owner re-test
`./build/fdk-files` and look for:
- `icons: theme 'Papirus-Nordic via ...` (detection verdict)
- the sidebar rows now carrying Papirus artwork (colored folder /
  house / drive icons), toolbar buttons the theme's own glyphs
- on quit: `icons: N system hits / M lookups` (N > 0 = the desktop
  theme is being served)
- if a WARN about a missing index.theme appears, the theme name is
  dangling — install it or fix the setting it came from.

# §31 continuation — UPDATE (owner round 2: "still not used" + unclosable Help window + the 2.4 MB ASAN leak)

## Remote proof
origin/main == `ea020af` ("icons+app: the unclosable Shortcuts window, the 2.4 MB icon-surface leak, and the one-line detection verdict").

## The three findings, each with its broken assumption
1. **The unclosable Shortcuts window** — fdk_event.h says FDK does NOT
   destroy on close request (the app decides); menus.c believed a
   comment claiming "auto-managed". Now sc_list_window_event destroys
   on CLOSE_REQUEST or Escape (the props_close pattern). This was also
   the "shutdown with 1 window(s) still open" WARN.
2. **The 2.4 MB icon leak** (owner ASAN: 925 surfaces via icons.c:190/193)
   — fdk_icon_theme_surface() returns a FRESH OWNED CLONE per call;
   the app treated it as borrowed. The app cache now holds system hits
   under "S|name|px" and frees them at teardown. Self-quit under
   detect_leaks=1: ZERO FDK leaks (only libGLX_mesa's 108 KB one-time
   globals — inside libGL, not ours).
3. **"Still not used"** — the resolver WAS working on his machine (925
   system surfaces allocated); the verdict was invisible because
   FDK_ICONS_TRACE was not set. The chain now logs ONE INFO line:
   `icons: theme '<name>' via <channel>` on first detection. ALSO:
   the symbolic tint bakes the color theme's ink at decode time and
   the surface cache never expired on a color-theme switch —
   install/uninstall now call fdk__icon_theme_retheme() (surface-cache-
   only reset; detection/lookup/name caches stay).

## Owner re-test (one command each)
- `./build/fdk-files` → the log must now contain
  `icons: theme '…' via …` (which theme, which channel).
- Help > Shortcuts → the WM close button / Alt+F4 / Escape all close it.
- Quit under ASAN → no LeakSanitizer block (or only the libGLX_mesa
  108 KB system-library globals).

## Rig notes
- /tmp/shotapp.sh now honors RIG_ASAN (default detect_leaks=0), RIG_FRAMES,
  RIG_WAIT, FDK_ICONS_TRACE, RIG_DATA. THE LEAK GATE IS detect_leaks=1 +
  A SELF-QUIT (small FDK_TESTAPP_FRAMES) — a rig-timeout kill hides
  nothing anymore but the self-quit is the proof.
- The remaining non-FDK leak (libGLX_mesa globals) is expected on this
  image; note it in reports so it is not chased as ours.

# §31 continuation — UPDATE (owner round: "icon theme not working at all" + sidebar.c:344 warning)

## Remote proof
origin/main == `238ccc4` ("icons: the THREE real reasons the toolbar was blank on a real desktop").

## What the owner's report exposed (all fixed in 238ccc4)
1. **-Werror had NOT actually landed** in test-apps/Makefile — the
   scripted edit silently missed; the owner's build line proved it.
   Really applied now; a canary (injected unused function) FAILS the
   build. The rig now re-greps the MAKE OUTPUT, not the file, for
   "warning:".
2. **sidebar.c:344 -Wformat-truncation**: newer gcc computes the
   array-SPANNING bound of g_bms[i].path. All fixed-buffer copies in
   sidebar.c go through ta_copy/ta_join now.
3. **The icon claim was FALSE, and here is why my verification lied**:
   - openbox was silently EXITING ("Unable to load a theme") in the
     rig — every "openbox" run was BARE Xvfb. It now gets a working
     Clearlooks theme in a fake HOME; EWMH verified up first.
   - The "13 clusters" in the old screenshots were menubar TEXT, not
     icons: FDK_ICONS_TRACE printed ZERO lines because
     icons_get was NEVER CALLED — the icon button's canvas child had
     EMPTY bounds (sized only in ta_icon_buttons_relayout, which
     runs on FDK_EVENT_WINDOW_CONFIGURE, an event that never arrives
     after the callback is registered) → paint_rec early-outs on
     rect_empty → THE BLANK TOOLBAR (S1's true root cause). Fixed:
     bounds set AT CREATION (the button's natural 32x28) + one boot
     relayout pass after menus_build.
   - The BUNDLED tier never rendered either: the embedded SVGs went
     to fdk_surface_create_from_image_bytes (stbi = raster only) →
     "image decode failed" WARNs. FDK gained the public API
     fdk_surface_create_from_svg_bytes() (nanosvg core factored from
     fdk__icon_svg_raster; test-first tests/test_svg_bytes.c;
     exports 995→996); icons.c routes SVG bytes to it.
   - Fallback-name chain added per Part 2.2 (view-hidden→visibility,
     view-compact→view-list-compact/view-list-details, view-grid→
     view-list-icons, system-search→edit-find, user-trash-full→
     user-trash, folder-*→folder): themes legitimately lack names
     (Adwaita has NO view-hidden/view-compact at any size; breeze has
     no view-list).

## The honest verification now (rerun these commands after any change)
- Rig = /tmp/shotapp.sh (recreate after /tmp wipe; it needs openbox
  debs in /tmp/iconsroot/root with a WORKING theme in fake HOME,
  netpbm+x11-apps in /tmp/pbroot, RIG_DATA for the icon dir,
  gtk-3.0/settings.ini in the fake HOME declares the theme — that is
  how a real desktop states it when no XSETTINGS daemon runs).
- Trace gate: `FDK_ICONS_TRACE=1` → expect 12 "icons: PAINT" and
  tier lines; Papirus = 12/12 SYSTEM; breeze = 11 SYSTEM + 1 BUNDLED
  (view-list); Adwaita-only = 10 + 2 (eye, view-compact). NO
  "decode failed" WARNs, no runtime errors.
- Pixel gate: toolbar band at window-top+30 must contain ≥8 runs of
  icon width (8..20 px); pre-fix shots have ZERO. Cross-theme cell
  hashes differ where the themes' glyph art differs.
- make test GREEN; make verify-exports = 996; apps -Werror clean;
  build/sidebar-test GREEN.

## Still open from §31 (unchanged except S10/S8 data point)
The rig logs show a "no D-Bus address resolves" INFO every ~2 s for
the app's lifetime (the notify client's try_connect re-arms; find
what ticks it every 2 s — that is ALSO the S8 idle-frame suspect).
Then: sidebar_view.c, PART-3 toolbar polish (24 px in 36×36 — the
buttons currently paint 16 px icons in 32×28), PART-4 artifacts,
S7 grab, S8 idle, S10 endpoint chain, --diag, PART-8 audit.

# §31 continuation state — FIX fdk-files (round cut; read this first)

## Remote proof
origin/main == `91e2b94` (the §31/PART-5 sidebar model). `git ls-remote origin main` is the authority.

## The real-conditions rig (PART V) — rebuild after a sandbox recycle
/tmp is wiped on recycle. Rebuild:
1. `bash scripts/setup-sandbox.sh` (x11dev, wayland prefix, rigs, hooks).
2. User-space desktop stack (apt-get download + dpkg -x <deb> root):
   openbox libobrender32v5 libobt2v5 libstartup-notification0 libimlib2t64
   libxcb-util1 adwaita-icon-theme papirus-icon-theme breeze-icon-theme
   hicolor-icon-theme → /tmp/iconsroot/root; netpbm libnetpbm11t64
   x11-apps → /tmp/pbroot.
3. Runners (in /home/z/my-project/scripts, survive recycles):
   realui-run.sh = Xvfb 1920x1080x24 + openbox (EWMH) + private
   HOME/XDG_DATA_DIRS with the chosen theme + no D-Bus + UBSAN
   halt_on_error + the log gate. /tmp/shotapp.sh (recreate on
   recycle): same stack, FDK_TESTAPP_FRAMES=10000, sleep 6, xwd -root
   | xwdtopnm | pnmtopng (netpbm from /tmp/pbroot, LD_LIBRARY_PATH
   /tmp/pbroot/usr/lib/x86_64-linux-gnu and
   /tmp/iconsroot/root/usr/lib/x86_64-linux-gnu for openbox).

## DONE and verified this round (commit → what)
- 0b9bed6 — S5 root cause: the first pane "duplicates the active
  tab's dir" but AT BOOT there is no tab → NULL deref; falls back to
  HOME now. Bonus: fsops.c:87 qsort(NULL,0) UB on an EMPTY $HOME
  (guarded, n==0). Log gate clean since (UBSAN halt + WARN scan).
- c028275 — S6: FDK's shortcut parser grew the xkb keysym names
  (question, semicolon, apostrophe, grave, backslash, bracketleft/
  right, equal), Prior/Next, F13–F24, KP_*, and the LITERAL-SYMBOL
  grammar ("Ctrl+?" = Ctrl+Shift+slash, the US shifted-glyph rule,
  documented in-code; trailing "Ctrl++" = the plus key). Test-first;
  272 format round-trips; make test + dbus GREEN.
- be56c5f — S9: fdk-edit's warnings fixed PROPERLY (unsigned-domain
  clamp files.c:259; (void) fixed-signature params; dead
  on_pref_toggle removed). test-apps Makefile: -Werror ON. Verified
  ZERO warnings gcc+clang19 × -O0/1/2 × 4 apps.
- e0b059d + e463248 — S3 / PART-2 core: icon priority INVERTED —
  system theme FIRST (bundled stems → freedesktop standard names,
  -symbolic preferred, comes back tinted), EMBEDDED bundled SVG
  second, glyph floor third. FDK gained fdk_icon_theme_name()/
  source()/set() (the --diag channel report; exports 992→995),
  test-first. Rig proof: 13 toolbar icon clusters under EVERY theme
  (Papirus/Adwaita/breeze/hicolor). Specs webfetched+recorded:
  http://specifications.freedesktop.org/icon-theme/latest/ and
  http://specifications.freedesktop.org/icon-naming/latest/
- 91e2b94 — S4 / PART-5 MODEL half: sidebar.c/.h (XDG places
  exist-checked, Trash IN Places w/ full-empty icon; bookmarks own +
  shared gtk-3.0 format merged; devices from /proc/self/mountinfo
  with the full exclusion table, /run/media + /run/mount exceptions,
  by-label→by-partlabel labels with lexical dot-dot normalization,
  statvfs usage, removable incl. nvme suffix strip, correct icons).
  `make build/sidebar-test` GREEN on the spec's fixture. make test
  GREEN; zero warnings.

## NEXT ACTIONS (exact order)
1. sidebar_view.c — the VIEW half (API in sidebar.h: sb_build/
   sb_refresh/sb_highlight_path/sb_bookmark_add): one scrollable
   column, 220px + splitter + persisted width; 11px dim bold headers
   (12px above except first, 4px below, nothing else); 32px rows,
   20px icon @ pad 8, 8px gap, ellipsized + full tooltip; accent +
   current-folder highlight; Up/Down/Enter; context menus; 3px usage
   bar (5s + on mount change); mountinfo POLL (POLLPRI|POLLERR) →
   sb_refresh. Then DELETE panes.c build_places, wire panes_build →
   sb_build. Geometry test (DRIG-GEO style): 32px pitch, margins.
2. PART-3 toolbar: 24px in 36x36, separators nav|view|fileops|search,
   disabled dimmed Back/Forward/Up, the Icons/Icons+text/Text pref.
3. PART-4 artifacts (S2): search entry's magnifier + conditional
   clear icon — canvas transparent-clear, damage covers the icon,
   pixel test outside-glyph == button background.
4. S7 modal grab bounded retry (MapNotify, ~100ms, WARN only on
   final failure); S8 idle ≤2 frames/2s (8381-frames bug; damage-
   limit caret blink; find the always-invalidate source) + time-to-
   first-frame (~2s pre-GPU-lane); S10 D-Bus endpoint chain (env →
   $XDG_RUNTIME_DIR/bus → X11 root property → ~/.dbus/session-bus,
   log candidates tried, no-bus stays INFO).
5. PART-1 --diag in all 4 apps (uses icons_last_tier +
   fdk_icon_theme_source; idle-frame counter; VIOLATION lines) +
   RUN-ON-YOUR-DESKTOP.md + the log gate (log-allowlist.txt, per-line
   reasons) in the smoke targets.
6. PART-8 widget-tree audit (a11y, tooltips, NONE-tier, contrast
   ≥3:1 per FDK theme × state incl. Solar Flare, clipped labels,
   spacing, 24px minimums, Tab reachability) in smoke + --diag.
7. Only then the §30 backlog (fdk-monitor, fdk-notes, PASS B,
   PRE-1.0 audit, chases).

## Rules unchanged
Master rule; zero warnings (-Werror on); no D-Bus library in FDK;
ta_buf in apps; 8-experiment timebox; identity ONLY
Faded Dream Project <242450780+FemBoyGamerTechGuy@users.noreply.github.com>;
WIP push every step; webfetch specs, record URLs.
