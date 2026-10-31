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
whose frames hold joined Arabic, wide CJK, and shaped Devanagari.
The sandbox rig that proves this mechanically is
`make test-textdemo` (tests/textdemo_rig.sh): it screenshots the
same strings in the shaped and the legacy (`$FDK_SHAPING=0`)
posture and asserts the differential (a broken fallback channel
renders .notdef in both — the delta collapses), the Devanagari
shirorekha width anchor, the Han ideograph density, and a legacy
negative control. The glyph-level truth (fallback face resolved,
no .notdef glyph ids, contextual joining forms, conjunct cluster
merging) is tests/test_shaping.c's six-script matrix, which runs
in plain `make test`. The EYE checks above are still yours.

**The 2026-10-31 fix this section's first report produced**: your
"Arabic and Devanagari as .notdef while Han worked, no fallback
log line" was an unsorted script-classification table — the
binary search silently classified most scripts as "common", so the
fallback was never requested (Han survived by search-path luck).
The table is sorted now, failures WARN with their reason, and the
chosen fallback face is INFO-logged per script — if you ever see
tofu again, the log now names the script, the codepoint, and
whether fontconfig had no answer, the face failed to load, or the
face did not map the codepoint.

**Status: DONE (should work; owner verification pending — re-run
after the 2026-10-31 fallback fix).**

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

## 12. Multi-monitor X11 (M-6 — needs your real monitors)

The headless rigs pin the single-monitor identity and the origin
math (a synthetic two-monitor table through the internal test
seam); what only your desktop can prove is REAL output hotplug and
a WM's per-monitor strut interpretation.

```sh
# What the toolkit sees (real connector names, per-CRTC geometry):
xrandr                       # compare against:
fdk-shell/build/examples/bar --anchor tr          # output 0
fdk-shell/build/examples/bar --anchor tr --output 1
```

Expectations:

