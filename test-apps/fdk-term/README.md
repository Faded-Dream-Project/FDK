# fdk-term — the FDK terminal emulator

A real, daily-use xterm-compatible terminal built purely on FDK (no
GTK/Qt/SDL/vte). It exists to prove FDK can replace the GTK/Qt stack.

## Features (verified in this sandbox)
- Real PTY job control (forkpty, TIOCSWINSZ resize, login-shell
  option, non-blocking I/O through FDK's fd watch).
- Full xterm escape support: CSI (cursor, erase/insert/delete, DECSTBM
  scroll regions, SGR incl. 256-color + truecolor + faint/inverse/
  strike), OSC 0/2 (title), OSC 7 (cwd), OSC 8 (hyperlinks), OSC 52
  (clipboard set/read), DEC special graphics (G0/G1 + SO/SI), DECSC/
  DECRC, RIS, bracketed paste (2004), alt screen (1049/47/1048),
  application cursor keys (DECCKM) + keypad, mouse reporting X10/1000/
  1002/1003 with SGR (1006) + UTF-8 (1005) encodings, DA1/CPR replies.
- Grid: wide chars (CJK, 2 cells), combining marks (clusters), emoji,
  per-cell deferred colors (scheme switch never rewrites the grid),
  scrollback ring (configurable), resize that keeps content (xterm
  rules: bottom rows stay, top flows to scrollback).
- Selection: character, word (double-click), line (triple), block
  (Alt+drag), copy-on-select, middle-click PRIMARY paste, Shift+
  Ctrl+C/V clipboard.
- URL detection with dotted-underline hover affordance and Ctrl+click
  open (via fdk_desktop_open_uri).
- Tabs (notebook, drag reorder, Ctrl+PgUp/PgDn), splits (Ctrl+Shift+O
  horizontal / E vertical, Paned), detach tab to a new window.
- Profiles through FDK prefs (fdk-prefs CLI configurable): font size,
  color scheme (theme-following / solarized-dark/light / gruvbox-dark
  / contrast), shell, working dir, login shell, scrollback lines,
  copy-on-select, audible+visual bell, background-finish notification,
  background opacity (X11 _NET_WM_WINDOW_OPACITY via fdk_window_set_
  opacity; Wayland: honest unsupported), cursor blink.
- Live zoom (Ctrl+plus/minus/0), clear screen+scrollback, find-in-
  scrollback bar (Ctrl+Shift+F, wrapping, scroll-to-match).
- Bell: audible (fdk_window_bell/XBell on X11) + visual flash + tab
  unread marks when unfocused.
- HiDPI: cell metrics from the font; window scale respected by the
  icon/theme layers. Session restore: window count + grid sizes.

## Keyboard shortcuts
Ctrl+Shift+T new tab · Ctrl+Shift+W close tab · Ctrl+Shift+N new
window · Ctrl+Shift+Q close window · Ctrl+Q quit · Ctrl+Shift+O/E
split h/v · Ctrl+Shift+C/V copy/paste · Ctrl+Shift+F find ·
Ctrl+plus/minus/0 zoom · Alt+drag block select · Ctrl+click open URL.

## Tests
- `term_test.c` (18 unit tests, headless, sanitizers): plain text +
  autowrap, controls, SGR colors (named/256/truecolor), EL + scroll
  regions, scrollback flow, alt screen, wide + combining, DEC
  graphics, OSC 0/7/52, modes + DA1/CPR, bracketed paste, selection,
  URL ranges, search, resize, key encodings, SGR mouse.
- Smoke under Xvfb with a fake shell driving every escape family
  (FDK_TERM_DUMP grid assertion).

## Should-work (not verifiable in this sandbox)
- Real vim/tmux/htop/less/ssh sessions (the escape coverage they need
  is unit-tested; the owner's desktop check).
- Wayland run (the code paths are backend-neutral through FDK; no
  compositor here).
- Actual transparency on a compositing manager (the EWMH property is
  written and round-trip tested against Xvfb).

## Known gaps
- Mouse-motion button tracking is approximate without a held-button
  record (1002 vs 1003 precision).
- The search bar overlays the notebook's first row rather than
  inserting a layout slot.
