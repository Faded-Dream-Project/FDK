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
