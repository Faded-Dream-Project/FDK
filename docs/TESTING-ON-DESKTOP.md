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
