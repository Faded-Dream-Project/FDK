# TESTING-ON-DESKTOP.md — what the owner verifies on real hardware

The sandbox rigs prove a great deal (real dbus-daemons, real
compositors, real pixel readbacks), but some behaviors only exist on
a real desktop with real daemons, real themes, and a human pointer.
Everything below is marked in the roadmap as
**DONE (should work; owner verification pending)** until you have
walked its numbered steps. Each step lists the exact commands, the
trace environment variable, and the output to expect.

Current open items:

1. [notifyd on Wayland (sway / labwc)](#1-notifyd-on-wayland)
2. [notifyd on X11 (Openbox)](#2-notifyd-on-x11)
3. [notifyd against a real incumbent daemon](#3-notifyd-vs-another-daemon)
4. [notifyd with real clients](#4-notifyd-with-real-clients)
5. [trayhost on Wayland (sway / labwc)](#5-trayhost-on-wayland)
6. [trayhost on X11 (Openbox / any EWMH WM)](#6-trayhost-on-x11)
7. [trayhost against real incumbents](#7-trayhost-against-real-incumbents)
8. [the UI font size on your desktop (TASK C)](#8-the-ui-font-size-on-your-desktop-task-c--needs-your-eyes)
9. [the app-facing API against YOUR desktop (TASK B)](#9-the-app-facing-api-against-your-desktop-task-b--needs-your-eyes)
10. [text_demo: complex scripts, bidi, fallback, caret (M-2)](#10-text_demo-complex-scripts-bidi-fallback-caret-m-2--needs-your-eyes)

---

## 1. notifyd on Wayland

Build and run against your compositor session:

```sh
make -C fdk-shell examples
export FDK_NOTIFYD_TRACE=1     # one log line per bus call + placement
./fdk-shell/build/examples/notifyd
```

Expected first lines (stderr):

```
notifyd: serving org.freedesktop.Notifications as :1.NN
```

Send a test notification from another terminal (notify-send, or
gdbus if notify-send is not installed):

```sh
gdbus call --session \
  --dest org.freedesktop.Notifications \
  --object-path /org/freedesktop/Notifications \
  --method org.freedesktop.Notifications.Notify \
  "Manual test" 0 "dialog-information" \
  "A summary" "A body with <b>bold</b> and a <a href='x'>link</a>" \
  "[]" "{}" 4000
```

What to verify (the card appears at the TOP-RIGHT of every output):

- the card renders with your theme's colors and the system font;
  the summary is bold, the body wraps, the link is underlined;
- an icon arrives (notify-send `-i dialog-information` resolves
  through your icon theme; the trace logs the resolution);
- the card expires after ~4 s and the space below the stack is
  clickable desktop — a click in the 8 px gap BETWEEN two cards
  passes through (a click on a card's rounded CORNER does not —
  that is the documented S-4 input-region limit);
- with two cards live, the newer one is on top and the older slid
  down with no flicker when it re-anchored;
- run `swaymsg create_output` (sway): the card re-appears on the
  new output too (the "every output" placement), and unplugging an
  output self-heals the card onto the survivors;
- the trace (`FDK_NOTIFYD_TRACE=1`) prints one `place id=...`
  line per card with anchor, margin, and size.

Known sandbox-verified limits you should NOT see violated: no
protocol errors under sway 1.10/labwc (the rig pins the
configure/ack handshake), no busy loop while idle.

## 2. notifyd on X11

Same binary, from your X11 session (Openbox):

```sh
export FDK_NOTIFYD_TRACE=1
./fdk-shell/build/examples/notifyd
```

Verify with the same gdbus call as above, plus:

- `xprop` on the card window reports `_NET_WM_WINDOW_TYPE_DOCK`,
  `_NET_WM_STATE_ABOVE`, and `_NET_WM_STATE_SKIP_TASKBAR` (the
  EWMH mapping of the overlay layer);
- the card never takes keyboard focus and never appears in the
  taskbar;
- a click on the card body dismisses it (reason 2 in the trace);
  with a `default` action (notify-send always sends one), the click
  also emits ActionInvoked — visible in the trace;
- full-screen windows stay ABOVE the card (the documented EWMH
  ceiling for docks — the README's fullscreen-over-docks note).

## 3. notifyd vs another daemon

With dunst/mako/swaync already running:

```sh
./fdk-shell/build/examples/notifyd
```

Expected: ONE clear line naming the incumbent through its own
GetServerInformation, and a clean exit 0 — e.g.

```
notifyd: org.freedesktop.Notifications is already owned by :1.23
notifyd: ...it is dunst 1.9.2 (vendor knopwob); exiting (--replace to take over)
```

Then the takeover dance:

```sh
./fdk-shell/build/examples/notifyd --replace
```

Expected: notifyd takes the name (if the incumbent allowed
replacement), the incumbent exits on its NameLost, and notifyd
serves. Against an incumbent that REFUSED replacement
(RequestName without ALLOW_REPLACEMENT), expect the one-line
refusal and exit 0. `--wait` queues instead of exiting.

Stop it with SIGINT/SIGTERM — the trace ends with a clean
shutdown (and no sanitizer findings in a debug build).

## 4. notifyd with real clients

The rig already drives dbus-send (libdbus), gdbus (GLib), and
FDK's own client at it. Worth one round with the real world:

- `notify-send` normal, `-u critical` (must NOT auto-expire),
  `-t 0` (must never expire), `-A` actions (buttons must render and
  fire ActionInvoked);
- a real `image-data` sender (e.g. an application screenshot
  notification) — the raw-icon path;
- a burst (`for i in $(seq 1 40); do notify-send "n$i" & done`):
  every call answered, the flood noted once, the daemon alive;
- a volume/brightness OSD applet if you use one.

---

## 5. trayhost on Wayland (sway / labwc)

The StatusNotifier tray host against your compositor session:

```sh
make -C fdk-shell examples
export FDK_TRAYHOST_TRACE=1    # one log line per registration,
                               # property fetch, menu transaction
./fdk-shell/build/examples/trayhost
```

Expected first lines (stderr):

```
trayhost: serving org.kde.StatusNotifierWatcher as :1.xx
trayhost[trace]: self-registered as host: :1.xx
```

1. Run it under your normal session; the floating tray strip
   appears top-right (a rounded pill, `--margin`/`--cell`/
   `--bar-height` to taste).
2. Start real items — `nm-applet`, `blueman-applet`,
   `udiskie -t`, or anything StatusNotifier-speaking. Each
   registration logs:

   ```
   trayhost[trace]: registered :1.yy (KDE bus-name dialect, sender :1.yy)
   trayhost[trace]: GetAll(org.kde.StatusNotifierItem) on :1.yy ...
   trayhost[trace]: bar relayout: N items, WxH at ...
   ```

3. Left-click an icon: the app activates (nm-applet opens its
   menu, etc. — for ItemIsMenu items left-click opens the
   dbusmenu; trace logs `Activate x=... y=...`).
4. Right-click an icon: its dbusmenu opens right-aligned under
   the cell, cascading submenus flip at the screen edge (trace:
   `submenu flip: ...`).
5. Middle-click: SecondaryActivate.
6. Kill an item (e.g. `pkill nm-applet`): it vanishes within a
   second (trace: `item drop: ...`; the strip relayouts).
7. A Passive item fades to ~45% alpha (never disappears);
   NeedsAttention draws the attention pixmap plus the accent
   marker strip under the cell.
8. Start a second tray host (any other panel's tray): one clear
   line, exit 0. `--replace` takes over from hosts that allow
   replacement.

Known honest limits (docs/DECISIONS.md D-007): no tooltip
windows on hover (the data is parsed; nothing renders it), the
IconTheme property answers the override channel or "default",
Activate coordinates are layer-local where the backend has no
global space (Wayland), and menu refreshes mid-open are
discarded (stale-safe) until the next open.

## 6. trayhost on X11 (Openbox / any EWMH WM)

Same binary, DISPLAY set:

```sh
DISPLAY=:0 FDK_TRAYHOST_TRACE=1 \
    ./fdk-shell/build/examples/trayhost
```

1. Same steps 2-8 as above; on X11 the trace additionally logs
   the bar's computed screen origin (`bar relayout: ... at
   (x,y)`), and Activate coordinates are true screen coordinates.
2. With a real WM: the strip is `_NET_WM_WINDOW_TYPE_DOCK` +
   sticky (visible on every workspace — switch and confirm).
3. `xprop` on the strip window: WINDOW_TYPE_DOCK, state STICKY +
   ABOVE (the fdk-shell TOP layer mapping).

## 7. trayhost against real incumbents

- Against an incumbent watcher (e.g. a running panel's tray):
  the one-line decline, exit 0; `--replace` only when the
  incumbent allowed replacement.
- Items that register through a real session bus (not
  dbus-run-session): same round as above — this is the
  deployment shape the rigs approximate.

## 8. the UI font size on your desktop (TASK C — needs your eyes)

The shells and FDK's chrome now scale off one answer: the
theme/settings (`ui.font_size`, `ui.scale`), the desktop's own
statement (XSETTINGS on X11, the portal on Wayland), or the 16 px
default. On your labwc setup the visible change is notifyd /
trayhost rendering at 16 px instead of the old 14.

1. The default (nothing set anywhere):

```sh
DISPLAY=:0 ./fdk-shell/build/examples/notifyd
notify-send "Default size" "should read comfortably at 16 px"
DISPLAY=:0 ./fdk-shell/build/examples/trayhost   # bar taller than before
```

Expected: cards and the tray strip are ~14% larger than the old
builds (font 16 vs the old 14); nothing clipped — summary and
body inside the card, menu rows inside the menu.

2. Each size, explicitly (the rig's XS sweep, by hand):

```sh
for S in 12 16 24 32; do
  DISPLAY=:0 FDK_UI_FONT_SIZE=$S FDK_NOTIFYD_TRACE=1 \
    ./fdk-shell/build/examples/notifyd &
  notify-send "Size $S" "the card grows with the font; nothing clips"
  sleep 4; pkill -f 'examples/notifyd'
done
FDK_TRAYHOST_TRACE=1 FDK_UI_FONT_SIZE=32 \
  DISPLAY=:0 ./fdk-shell/build/examples/trayhost
```

Expected: each daemon's first trace line reads
`ui: font <S>px (k=...) card_w=...` (trayhost: `bar_h=...`) —
the number you asked for; the card/bar/menu grow with it; at 32
px the menu's 10 rows still fit the screen and no label touches
a card edge.

3. The desktop's own statement (the follow-by-default channel):
run your session's settings (labwc + xsettingsd / gsettings
portal) with a different font (`gsettings set
org.gnome.desktop.interface font-name 'Sans 11'` where a portal
exists), restart notifyd, and confirm the trace line follows.
`FDK_UI_FONT_SIZE` beats it; `fdk-set fdk ui.font_size 18` beats
it too (the FDK-settings-wins rule).

4. Trace variable: `FDK_NOTIFYD_TRACE=1` / `FDK_TRAYHOST_TRACE=1`
prints the `ui:` metrics line (the one the rigs parse).

## 9. the app-facing API against YOUR desktop (TASK B — needs your eyes)

The core client (`fdk_notify.h`) and tray item (`fdk_tray.h`)
are proven against in-test mocks; against your real daemon/host
this is the check (build any example against the installed FDK,
or use the rig's probe binary as the item):

1. Your daemon (dunst / mako / the DE's own) — send with actions
   and an image, confirm the action callback and the close
   reason arrive; kill the daemon mid-flight and confirm the
   status moves to NO_DAEMON and back on restart.
2. Your tray host (Waybar / the panel's) — register an item with
   the snippet in docs/EXTENDING.md §15, confirm the icon, the
   tooltip, the menu tree (including radios and a submenu), and
   a live `set_label` (the host re-fetches on LayoutUpdated).
3. Restart the host (or the watcher service) and confirm the
   item re-registers (on_host flips 0 then 1).

## 10. text_demo: complex scripts, bidi, fallback, caret (M-2 — needs your eyes)

The shaping pipeline (HarfBuzz + FriBidi + per-script fontconfig
fallback) is proven headlessly (tests/test_shaping.c: RTL visual
order, mixed bidi, fallback resolution); `examples/text_demo`
puts it IN FRONT OF YOUR EYES without an input method:

```sh
./build/examples/text_demo "مرحبا 漢字" "नमस्ते"
# no arguments: the built-in showcase (Latin / Arabic / CJK /
# Devanagari / one mixed bidi line)
```

Each argument is one frame: a wrap Label (passive layout) and an
Entry prefilled with the string (caret parked at the END of the
buffer), plus the byte/codepoint census in the frame title.

What to verify, per string:

- `مرحبا` (Arabic, RTL): the letters JOIN — one connected word
  (initial/medial/final forms), not five isolated glyphs; the
  caret starts after the last codepoint which is on the LEFT
  (the logical end of an RTL run renders leftmost); clicking
  mid-word snaps the caret between CLUSTERS, and the caret's
  horizontal movement mirrors the visual order (clicking right
  of center lands EARLIER in the buffer).
- `漢字` (CJK): full-width ideographs from the CJK fallback face
  (the default Latin face has no Han coverage — if these render,
  the fallback channel worked); the caret snaps at the glyph
  boundary between the two characters.
- `नमस्ते` (Devanagari): the conjunct + matras shape correctly
  (न म स्ते — the स्ते cluster must render as a ligature with
  the vowel signs ATTACHED, not as separate letters); the caret
  treats each attached cluster as ONE position.
- `مرحبا 漢字` (the mixed line): the Arabic sits RIGHT of the CJK
  (first strong character is Arabic: RTL paragraph base, so
  logical-first renders rightmost); the two scripts come from
  two different faces (fallback mixing inside one line).

The size follows the UI metrics (item 8's answer):
`FDK_UI_FONT_SIZE=24 ./build/examples/text_demo ...` grows the
whole column around the same strings.

CI / no-pointer mode (the rigs use this; exit 0 and a PNG):

```sh
Xvfb :94 -screen 0 1024x768x24 &
DISPLAY=:94 ./build/examples/text_demo "مرحبا 漢字" "नमस्ते" \
    --screenshot out.png
```

Expected: `[screenshot] out.png (720xNNN, 2 blocks)` and a PNG
whose frames hold joined Arabic, wide CJK, and shaped Devanagari
(the sandbox rig verifies ink presence + a contiguous joined
Arabic run; the EYE checks above are yours).

**Status: DONE (should work; owner verification pending).**

## 11. IME against a real input method (M-1 — needs your eyes)

The sandbox has no fcitx5/ibus; the sway rig proved the protocol
both ways with a hand-rolled input-method client, and the XIM
callback wiring is code-reviewed. A real engine on your desktop is
the last witness.

### Wayland, sway + fcitx5

```sh
sudo pacman -S fcitx5 fcitx5-configtool fcitx5-mozc   # or ibus-anthy / your engine
export GTK_IM_MODULE= QT_IM_MODULE= SDL_IM_MODULE=    # leave these unset for other apps
export XMODIFIERS=@im=fcitx
fcitx5 -d --replace
# then run any FDK example with a text field under sway:
./build/examples/controls_demo   # or your own app with an Entry
```

Type in the entry: composition must appear INLINE at the caret
(underlined), revise as you type, and commit whole words. Switch
windows (or close one and focus another) — the IME must KEEP
working (the stale-session self-heal; the compositor never sends
the leave wlroots owes). Check the fcitx5 candidate window opens
near the caret.

### X11, fcitx5 via XIM

```sh
export XMODIFIERS=@im=fcitx
fcitx5 -d --replace
DISPLAY=:0 ./build/examples/controls_demo
```

Same expectations: inline composition (the XIMPreeditCallbacks
path), multi-character commits landing whole. A one-character
commit may also arrive as a normal keypress — that is the
compatibility rule, not a bug.

**Status: DONE (should work; owner verification pending).**
