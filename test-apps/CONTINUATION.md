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
