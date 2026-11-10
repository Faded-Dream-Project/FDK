# TESTING-ON-DESKTOP.md — what the owner verifies on real hardware

The sandbox rigs prove a great deal (real dbus-daemons, real
compositors, real pixel readbacks), but some behaviors only exist on
a real desktop with real daemons, real themes, and a human pointer.
Everything below is marked in the roadmap as
**DONE (should work; owner verification pending)** until you have
walked its numbered steps. Each step lists the exact commands, the
trace environment variable, and the output to expect.

All items are now walked or dispositioned on the owner's hardware —
the verdicts, quotes and dispositions live in
docs/REAL-DESKTOP-SIGNOFF.md (signed 2026-11-09, gate G4). The
sections below remain as the standing verification recipe for every
future release; re-walk them when the hardware or the desktop
changes.

---

## Field results — the owner's 10-item sweep (2026-11-09)

The owner answered the 10-item verification list on their hardware
(X11: Openbox + tint2 + the GTX 1050 Ti proprietary driver; Wayland:
**labwc** — this note re-scopes every "sway" mention). Evidence:
their session logs, quoted where the wording is theirs. This table
is the raw material for REAL-DESKTOP-SIGNOFF.md (gate G4).

| # | item | verdict | evidence |
|---|------|---------|----------|
| 1 | the UI font size (TASK C, §8) | **PASS** | default runs of fdk-notes/fdk-edit; the `FDK_UI_FONT_SIZE` 12→16→24→32 sweep ("the text just got bigger and bigger"); the desktop statement read at boot (`ui-metrics: the portal's font statement is 10 pt (14 px)`, scale 1.00) |
| 2 | the tray against the real bar | **PASS** | the fdk-files runs: docked to tint2 via the systray manager + the System Tray v0.2 fallback, popup menus at the click's position, the live-model row (rounds 48–51) |
| 3 | notifications against the real daemon | **PASS** | the dunst session delivered; the daemon-less sessions degraded correctly (and prompted round 51's About dialog, which no longer needs a daemon at all) |
| 4 | multi-monitor hotplug (§12) | **BLOCKED by hardware** | the rig has a single HDMI-0 output — untestable there |
| 5 | the Wayland compositor walk | **RE-SCOPED, pending** | the owner's compositor is labwc, not sway; the §1–7 walkthrough applies verbatim on labwc |
| 6 | drag icons (§14) | **PASS** | "I can drag icons on fdk-files and it follows the cursor" |
| 7 | portals (§15) | **CLARIFIED — confirmed where it applies** | FDK ships no portal file chooser (a documented PARITY out — not an FDK surface); FDK's portal check is the settings/font statement, and it reads correctly on every boot |
| 8 | Orca / AT-SPI (§16) | **PASS** (second pass, 2026-11-09) | the owner ran a real screen reader against the test apps: "Tested orca with fdk and it works" (loud enough to complain about — speech and focus announcements live over the level-1 bridge) |
| 9 | touch (§26) | **N/A by hardware** | no touch device; the probe behaves correctly in the field (`X Input 2.2: touch events available`, `no stylus evidence among 5 pointer device(s)`) |
| 10 | the GL/GPU lane (§27) | **PASS** | `GPU present lane active (NVIDIA GeForce GTX 1050 Ti)` and the small-surface CPU path both observed in the field; the owner confirmed enable/disable |

**Steps for #8 (Orca, ~5 minutes):**

```sh
sudo apt install orca accerciser      # or the distro equivalent
DISPLAY=:0 orca --replace &           # speech + the screen reader
DISPLAY=:0 accerciser &               # the silent AT-SPI tree browser
DISPLAY=:0 ./test-apps/build/fdk-edit
```

Tab through the editor: Orca announces the window, focus moves and
roles (text, menu, button). In accerciser, the FDK tree shows
roles/names/states live under `application: org.fdk.testapps.edit`
— that is the level-1 bridge doing its job, and enough to sign §16
even without audio.

**#7 — the portal half, resolved (2026-11-09, second pass).** The
owner ran the gsettings test on the X11 desktop: librewolf (GTK)
followed the new font, FDK stayed at the 10 pt statement its portal
serves. Diagnosis, with the follow proven in-repo the same day:
`gsettings set` writes dconf, and GTK apps read dconf directly —
that is the control group. FDK's integration channel is the desktop
STATEMENT (XSETTINGS `Gtk/FontName` on X11 when served, the
Settings portal `ReadOne` everywhere); the statement only follows
dconf when the portal routes Settings to a dconf-backed backend
(`xdg-desktop-portal-gtk`). The rig's portal answers 10 pt from its
own source, so the bridge is missing — a desktop-side routing gap,
not an FDK bug. Proof (scripts/portal-mock.py, the real D-Bus
interface on a session bus): the portal serving `Sans 11` boots the
chrome at "11 pt (15 px)"; flipping the statement to `Sans 14`
moves it to "14 pt (19 px)". The channel follows whatever serves
it, by design.

The one-line check on any desktop — what does the portal serve
RIGHT NOW?:

```sh
busctl --user call org.freedesktop.portal.Desktop \
  /org/freedesktop/portal/desktop \
  org.freedesktop.portal.Settings ReadOne ss \
  org.gnome.desktop.interface font-name
```

If it disagrees with `gsettings get
org.gnome.desktop.interface font-name`, the backend is not
dconf-backed: install `xdg-desktop-portal-gtk` and route Settings
to it (`~/.config/xdg-desktop-portal/portals.conf`:
`[preferred]` / `org.freedesktop.impl.portal.Settings=gtk`), then
restart the app — the statement follows gsettings on both X11 and
labwc. Until then, FDK's own knobs reach it with no portal at all
(`FDK_UI_FONT_SIZE`, `fdk-set fdk ui.font_size N` — the wins rule,
field-proven in the round-53 sweep).

**Addendum — the second pass, same day (2026-11-09).** The owner's
"task 18" is the walkthrough's open item 3 (M-2): the text_demo
complex-script exercise, run twice on their fonts —
`text_demo "مرحبا 漢字" "नमस<094d>त<0947>"` and
`text_demo "مرحبا" $'\u0928\u092e\u0938\u094d\u0924\u0947'`
(नमस्ते with the explicit virama). Both logs show the full
per-script fallback chain firing (Arabic → DejaVu Sans, Han →
Noto Sans CJK, Devanagari → Noto Sans Devanagari) under the active
harfbuzz + fribidi pipeline, and both sessions exited cleanly —
**M-2 PASS**. The GL run (`fdk-gl.log`) adds a third field
observation of the GPU lane self-check passing on the GTX 1050 Ti
(§27). **G4's remaining delta after this pass:** the labwc walk
(item 5), the portal font-follow on labwc (the #7 half above), and
the signoff document itself; hotplug (#4) stays hardware-blocked
on the single-output rig.

---

## 1. the UI font size on your desktop (TASK C — needs your eyes)

FDK's chrome now scales off one answer: the
theme/settings (`ui.font_size`, `ui.scale`), the desktop's own
statement (XSETTINGS on X11, the portal on Wayland), or the 16 px
default. On your desktop the visible change is any FDK application
rendering its chrome at 16 px-derived sizes instead of the old 14
(the test apps under `test-apps/build/` are real FDK applications —
use them as the subjects).

1. The default (nothing set anywhere):

```sh
DISPLAY=:0 ./test-apps/build/fdk-notes
DISPLAY=:0 ./test-apps/build/fdk-edit
```

Expected: menus, dialogs, statusbar and tooltips read comfortably
at the 16 px-derived sizes (about 14% larger than the old 14 px
baseline); nothing clipped — menu rows inside their menus, prompt
text inside its dialog.

2. Each size, explicitly (the rig's XS sweep, by hand):

```sh
for S in 12 16 24 32; do
  FDK_UI_FONT_SIZE=$S DISPLAY=:0 timeout 4 ./test-apps/build/fdk-notes
done
FDK_UI_FONT_SIZE=32 DISPLAY=:0 ./test-apps/build/fdk-edit
```

(`timeout` closes each instance before the next opens — the earlier
`… & sleep 4; kill %1` form depends on the shell's job control and
left every instance alive in shells without it, piling up windows.
Since round 53 the TERM lands as a clean exit, so `timeout` also
exercises the graceful shutdown on the way past.)

Expected: every app's chrome grows with the number you asked for;
at 32 px the menus' rows still fit the screen and no label touches
a dialog edge; the text editors' bodies reflow under the larger
metrics.

3. The desktop's own statement (the follow-by-default channel):
run your session's settings (labwc + xsettingsd / gsettings
portal) with a different font (`gsettings set
org.gnome.desktop.interface font-name 'Sans 11'` where a portal
exists), restart the app, and confirm the chrome follows.
`FDK_UI_FONT_SIZE` beats it; `fdk-set fdk ui.font_size 18` beats
it too (the FDK-settings-wins rule).

## 2. the app-facing API against YOUR desktop (TASK B — needs your eyes)

The core client (`fdk_notify.h`) and tray item (`fdk_tray.h`)
are proven against in-test mocks; against your real daemon/host
this is the check (build any example against the installed FDK,
or use the rig's probe binary as the item):

1. Your daemon (dunst / mako / the DE's own) — send with actions
   and an image, confirm the action callback and the close
   reason arrive; kill the daemon mid-flight and confirm the
   status moves to NO_DAEMON and back on restart.
2. Your tray host (Waybar / the panel's) — register an item with
   the snippet in docs/EXTENDING.md §7, confirm the icon, the
   tooltip, the menu tree (including radios and a submenu), and
   a live `set_label` (the host re-fetches on LayoutUpdated).
3. Restart the host (or the watcher service) and confirm the
   item re-registers (on_host flips 0 then 1).
4. Classic X11 bars (tint2's legacy systray, trayer, stalonetray):
   with NO SNI watcher on the bus the item docks through the older
   System Tray v0.2 protocol instead (round 48) — set pixels with
   fdk_tray_item_set_icon_pixmap, or just give it an icon name
   (round 49: the theme engine renders it), and the icon appears.
   Attach an fdk_tray_menu with fdk_tray_item_set_menu and a click
   on the icon opens it as a dropdown (round 50: the toolkit pops
   the same model locally that a StatusNotifier host would render); kill and
   restart the bar and confirm the unhost → re-dock. (SNI still
   wins the moment a watcher appears.)

## 3. text_demo: complex scripts, bidi, fallback, caret (M-2 — needs your eyes)

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

## 4. IME against a real input method (M-1 — needs your eyes)

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

## 5. Activation + startup notification on your desktop (S-6 — needs your eyes)

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
  no focus steal. (Taskbar/launcher integration is the shell
  add-ons' consumer of this.)
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

## 6. Drag icons + widget drop targets on your desktop (S-9 — needs your eyes)

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

## 7. The portal FileChooser on your desktop (C-1 — needs your eyes)

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

## 8. The AT-SPI bridge against a real screen reader (S-8 — needs your eyes)

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

## 9. Gestures on your touchscreen (C-6 — needs your fingers)

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

## 10. The Wayland GPU present lane on your hardware (D-1 / B-15 — needs your GPU)

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

## 11. Subsurfaces on your desktop (D-2 / B-16 — needs your eyes)

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

## 12. Your drawing tablet (D-3 / B-17 — needs your pen)

What the rigs proved: the full event alphabet through the test
seams, the Wayland frame batching, distinct simultaneous tool ids,
the pressure normalization — AND, since the 1.0.0-rc2 crash fix, the
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
(the 1.0.0-rc2 log lines below are the tell).

**The 1.0.0-rc2 log contract — what startup must say.** On a machine
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
  move it, click, type. The pre-1.0.0-rc2 build died on exactly that
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

## 13. The foreign toplevel list on your desktop (D-4 — needs your apps)

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

## 14. Session save/restore on your desktop (D-5 / B-19 — needs your
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

## 15. Idle inhibition on your desktop (S-3 / B-11 — needs your
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

## 16. The cursor shape set on your desktop (S-5 / B-12 — needs your
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

## 17. Touch basics on your touchscreen (M-3 / B-03 — needs your
fingers)

The engine (tap→click, two-finger scroll, the slot model) is pinned
headless by tests/test_touch.c and both backends' translation paths
run through the ops seam; §9 covers the gestures above it. What
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

## 18. The GTX 1050 Ti acceptance: the GL lane under the proprietary
NVIDIA driver on labwc (the §11 report's hardware — needs your GPU)

**Status: implemented — should work (untested on real hardware).**
The §11 crash (two presenters on one wl_surface) is fixed and pinned
on the sway/llvmpipe rig by the attach-count assertion; the §12 round
then made the lane pick its upload shape by PROBING the driver (a
real 4x4 upload per shape, both BGRA internalformat spellings tried —
the spec row first) instead of trusting the extension string, added a
retry ladder (a failed present rotates to the next shape and retries
once per shape before any permanent downgrade), and fixed a real
every-driver bug (the quad program lived in the first window's
context namespace; every later window silently downgraded). What that
rig CANNOT show is the explicit-sync role itself (no DRM render node
in the sandbox) or a driver that rejects the mixed BGRA pair. This is
the recipe for confirming the whole chain on your machine.

First, the trace — one command, everything the lane knows:

```
FDK_GL_TRACE=1 ./build/examples/text_demo "test" 2>&1 | tee fdk-gl.log
```

The trace logs the engagement dump (EGL vendor/version/client APIs,
the chosen config's attributes, the context's API+version, the GL
vendor/renderer/version, the upload-extension flags, the
wl_egl_window's attached size) and every GL/EGL call of the first
presents with each call's result. Then run the real text demo:

```
./build/examples/text_demo "مرحبا" $'\u0928\u092e\u0938\u094d\u0924\u0947'
```

and look for, in order:

- `GPU present lane active on Wayland (renderer "NVIDIA GeForce
  GTX 1050 Ti/PCIe/SSE2")` — the lane engaged.
- `GL lane: compositor advertises wp_linux_drm_syncobj_manager_v1 —
  ...` — the relevance note (ONE line, at connect, plus the §11 note
  at engagement).
- `GPU lane upload path: <shape> — driver-probed (a real 4x4 upload
  picked this shape; ...)` — **THE PROBE'S VERDICT**. If this says
  `BGRA + UNPACK_ROW_LENGTH` with `BGRA internalformat=GL_BGRA_EXT`,
  the strict-spelling fix alone closed your report; if it says a
  TWIN shape, your driver rejected every BGRA spelling and the lane
  stayed on the GPU through the pure-core fallback — either way the
  window keeps hardware presentation.
- **THE ACCEPTANCE**: NO `GL error ... at <call>` WARN line (any
  lane error now names the call, the error code as a name, the
  shape, the texture format, the rect and the stage — if one ever
  appears, that single line is the bug report), NO "GPU present
  ladder ... rotating" line (the ladder fired and recovered — send
  the trace), NO "reverting this window to the CPU present path",
  NO `backend dispatch_pending failed (-101)`, and
  `FDK_RENDER_PATH_GL` persists on every window across several
  minutes of resizing, moving and typing into text_demo.
- **A resize after minutes of run**: resize the window and keep
  typing — the first present at the new size re-runs the per-call
  attribution (a resize-time failure names its call exactly like
  the first-present one).
- If the lane ever DOES fail on your machine, the dead-connection
  line names the interface, object id and protocol error code
  (`wl_display_get_protocol_error`) plus errno — the line the old
  build silently omitted. Send that line plus fdk-gl.log; the trace
  correlates 1:1 with the compositor's own log.

Optional, decisive variant: `WAYLAND_DEBUG=1` the same run and
count `wl_surface.attach` on the toplevel across a handful of
presents — every attach should come from the driver's queue (a
`{... egl ...}` queue name), and NONE from libwayland's
`{Default Queue}`. That count is exactly what the sandbox rig
asserts.

A clean exit under the sanitizers: pass
`LSAN_OPTIONS=suppressions=tests/data/lsan-gl.supp` so the
third-party modules stay quiet while every FDK allocation remains
accounted. On your machine that is TWO known NVIDIA-side exit
reports, both suppressed with proof and BOTH fully explained: the
driver's `libdbus-1` one-time state (670-766 B in 3 allocations —
184 B direct at libdbus+0x276d7, 582 B indirect at +0x3774d) and
`libEGL_nvidia`'s state (256 B in 4 allocations, every frame
inside `libEGL_nvidia.so.0+0xc94e1`). The §13 round audited FDK's
teardown call-by-call before suppressing and fixed the one missing
canonical call (eglReleaseThread, now the connection shutdown's
tail). Your two post-fix runs then closed the last question from
opposite sides: the UNSUPPRESSED run totaled 1022 B in 7
allocations = the 766 B libdbus block plus the SAME 256 B, while
the sign-off run below (suppression active) printed a footer
naming libdbus-1 alone — so the 256 B is INTERMITTENT run-to-run
on this driver, the entry is there for the runs where it appears,
and the fix stays because it is the canonical sequence (and
Mesa-side runs leak zero bytes without it). Your LD_DEBUG=files
trace also NAMED the libdbus puller:
`libnvidia-eglcore.so.580.196.07` (its "dynamically loaded by
libasan.so.8" line is the ASan dlopen-interceptor artifact, and
the driver's failed non-handle dlsym of dbus_bus_get carries the
requester's path; confirmed against the driver binary itself —
libnvidia-eglcore dlopens "libdbus-1.so.3" and dlsyms the full
dbus API with no DT_NEEDED, and the GLX twin libnvidia-glcore
carries the same client). The suppression file's proof blocks
carry the full stories. If an FDK frame appears in any leak
report, that is a real bug — file it; the negative control in the
§13 record proves the suppressions cannot hide one.

THE SIGN-OFF RUN (2026-11-01, GTX 1050 Ti, this recipe on
text_demo): "exited cleanly after 587 frames", and LSan ended the
run with exactly this footer:

    Suppressions used:
      count      bytes template
          3        754 libdbus-1

Read that footer as the run's own audit, not decoration: LSan
lists ONLY the templates that actually matched (a silent entry
matched nothing this run), and the count/bytes are the suppressed
allocations summed across direct and indirect classes (verified
directly with a controlled probe; see docs/testing.md's
"Reading the Suppressions-used footer"). So this run retired
libdbus-1's 3-allocation block and NOTHING ELSE — no
libEGL_nvidia row (that block did not appear this run), no leak
report above the footer, exit 0. That is the §13 clean-exit
contract delivered on the driver desktop: the full stack (GL
lane engaged, driver-probed upload path, HarfBuzz+FriBidi,
atspi) runs, exits cleanly, and the footer accounts for every
byte LSan would otherwise have reported. A future run that
prints a different template set or a leak report above the
footer is telling you something changed — read it the same way.

## 19. The Wayland/NVIDIA sign-off round (the desktop's answer, not
the rig's)

The §18 recipe is the single-window acceptance; this is the
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
  is expected to name third-party modules — on the NVIDIA chain
  specifically the two known ones (libdbus-1's one-time state and
  libEGL_nvidia's per-thread 256 B; both suppressed with proof in
  `tests/data/lsan-gl.supp`, see §18's clean-exit note); FDK-owned
  allocations report zero. If an FDK frame appears in the report,
  that is a real bug — file it.