- `bar` with `--output 1` docks against THAT monitor's top-right
  corner (its own edges, not the screen's).
- `xrandr --output <second> --off` fires the REMOVED output event
  (the bar's logs); `--auto` re-adds it with ADDED. A layer on the
  removed output is the app's business — the enumeration keeps the
  gone output's record until shutdown.
- A bar with an exclusive zone on a SECONDARY monitor: how much
  space it reserves is the WM's per-monitor strut interpretation
  (modern WMs clamp to the window's monitor; simpler ones reserve
  screen-wide) — the documented EWMH limit, not a bug.
- Maximize/fullscreen (bare-X, no WM path): fill the monitor the
  window is ON. Under a real EWMH WM the WM owns the outcome —
  compare against what it does for other clients.

**Status: DONE (should work; owner verification pending).**

## 13. Activation + startup notification on your desktop (S-6 — needs your eyes)

What the headless rigs proved: the xdg-activation round trip (mint
through the private-queue roundtrip, bogus voucher REFUSED, the full
two-process launcher flow with a held-button serial — sway flipped
focus to the child through the voucher), the X11 SN wire (the
chunked "new:"/"remove:" root messages, the _NET_STARTUP_ID
property, read-once env capture), and the bare-X raise+focus. What
only your desktop can prove is the REAL compositor's and REAL WM's
policy layer — focus-stealing prevention is a policy, not a
mechanism.

```sh
# Wayland (labwc/sway): does a notification click jump you to the app?
# (any FDK app calling fdk_window_activate on itself after user input)
examples/build/03_windows   # then, from another terminal:
swaymsg -t get_tree | jq -r '..|select(.focused? == true).name'
```

Expectations, Wayland:

- An FDK app calling `fdk_window_activate()` after the user clicked
  IN it: focus arrives (the serial vouches).
- The launcher flow — your launcher mints with
  `fdk_window_activation_token()` at button-press, the child spends
  `XDG_ACTIVATION_TOKEN`: the child's first window arrives focused,
  no focus steal. (Taskbar/launcher integration is the fdk-shell
  consumer of this.)
- A stale/bogus token: refused, focus unchanged — that refusal is
  the feature.
- labwc's `focus_on_window_activation` setting (smart/urgent/focus/
  none) governs the outcome for UNvouched requests; urgent (the
  taskbar flash) is the common default.

Expectations, X11 (Openbox):

- `fdk_window_activate()` sends `_NET_ACTIVE_WINDOW` with the last
  user-event timestamp; Openbox honors it for a recently-active app
  and may flash the taskbar otherwise (its own steal policy —
  `xdg_activation` semantics do not exist on X11; the timestamp IS
  the mechanism).
- Launching an FDK app from a launcher that sets
  `DESKTOP_STARTUP_ID`: the app's window carries `_NET_STARTUP_ID`,
  and Openbox's startup-feedback (the launch bubble/cursor) ends at
  the window's first map (the "remove:" state) instead of timing
  out — the visible half of the protocol on a real WM.

**Status: DONE (should work; owner verification pending).**

## 14. Drag icons + widget drop targets on your desktop (S-9 — needs your eyes)

What the rigs proved: the icon window's geometry/shape/tracking/
destruction through a real X11 drag, the per-position acceptance
(XDND Status + wl_data_offer.accept re-evaluated per motion), the
scoped widget delivery, and — on sway — the FDK-source drag with
icon end to end, including the same-connection payload fix. What
only your desktop shows is the REAL compositor's icon compositing
and a real WM's take on the override-redirect icon window.

```sh
# Any FDK app dragging with an icon (fdk_drag_begin_ex): watch the
# icon follow the cursor — on X11 it is a real window (xwininfo can
# find it), on Wayland compositor-blended.
```

Expectations:

- The icon tracks the pointer at the up-right hotspot offset (the
  GTK convention), never lags perceptibly, and vanishes the instant
  the drag ends (drop OR cancel — Esc too).
- Rounded/transparent icons are SHAPED (no black box) on X11; on
  Wayland the alpha blends against whatever is under it.
- A widget registered via `fdk_widget_set_drop_target` gets drops
  only within its bounds; the source's cursor feedback changes as
  the pointer crosses between the zone and the rest of the window
  (per-position acceptance) — that flip is the feature.
- Your WM may or may not keep the X11 icon window above other
  windows (override-redirect stacking is WM-dependent; openbox
  raises it, some WMs leave ordering alone — the icon follows the
  pointer regardless, only z-fighting with a window at the exact
  hotspot would hide it).

**Status: DONE (should work; owner verification pending).**

## 15. The portal FileChooser on your desktop (C-1 — needs your eyes)

What the rig proved: the whole protocol dance against a mock portal
(the call's option shape, the race-free subscription, the Response
decode with percent-encoded URIs) and both fallback postures (auto
mode's refusal -> the own browser; forced mode's refusal -> ERROR).
What only your desktop shows is the REAL portal backend's picker.

```sh
# With xdg-desktop-portal running (most modern sessions):
# any FDK app's file dialog should open the DESKTOP'S picker.
# To compare the toolkit's own browser:
FDK_FILE_DIALOG_PORTAL=0 <your-fdk-app>
```

Expectations:

- The desktop's picker appears (GTK-styled on GNOME, Qt-styled on
  KDE, the file chooser your other apps use) with your title, your
  filters, and — for OPEN kinds — your start_dir as the initial
  folder.
- Accepting files returns POSIX paths exactly like the own browser
  (including percent-decoding); Cancel returns CANCELLED.
- Kill the portal (`systemctl --user stop xdg-desktop-portal`) and
  open a dialog: the toolkit's own browser appears instead — the
  fallback. That resilience is the feature.
- Flatpak-style sandboxes: the picker works where the own browser
  cannot even see the filesystem — the portal's whole point.

**Status: DONE (should work; owner verification pending).**

## 16. The AT-SPI bridge against a real screen reader (S-8 — needs your eyes)

What the rig proved: the whole level-1 surface against a mock stack
(two-bus handshake, discovery, tree enumeration with virtual rows,
roles/states/name/parent, component extents, the focus events, and
DoAction activating a real button). What only your desktop shows is
a REAL screen reader driving a real FDK app.

```sh
# With orca running (Wayland or X11):
#   open any FDK app/example, tab around, listen.
# On X11 you can also inspect with:
busctl --user call org.a11y.Bus /org/a11y/bus org.a11y.Bus GetAddress
# then, on THAT bus, enumerate /org/freedesktop/atspi/accessible/root
```

Expectations:

- Orca announces the window, then widgets as you tab: "Click me,
  push button, not pressed", the labels, "entry" for the text
  fields — names and roles coming from the internal tree's snapshot.
- Activating a focused widget through Orca's "click" fires the
  widget's real callback (DoAction -> the programmatic ACTIVATE).
- Lists/trees announce their rows (the virtual-children export).
- A screen reader started AFTER the app still finds it (the retry
  handshake — the rig's caught bug, fixed as the posture).
- Nothing at all changes on desktops without an accessibility stack
  (the bridge retries silently; the internal narrator still works).

**Status: DONE level 1 (should work; owner verification pending;
text interfaces are C-2's next tier).**

## 17. Runtime re-anchoring + auto-hide on your desktop (C-5 — needs your eyes)

What the rigs proved: the live placement changes on both backends
(geometry/strut readback on X11, configure-through on sway, the role
swap, the auto-hide cycle with a REAL XTEST strip entry). What only
your desktop shows is a real WM's take on a moving dock and a real
compositor's slide.

```sh
# any fdk-shell layer, live:
#   fdk_shell_layer_set_anchor(layer, ...)   # watch it slide
#   fdk_shell_layer_set_auto_hide(layer, 3, 300)  # hover the edge
```

Expectations:

- A top bar re-anchored BOTTOM moves to the floor; other windows
  follow the strut change on their next layout (EWMH WMs re-flow on
  _NET_WM_STRUT_PARTIAL changes — verify your WM does).
- set_level to OVERLAY: the layer jumps above normal windows
  (Wayland: a brief unconfigure/configure blink is legal; X11: no
  blink).
- Auto-hide: the bar slides off leaving 3px; windows maximize into
  the freed space; touching the edge (or the strip) reveals; moving
  away for the dwell hides it again. On X11 your WM must not fight
  the off-screen move (USPosition claims it; some WMs clamp dock
  windows to the screen — a visible clamp is a WM difference to
  report, not an fdk-shell bug).

**Status: DONE (should work; owner verification pending).**

## 18. Gestures on your touchscreen (C-6 — needs your fingers)

What the rig proved: the engine's whole gesture policy headless
(span arithmetic, both swipe directions, every reject, the CANCEL
wipe). What only your touchscreen shows is real hardware's event
cadence.

Expectations:

- Two fingers spreading/pinching over any FDK window delivers
  GESTURE_PINCH with scale > 1 / < 1 at the centroid; a two-finger
  drag scrolls AND pinches simultaneously when the span changes.
- A fast two/three-finger fling delivers GESTURE_SWIPE with the
  compass direction and the finger count; slow lifts, short travel,
  or fingers disagreeing do not.
- The compositor may take some gestures itself (edge swipes) — those
  arrive as TOUCH_CANCEL (nothing synthesized, by design).

**Status: DONE (should work; owner verification pending).**

## 19. The Wayland GPU present lane on your hardware (D-1 / B-15 — needs your GPU)

What the rig proved: the lane's whole state machine against a REAL
compositor (sway headless, through Mesa's software EGL under
FDK_GL_FORCE) — policy, engagement, full + partial-damage presents,
two windows interleaved on one connection, the permanent downgrade
with frame salvage, the self-check's real-failure refusal — plus
the compositor-side bytes: the rig's screenshot smoke read the
GL-lane window's probe colors back byte-exact through grim. What
only your machine shows is a HARDWARE renderer's side of it.

Expectations (run any example or your own app on your Wayland
session — labwc, sway, KDE, GNOME all qualify):

- With a hardware GL driver, a window larger than 512x256 presents
  through EGL automatically — nothing to enable. Verify with the
  log line `GPU present lane active on Wayland (renderer "...")`
  (FDK_LOG_DEBUG), or call `fdk_window_get_render_path` and expect
  `FDK_RENDER_PATH_GL`.
- Pixels are byte-identical to the CPU path — if anything looks
  tinted, ghosted, or transparent, that is a bug (the lane's
  self-check should have caught it; if it did not, report the
  renderer string).
- Small windows (the panel/popup class) stay on wl_shm by design —
  render path CPU there is correct, not a failure.
- `FDK_NO_GL=1` must force wl_shm everywhere; `FDK_GL_FORCE=1`
  must engage the lane even on llvmpipe (useful to compare the
  two paths' output on the same machine).
- NVIDIA's Wayland EGL driver is the interesting case the rig
  cannot cover: if the self-check refuses it you will see one
  warning line and every window silently stays on the CPU path —
  the app must keep working perfectly either way. That graceful
  silence is the contract; a crash or a black window is not.

**Status: DONE (should work; owner verification pending on real
hardware — the rig's coverage is the Mesa swrast path).**

## 20. Subsurfaces on your desktop (D-2 / B-16 — needs your eyes)

What the rigs proved: the X11 child-window half against a real Xvfb
server (the child is a REAL X child — XQueryTree both directions,
server-side geometry and stacking, XTEST clicks routed to the child,
pixel readback of the child compositing over the parent, both
destroy orders behind a counting protocol-error fence) and the
Wayland half against sway headless (injected clicks on the
subsurface with child-local coordinates, moves and resizes proven by
hit-testing the new geometry, the place_below stacking flip, grim
pixel proof of the DESYNC commits, the popup refusal, the parent
sweep). What only your desktop can show is the feature's actual use
— a video region inside a UI, on your compositor and your GPU.

There is no shipped subsurface example yet (the API is add-on and
application surface, not a widget), so the fastest sign-off is a
ten-line snippet inside any example's window — e.g. in
`examples/08_canvas.c` after the window exists:

```c
#include <fdk/fdk_extend.h>
fdk_ext_surface *sub = NULL;
fdk_ext_subsurface_create(win, 40, 40, 200, 150, &sub);
fdk_surface *s = NULL;
fdk_ext_surface_get_surface(sub, &s);
fdk_surface_fill(s, (fdk_color){1, 0, 0, 1});   /* a red region */
fdk_surface_present(s);
/* click inside the red region: your window's callback must NOT see
 * the button — set one on the sub to see it land child-local:
 *   fdk_ext_surface_set_event_callback(sub, cb, NULL);
 */
```

Expectations, both backends (labwc or sway on Wayland; any WM on
X11):

- The red region appears INSIDE the window at (40, 40) — over the
  window's own pixels — without the window repainting.
- Re-filling and re-presenting ONLY the sub updates the region while
  the window's UI underneath stays untouched (the desync contract).
- Clicks inside the region belong to the SUB (child-local
  coordinates); the window's own areas keep their clicks. Keyboard
  input stays with the window — that is the documented contract, not
  a bug.
- `fdk_ext_subsurface_set_position(sub, 100, 80)` + a re-present
  moves the region immediately on both backends (Wayland rides an
  internal applying commit — the API's contract, you do nothing).
- On X11 only: a menu/popup can anchor to the sub. On Wayland that
  call answers `FDK_ERR_UNSUPPORTED` by design — parent menus on the
  window.
- Tearing the window down (or the app quitting) with the sub alive
  is clean — no protocol errors on either backend (the sweep).

## 21. Your drawing tablet (D-3 / B-17 — needs your pen)

What the rigs proved: the full event alphabet through the test
seams, the Wayland frame batching, distinct simultaneous tool ids,
the pressure normalization — AND, since the 1.1.0-rc2 crash fix, the
X11 cookie pipeline three ways: a hostile-cookie rig that feeds
the REAL handler the crash's exact garbage (the owner's desktop
died in it — the mirror was misaligned eight bytes, the constants
were off by one, and a stylus-less machine had classified its
mouse as four tablets), a classification regression over synthetic
devices (your wheel mouse is NOT a tablet, whatever its axis
count), and REAL XTEST cookies end-to-end through the dispatch
loop with a tablet record injected for the XTEST pointer. What
only your hardware shows is a real driver's stream — and on X11,
the honest verdict that no phantom tablets get classified
(the 1.1.0-rc2 log lines below are the tell).

**The 1.1.0-rc2 log contract — what startup must say.** On a machine
with NO tablet (your crash machine, before the fix, said "X Input:
4 tablet device(s) detected"):

```
[x11] X Input: no tablet device (no stylus evidence among N pointer device(s)) — tablet events disabled
```

On a machine WITH a tablet, one line per accepted device, each
with its evidence:

```
[x11] X Input: tablet 'Wacom Intuos S Pen' (id 12): Abs Pressure label — pressure axis 2, tilt 3/4, 5 axes
[x11] X Input: 1 tablet device(s) — stylus events available
```

If a machine with no stylus reports tablet devices, that is a
classification bug — report it with the full startup log and the
output of `xinput list`.

The API is event-only (no example yet), so the fastest sign-off is
a callback on any example's window:

```c
static void evt(fdk_window *w, const fdk_event_data *e, void *u) {
    if (e->type == FDK_EVENT_TABLET_PROXIMITY_IN)
        printf("pen in (type %u)\n", e->tablet.tool_type);
    if (e->type == FDK_EVENT_TABLET_MOTION)
        printf("%.0f,%.0f  pressure %.2f  tilt %.0f/%.0f\n",
               e->tablet.position.x, e->tablet.position.y,
               e->tablet.pressure, e->tablet.tilt_x, e->tablet.tilt_y);
    if (e->type == FDK_EVENT_TABLET_DOWN) printf("tip down\n");
    if (e->type == FDK_EVENT_TABLET_UP)   printf("tip up\n");
}
fdk_window_set_event_callback(win, evt, NULL);
```

Expectations (Wayland: labwc, sway, KDE, GNOME; X11: any WM):

- **First, the crash check**: use the window with the mouse —
  move it, click, type. The pre-1.1.0-rc2 build died on exactly that
  (the misclassified mouse fed the broken valuator reader). The
  fixed build must stay up with zero sanitizer reports, on both a
  stylus-less machine (tablet events simply absent) and a tablet
  machine (the mouse never produces tablet events — only the pen
  does; the streams are additive by design).
- Hovering the pen over the tablet fires PROXIMITY_IN, then
  MOTION streams while the pen is in range — with pressure rising
  from 0 as you press, tilt following the pen's lean.
- Tip contact fires DOWN, lift fires UP (at the last position);
  the side buttons fire BUTTON with the release marked by the high
  bit. A second tool (the eraser end) carries a different tool id
  and tool_type.
- Pressure reads 0..1 (drivers reporting raw 0..65535 units are
  normalized). If pressure looks stuck at 0 or 1, that is a bug
  worth reporting — include the backend and driver.
- On X11 there is no PROXIMITY_OUT (the X Input protocol has no
  proximity for XI2 clients — the events simply stop when the pen
  leaves range). On Wayland PROXIMITY_OUT arrives with zeros.

## 22. The foreign toplevel list on your desktop (D-4 — needs your apps)

What the rigs proved: both halves end to end — the X11 leg as a
full fake-WM round trip (the roster read from real server state,
the activate/close client messages arriving at the "WM"), the sway
section against the live compositor (the client's own windows
listed, activation verified through sway's own tree, the close
REQUEST landing as the window's own close-request event). What
only your desktop shows is the feature's actual job: a real
desktop's window list.

Fastest sign-off (any example, plus a terminal beside it):

```c
#include <fdk/fdk_toplevel.h>
static void tl_changed(fdk_toplevel_list *l, void *u) {
    printf("--- %u toplevels ---\n", fdk_toplevel_count(l));
    for (fdk_u32 i = 0; i < fdk_toplevel_count(l); i++) {
        fdk_toplevel_info info = { .struct_size = sizeof(info) };
        fdk_toplevel_get_info(l, i, &info);
        printf("%u: %-30s %s %s%s%s%s\n", i, info.title, info.app_id,
               info.states & FDK_TOPLEVEL_ACTIVATED ? "[active] " : "",
               info.states & FDK_TOPLEVEL_MAXIMIZED ? "[max] " : "",
               info.states & FDK_TOPLEVEL_MINIMIZED ? "[min] " : "",
               info.states & FDK_TOPLEVEL_FULLSCREEN ? "[full] " : "");
    }
}
/* after fdk_init: */
fdk_toplevel_list *tl = fdk_toplevel_list_open(ctx, tl_changed, NULL);
```

Expectations (Wayland: sway, labwc; X11: any EWMH WM):

- Your terminal and every other window appear with title + app_id
  (X11 shows WM_CLASS — usually the program's generic name). Opening
  and closing windows fires the callback and the list follows.
- `fdk_toplevel_activate(tl, i)` focuses window i (the platform's
  focus-stealing rules apply — on a normal desktop a user-triggered
  click on your taskbar button is exactly the "user action" the
  rules want). `fdk_toplevel_close(tl, i)` asks the window to close
  politely — an unsaved-document app may still ask its user first.
- On X11, a mid-session retitle (renaming a terminal tab) shows up
  at the next window open/close/focus change, not immediately — the
  documented X11 limit (clients own their PropertyNotify). On
  Wayland retitles arrive immediately.
- GNOME (no wlr-foreign-toplevel) answers NULL from open — the
  documented degradation; treat NULL as "no taskbar substrate".

## 23. Session save/restore on your desktop (D-5 / B-19 — needs your
logout)

What the rigs proved: the X11 lane end to end — a second connection
playing the session manager sent WM_SAVE_YOURSELF, the window's
callback received FDK_EVENT_SESSION_SAVE, and the WM_COMMAND
rewrite it demanded came back as the PropertyNotify the protocol
defines as the "state saved" ack (the handler's NEW argv is what
got acked — dispatch-before-ack ordering pinned); the zero-length
reply for a command-less window; the argv round trip. The Wayland
half: no released compositor offers xdg-session-management-v1 yet
(sway 1.10 AND 1.12/wlroots 0.20 were probed live), so the object
lane is seam-proven (the real listener bodies) plus the live
degradation (open answers NULL). What only your desktop shows is
the actual logout/login round.

X11 (openbox + your session manager — xsm, lxsession, or your DE's):

```c
/* once, after fdk_init, on the app's MAIN window: */
const char *argv[] = { "my-app", "--restore", "~/.cache/my.state" };
fdk_window_set_command(win, argv, 3);

/* the save handler — same callback as everything else: */
if (event->type == FDK_EVENT_SESSION_SAVE) {
    save_state();                 /* fdk_prefs_save() + your own files */
    fdk_window_set_command(win, argv, 3);  /* updated restart args */
}
```

- `xprop -id <window> WM_COMMAND` shows the argv, and
  `xprop -id <window> WM_PROTOCOLS` lists WM_SAVE_YOURSELF (on
  every FDK window).
- Log out of a session that saves clients (a classic Xsession /
  xsm flow): the session manager asks FDK apps to save, the ack
  arrives (watch `xev -id <window>` while logging out — the
  PropertyNotify on WM_COMMAND right after the save request), and
  the next login re-executes your WM_COMMAND. Your app starts with
  `--restore ~/.cache/my.state` and puts its windows back.

Wayland (a compositor with xdg-session-management — when yours
grows it; sway/labwc today answer NULL and that IS the documented
degradation):

```c
fdk_session *s = fdk_session_open(ctx, FDK_SESSION_REASON_LAUNCH,
                                  saved_id_or_NULL, on_notify, &app);
/* in on_notify, state FDK_SESSION_STATE_CREATED: persist
 * fdk_session_id(s) (prefs) — that string is the whole round trip. */
fdk_window_join_session(win, s, "main");   /* BEFORE fdk_window_show() */
fdk_window_show(win);
/* FDK_EVENT_SESSION_RESTORED arrives before the first configure
 * when the compositor remembered the window. At exit:
 * fdk_session_destroy(s)  (state preserved for the next launch). */
```

- Relaunch with the persisted id: the notify fires RESTORED, and
  windows joined pre-show come back where the compositor remembers
  them (workspace, stacking, size) — FDK_EVENT_SESSION_RESTORED
  precedes the first configure on each restored window.
- `fdk_session_remove(s)` is the "forget me" call — the next launch
  with the old id mints a fresh session instead.

## 24. Idle inhibition on your desktop (S-3 / B-11 — needs your
screensaver)

The rigs proved both mechanisms: the live D-Bus battery's mock
ScreenSaver (the cookie round trip, the deferred release, the
refusal) and the sway rig's compositor inhibitor (engage/release/
destroy-while-inhibited). What only your desktop shows is the real
blanker actually staying off.

```c
#include <fdk/fdk_idle.h>
/* while a video is playing: */
fdk_idle_inhibitor *inh = NULL;
fdk_idle_inhibit(ctx, win, "my-player", "playing a movie", &inh);
/* check: FDK_IDLE_INHIBIT_ACTIVE — the mechanism the desktop chose */
/* when playback stops: */
fdk_idle_uninhibit(inh);
```

- X11 (any desktop with org.freedesktop.ScreenSaver — GNOME, KDE,
  Xfce, MATE): start a video in your FDK app, wait past your usual
  blank timeout with no input: the screen stays on. Stop playback:
  the next idle times out and blanks as usual.
- Wayland (sway, labwc — zwp_idle_inhibit-manager-v1): same shape;
  sway's `swaymsg -t get_outputs` shows nothing special — the
  compositor simply holds the idle timer.
- No bus and no protocol: the handle answers
  FDK_IDLE_INHIBIT_UNSUPPORTED — honest, never an error loop.
- GNOME's org.gnome.ScreenSaver variant is not probed (documented
  in the B-11 row); on a GNOME X11 session the freedesktop name
  carries it.

## 25. The cursor shape set on your desktop (S-5 / B-12 — needs your
pointer)

The rigs pinned the set both sides (the sway suite's enum-table row
by row; the hermetic RENDER proof on Xvfb; the themed fallback's
XCursor parse). What only your desktop shows is your CURSOR THEME's
own artwork for each shape.

- In any FDK app with entries (the settings example is ideal):
  hover an Entry / TextView / Spin — the I-beam; hover a link-ish
  widget or drag a slider — the hand; resize a window from an edge
  — the directional arrows; plain background — the arrow.
- `fdk_window_set_cursor(win, FDK_CURSOR_TEXT)` (or WAIT, CROSSHAIR,
  GRAB, ...) applies window-wide and survives re-entering the
  window.
- X11 with a cursor theme installed (breeze, Adwaita, whatever your
  desktop sets): the shapes come from YOUR theme via RENDER; the
  handful the core cursor font cannot spell are the documented
  nearest-glyph substitutions (D-016). Wayland with cursor-shape-v1
  (sway, labwc): the compositor's own themed shapes.

## 26. Touch basics on your touchscreen (M-3 / B-03 — needs your
fingers)

The engine (tap→click, two-finger scroll, the slot model) is pinned
headless by tests/test_touch.c and both backends' translation paths
run through the ops seam; §18 covers the gestures above it. What
only your touchscreen shows is the real feel.

- Tap any FDK button: one click (the tap synthesis is the ONLY
  click a touch produces — no double-firing with emulated pointer
  events).
- Two fingers and drag: the ScrollView under your fingers scrolls
  (the page follows the average delta; lift both and it settles).
- A drag that starts as a touch press on a draggable band moves the
  window on X11; on Wayland the compositor takes over.
- Press-drag-release inside a List or Tree selects exactly as the
  mouse does (the same hit tests, no touch-only surprises).

## 27. The GTX 1050 Ti acceptance: the GL lane under the proprietary
NVIDIA driver on labwc (the §20 report's hardware — needs your GPU)

**Status: implemented — should work (untested on real hardware).**
The §20 crash (two presenters on one wl_surface) is fixed and the
fix is pinned on the sway/llvmpipe rig by the attach-count
assertion; what that rig CANNOT show is the explicit-sync role
itself (no DRM render node in the sandbox). This is the recipe for
confirming the whole chain on your machine.

Run `text_demo "مرحبا" $'\u0928\u092e\u0938\u094d\u0924\u0947'`
under labwc and look for, in order:

- `GPU present lane active on Wayland (renderer "NVIDIA GeForce
  GTX 1050 Ti/PCIe/SSE2")` — the lane engaged.
- `compositor advertises wp_linux_drm_syncobj_manager_v1 (explicit
  buffer synchronization) — ...` — your compositor offers the
  explicit-sync protocol (ONE line, at connect).
- `GL lane: compositor advertises wp_linux_drm_syncobj_manager_v1 —
  a driver wrapping this surface in the explicit-sync role makes
  FDK's wl_shm-commit refusal load-bearing (the single-owner rule)`
  — the relevance note at lane engagement.
- `GPU lane upload path: BGRA texture + UNPACK_ROW_LENGTH sub-rects`
  (or the RGBA-twin / packed-row spelling) — the upload contract
  detected your driver's extensions.
- **THE ACCEPTANCE**: `FDK_RENDER_PATH_GL` persists on every window
  (`fdk_window_get_render_path`, or just: no "reverting to the CPU
  present path" WARN), NO `backend dispatch_pending failed (-101)`,
  and NO protocol error across several minutes of resizing, moving
  and typing into text_demo. Before the §20 fix this exact run died
  within the first configure: FDK's own wl_shm background attach hit
  the EGL-wrapped surface without an acquire point — protocol error
  4, dead connection, "exited cleanly after 0 frames".
- If the lane ever DOES fail on your machine, the dead-connection
  line now names the interface, object id and protocol error code
  (`wl_display_get_protocol_error`) plus errno — the line the old
  build silently omitted. Send that line; it correlates 1:1 with
  the compositor's own log.

Optional, decisive variant: `WAYLAND_DEBUG=1` the same run and
count `wl_surface.attach` on the toplevel across a handful of
presents — every attach should come from the driver's queue (a
`{... egl ...}` queue name), and NONE from libwayland's
`{Default Queue}`. That count is exactly what the sandbox rig
asserts.

## 28. The Wayland/NVIDIA sign-off round (the desktop's answer, not
the rig's)

The §27 recipe is the single-window acceptance; this is the
half-hour round that says "the lane is fine on this desktop" as a
property, not an anecdote. All of it on your real session
(labwc/sway + the proprietary driver):

- **Sustained lane**: run 01_hello_world and 04_widgets side by
  side for a few minutes (both float at different sizes — the lane
  policy engages both). No `WARN` lines, no `-101`, both stay
  `FDK_RENDER_PATH_GL`.
- **Resize storm**: drag text_demo's edges for ~30 s (continuous
  configures at many sizes). The resize seam's background-buffer
  refusal is exercised here — the pre-fix tree attached at every
  configure-before-first-present. No protocol errors, no flicker
  beyond the compositor's own frame pacing.
- **Hide/show and destroy**: hide and re-show a window, destroy a
  lane-owned window and open a new one. FDK's queue never attaches
  while the lane owns the surface; the EGL teardown is the lane's
  own (context → surface → wl_egl_window, one MakeCurrent(nil)).
- **The downgrade path** (if you can force it):
  `FDK_GL_FAIL_AT=2 ./build/examples/text_demo …` — the second
  present is poisoned, the window reverts to the CPU path with the
  frame salvaged, and the connection must stay alive (the load-
  bearing order: EGL surface released BEFORE the first wl_shm
  attach).
- **A clean exit**: LeakSanitizer's exit report on a healthy run
  is expected to name third-party modules (NVIDIA/libwayland
  internals); FDK-owned allocations report zero. If an FDK frame
  appears in the report, that is a real bug — file it.
