# ROADMAP-1.1.0 — the gap audit and the proposed release scope

**Status:** IN IMPLEMENTATION (Phase 2, owner-approved). Version is
`-rc2`. Per-item status is marked inline as work lands; the
owner's Phase 2 directive: implement every tier (MUST, SHOULD,
COULD, DEFER), re-audit when done, keep going until nothing
meaningful remains or the sandbox blocks it.

**Base:** `main` @ `7db4a81` (2026-10-26). All verdicts were read out
of the tree at that commit — headers, backends, tests, docs — not
from memory of what was planned.

**Verified against:** `main` @ `ccafa7c` (2026-10-27, owner-directed
verification pass — see the Verified status table below). Verdicts
below that changed since the base are marked inline with
**[VERIFIED-CHANGED]**; the original audit text is kept.

**The rule this audit is judged by** (the project's own): *if an app
needs it and FDK lacks it, that's a bug in FDK.* GTK4 and Qt6 are the
feature references for the core; the wlr-layer-shell spec and
gtk-layer-shell are the references for fdk-shell. Judgment calls are
marked as such.

**How to read the registers:**

- Verdict: **present** (shipped and tested), **partial** (works with
  named limits), **missing** (absent).
- Effort: **S** (a focused slice), **M** (a multi-part slice with
  rigs and tests), **L** (a subsystem, several slices).
- ABI risk: whether closing the gap needs anything beyond FDK's
  append-only discipline (docs/abi-policy.md). Every item below is
  closable append-only; the two version bumps (extension API v2, no
  SONAME change) are the only ABI-adjacent events, and both are
  additive.

---

## Verified status (2026-10-27, @ `ccafa7c`)

Every item was re-judged against the live tree: sources inspected,
the full local battery run (`make test`, `make test-x11` 3x,
`make test-dbus` incl. the libdbus cross-implementation leg,
`make FDK_SANITIZE=0 test` + `test-dbus`, `make FDK_CONFIG=release
all examples verify-exports` at 757 exports, the sway-rig Wayland
integration suite, fdk-shell unit + X11 + Wayland suites, the
bar rig on BOTH sway and labwc, and the prefix-isolation proof).

**Counts** (recounted 2026-10-30 at the D-2 landing against every
register row — the arithmetic is the table's, item for item):
DONE 55 · PARTIAL 1 (B-19) · IN PROGRESS 1 (M-9) · MISSING 0 ·
DEFERRED (by design) 5 · BLOCKED-BY-SANDBOX 0 — of 62 items. The
62 = 13 A + 19 B + 9 M + 10 S + 6 C + 5 D. Per the owner's Phase 2
directive ("DEFER D-1 to D-5: do these too"), the D tier is now the
active queue: ~~D-1 (B-15)~~ **SHIPPED**, ~~D-2 (B-16)~~
**SHIPPED**, D-3 (tablets — B-17's fenced half), D-4
(foreign-toplevel), D-5 (B-19's session-restore half).
Re-verified 2026-10-30 (post-D-2): the dual-backend rebuild zero
warnings; core `make test`; `make test-x11` (the NEW subsurface leg
green alongside the full 201: the second-connection window-tree
truth both XQueryTree directions, geometry+position, over-parent
pixel readback, move/resize + CONFIGURE, sibling restack both ways
through the server's own stack order, REAL XTEST click routed to
the child with child-local coordinates, popup-on-sub, solo destroy +
recursive nested sweep, and the parent sweep behind the counting
protocol-error fence — zero X errors both sweep orders);
`FDK_DBUS_TEST_REQUIRE`-shaped `make test-dbus` (ALL PASS);
`FDK_SANITIZE=0` test; release + examples + verify-exports (855,
+7 — the seven v2 subsurface symbols); the FULL sway suite green
with the NEW subsurface section (injected clicks routed to the
child with child-local coords; the move proven by hit-testing the
NEW origin AND the vacated old region; resize by the far-corner
click; the place_below stacking flip by which surface takes the
click; the popup refusal; nested create; the parent sweep
protocol-clean; grim+PIL pixel readback of the child's DESYNC
commits compositing over the parent — with the pointer parked away,
the sprite was the white pixel that cost a debug session); and
fdk-shell build zero warnings + test-x11 + test-desktop + its
Wayland suite (ALL PASS). The one protocol lesson of the landing is
recorded as DECISIONS D-029: parent-managed tree state, absorbed
into create/set_position/place as the empty applying ancestor
commit.
Re-verified 2026-10-30 (post-D-1): the dual-backend rebuild zero
warnings; the FULL sway suite green WITH the new GL-lane group
(policy matrix incl. the deterministic small-surface gate;
present + health under FDK_GL_FORCE — two windows, one connection,
full + partial damage frames, interleaved presents; the
FDK_GL_FAIL_AT downgrade leg; the SELFCHECK_POISON real-failure leg
with the one-warning-per-connection log contract and the
fresh-connection FORCE override) — all four legs exercising the
REAL lane (the rig's Mesa swrast EGL through libEGL dlopen +
GLVND's vendor ICD); plus the standalone screenshot smoke proving
the compositor-side end: the GL-lane window's red/green/yellow
probes read back byte-exact through grim on the sway headless
output. Core `make test` + `test-x11` re-run green after the
change (the X11 GLX lane untouched by construction — the new code
is Wayland-only files plus the gated engagement block).
Re-verified 2026-10-30 (post-CI-repair — the 15-push red streak's
root causes): S-5's Xrender.h include (a build dep the lane's own
design forbids — every job on every runner failed at that compile
line) removed with the wire types spelled locally; the
PictStandardARGB32 transcription bug it HID (4 = A1, not ARGB32 —
garbage pictures behind async BadMatch, invisible to the non-None
asserts because Xlib mints XIDs client-side) fixed to 0 with the
reference values pinned in the comment and a counting-error-handler
fence + XSync added to the named-cursor leg (fails on ~20 errors
with the bug, zero with the fix — verified both ways); M-6's
libXrandr link dependency declared everywhere it was missed (ci.yml,
the five packaging scripts and metadata families, both deb soname
maps, the fdk-shell packaging set, docs/build.md + packaging.md +
dependencies.md reconciled). Full local battery green after the
repair: the dual-backend rebuild zero warnings, core make test,
FDK_DBUS_TEST_REQUIRE=1 test-dbus (ALL PASS), make test-x11 (205
[ok] incl. the fence — zero protocol errors), FDK_SANITIZE=0 test,
release + examples + verify-exports (848, unchanged), the sway suite
(56 [ok]), fdk-shell build + test-x11 + test-desktop + isolation +
the notifyd and trayhost rigs (ALL PASS). That push then exposed the
NEXT layer the compile error had hidden (every job failed before the
tests even ran): the CI runners carry a session bus the sandbox never
had, and three defects surfaced only there. All fixed and re-verified
2026-10-30 (round two): (1) the PUMP returned early on FDK's own
D-Bus socket traffic — the extfd sources the native client rides
counted as activity, so one round of bridge handshake returned the
pump out from under an X event still in flight (fired extension fd
sources now split: application sources keep the counted contract,
FDK-internal ones run their callbacks but are the pump's keep-waiting
class, the MIT-SHM rule — fdk__extfd_add_internal); (2) three test
races that only the bus-present timing could lose — the ext
notify_configure leg asserted "the LAST event is the configure" (a
trailing Expose legitimately follows it; now ordering-safe: the
configure WAS delivered, with the size), the animated-Picture GUI leg
relied on the first paint landing after the first frame tick instead
of following the documented pump;paint; cadence (now it does), and
the file-dialog GUI legs drove a real FDK window that portal mode
never hands out (OK + NULL handle is C-1's contract; the legs pin
$FDK_FILE_DIALOG_PORTAL=0 — the mock-portal legs in test-dbus own
the portal path); (3) test_a11y's font ladder missed the Fedora/
Alpine/Arch layouts (extents legs ran fontless — now the calendar
suite's candidate ladder, with an honest skip when nothing loads).
The debian anim leg and the ubuntu ext leg both reproduced locally
under dbus-run-session and pass; the full battery above re-ran green,
plus two consecutive dbus-mode test-x11 passes. Round three (same
day): the ubuntu job GREEN, and the four distro containers then hit
fdk-shell's own test link — the S-4 XShape readback legs call
XShapeGetRectangles (libXext) and X11_TEST_DEPS carried only Xtst;
the sibling-tree FDK_LIBS' -lXext (for the static archive) masked it
wherever the sibling stack was used, but the containers link the
INSTALLED libfdk.so whose .pc is deliberately bare. The dependency
moved to the test-deps line where it belongs; the fdk-shell battery
(build zero warnings, test-x11 incl. the region legs, test-desktop,
isolation, both desktop rigs) re-ran green. Round four: five of six
jobs green; void's clean-container install-test then died at
fdk-theme's missing libXrandr.so.2 — the hermetic pre-install set
(the 0d9d4de xbps lesson) predated M-6; libXrandr joined it in both
void install-tests. The other families resolve the dependency
through their package managers and passed. Round five: the debian
container then flaked a run-to-run race in fdk-shell's passive-grab
test — xtest_super_digit's single XSync ordered only the watcher's
own requests while the XTEST injections raced the server's socket
read order; the count now polls bounded (a second, early exit on
arrival). Re-verified 2026-10-29 (post-S-5): the clean
dual-backend rebuild zero warnings, core make test (incl. the NEW
tests/test_cursor.c: the mapping table, the resolution order, the
name-table grammar, the loader against a synthetic XCursor file) +
test-x11 (incl. the NEW named-cursor legs: all 22 shapes applied,
the no-twin five built through the dlopen'd RENDER lane from a
hermetic fixture theme, the Entry-hover I-beam + the app > widget >
compass priority through real XSendEvent motion) + the sway suite
(incl. the NEW named-cursor legs: 22 shapes through the production
op with real injector serials, the cursor-shape-v1 enum table
pinned row by row) + FDK_SANITIZE=0 test + release + verify-exports
(802, unchanged — the set rides the existing symbol), fdk-shell
0.4.0 build zero warnings + test-desktop + test-x11 + test-wayland
+ the notifyd + trayhost rigs + the isolation proof (the audit now
permits the package's own fdk-shell/ paths in comments — the
6cb1aaa comment prose tripped it; caught and fixed here).
Earlier, re-verified 2026-10-29 (post-M-6): the full
dual-backend fdk-shell build zero warnings, core make test +
test-x11 + test-dbus + verify-exports (802), fdk-shell test-x11
(incl. the NEW RandR legs: probed-table identity, synthetic
two-monitor origin math, stale-index fallback), the bar rig
unchanged-and-green, and the notifyd rig (the dispatch-loop change
under a real WM). Earlier: post-T-B-side-2 — both desktop rigs
unchanged-and-green (notifyd 68, trayhost 103).
Re-verified 2026-10-29 (post-C-6): the dual-backend rebuild zero
warnings, core make test (incl. the SIX new gesture legs in
tests/test_touch.c: the pinch span arithmetic with the coexisting
scroll, the re-baseline numbers, SWIPE(RIGHT,2) + SWIPE(UP,3), the
four rejects, the CANCEL wipe), make test-x11 205 [ok], test-dbus
46 [ok], FDK_SANITIZE=0, release + examples + verify-exports (848,
unchanged — the gesture events are payload additions to the numbered
enum, the ABI append policy), the sway suite 56 [ok], and fdk-shell
build + test-x11 + test-desktop + the notifyd/trayhost rigs ALL
PASS (the tap/scroll tests re-asserted type-filtered — the pinch
coexistence).
Re-verified 2026-10-29 (post-C-5): fdk-shell 0.5.0 build zero
warnings, its X11 suite (incl. the NINE new C-5 legs: the four
re-anchor legs — set_margin/zone/anchor/level with geometry + strut
readback — and the five auto-hide legs — the hidden slide's
geometry/size/strut, the reveal, the REAL XTEST strip entry, the
leave+dwell, the disarm), its Wayland suite under sway (12 [ok]
incl. the four new legs: the margin/zone/size configure, the anchor
flip, the set_level role swap, the auto-hide cycle), the desktop
model tests, the notifyd + trayhost rigs (ALL PASS), and the CORE
regression battery (make test, test-dbus 46 [ok], test-x11 205
[ok], verify-exports 848, the sway suite 56 [ok]) — unchanged and
green (the layer changes are additive; the core never moved).
Re-verified 2026-10-29 (post-S-8): the dual-backend rebuild zero
warnings (debug AND release), core make test, make test-dbus (46
[ok] incl. the five NEW AT-SPI legs against the mock stack: the
discovery through the announcement, the tree walk, the Name/State/
Component surface, the FOCUS event, Action.DoAction's real
activation), make test-x11 (205 [ok] — the bridge retries silently
on the dbus-less Xvfb, nothing regressed), FDK_SANITIZE=0 test,
release + examples + verify-exports (848, unchanged — the bridge is
internal; the tree was already public), the sway suite (56 [ok]),
and fdk-shell build + test-x11 + test-desktop + the notifyd and
trayhost rigs (ALL PASS both).
Re-verified 2026-10-29 (post-C-1): the dual-backend rebuild zero
warnings (debug AND release — the truncation-prone buffers widened),
core make test, make test-dbus (41 [ok] incl. the five NEW portal
FileChooser legs against the mock portal: the happy path's call-
shape asserts + decoded paths incl. a percent-encoded URI, the
dismissed response, the folder kind's directory choice, the auto-
mode refusal -> the own browser's real window, the forced-mode
refusal -> ERROR with no fallback), make test-x11 (205 [ok], the
file-dialog legs unchanged-and-green — no portal on a dbus-less
Xvfb, the auto fallback engages silently), FDK_SANITIZE=0 test,
release + examples + verify-exports (848, +1 — the fdk__dialog_
portal_pref env knob declared in the public header), the sway suite
(56 [ok], unchanged), and fdk-shell build + test-x11 + test-desktop
+ the notifyd and trayhost rigs (ALL PASS both).
Re-verified 2026-10-29 (post-S-9): the dual-backend rebuild zero
warnings, core make test, make test-x11 (205 [ok] incl. the seven
NEW S-9 legs: the drag-icon window's existence/hotspot-offset/
alpha-shape/tracking/destruction through a real drag into the
external sink; the zone's scoped delivery + the window callback
unchanged; the fallback region's window-mask acceptance with NO
widget events; the synthesized ENTER/LEAVE across zone boundaries)
— plus the xtest_driver selfcheck fix (the startup warp corrupted
every position-dependent flow; now opt-in), make test-dbus,
FDK_SANITIZE=0 test, release + examples + verify-exports (847, +4
— fdk_drag_begin_ex, fdk_widget_set_drop_target, fdk_widget_get_
drop_target, fdk_widget_set_drag_callback), the sway suite (56 [ok]
incl. the zone leg — a URI drag landing on a TEXT-only window
through per-position acceptance — and the FDK-source drag-with-icon
leg: the icon surface live during the drag, destroyed after, the
zone received the drop WITH the full payload through the
same-connection ::send fix, connection healthy), and fdk-shell
build + test-x11 + test-desktop + the notifyd and trayhost rigs
(ALL PASS both).
Re-verified 2026-10-29 (post-S-6): the dual-backend rebuild zero
warnings, core make test, make test-x11 (197 [ok] incl. the six NEW
activation legs: the env-capture discipline, the _NET_STARTUP_ID
round-trip, the spec-format id + the chunked "new:" reassembled from
the wire, the unique re-mint, the one-shot "remove:", the bare-X
XGetInputFocus proof — plus the fake-WM _NET_ACTIVE_WINDOW legs with
a REAL XTEST click's timestamp), make test-dbus, FDK_SANITIZE=0
test, release + examples + verify-exports (843, +2 —
fdk_window_activate, fdk_window_activation_token), the sway suite
(54 [ok] incl. the five NEW activation legs: capture-unset, the
private-queue mint with unique tokens, the injected-click focus
state, the bogus voucher REFUSED, the two-process launcher flow with
the held-button serial), and fdk-shell build + test-x11 + test-
desktop + the notifyd and trayhost rigs (both ALL PASS — the
behavior-preservation proof).
Re-verified 2026-10-27 (post-trayhost): the full battery plus the
notifyd rig and the trayhost rig (all three phases green each).
Re-verified 2026-10-28 (post-M-4/M-3): full dual-backend build zero
warnings, make test, make test-x11 (identity + XI2-touch legs green;
the 12 pre-existing tolerated MIT-SHM warnings traced to the BASE
commit ec0b2f2's own runs under direct Xvfb — environmental Xvfb
XID-base recycling, not a regression), the sway rig (WAYLAND RIG
PASS incl. the toplevel-icon and touch legs), verify-exports 799.

**Phase 2 owner directives** (the mission's lettered tasks,
tracked beside the register; the register's 62-item math is
unaffected):

| id | item | status | evidence | remaining |
|----|------|--------|----------|-----------|
| T-B | reusable notifications + tray: the app-facing API in FDK core (notification client, StatusNotifierItem export, DBusMenu server) on the native D-Bus client, and the host-side model module in fdk-shell (watcher/host, item state, dbusmenu client, notification server/queue — data + events, no drawing), with trayhost/notifyd as thin reference UIs | **DONE** — side 1 (the core app-facing API): fdk_notify.h + fdk_tray.h (src/desktop/), the byte-array bulk form in the value model, live-bus rigs with in-test mock daemon/watcher (make test-dbus, all green); docs EXTENDING.md §§14-15 + DECISIONS D-008. Side 2 (the host-side model split, 2026-10-29): fdk-shell-desktop.h + src/desktop/{tray_model,notify_model}.c (0.3.0) — the watcher/item registry/dbusmenu client and the Notifications server/etiquette/flood/live-cap/timers as no-drawing models with snapshot + action APIs; trayhost/notifyd rewritten as the thin reference UIs; the UNCHANGED rigs re-proved both pairs end to end (notifyd 68, trayhost 103 checks — the behavior-preservation proof); new make -C fdk-shell test-desktop drives the models headlessly on a private bus; DECISIONS D-014, EXTENDING.md §16 | — |
| T-C | UI font size from the theme/settings (font_size + optional ui_scale, default 16px; a desktop text-scaling preference followed by default, FDK settings overriding); notification box, paddings, icons, tray cells scaled from it | **DONE (should work; owner verification pending)** | src/theme/ui_metrics.c (the four-channel resolution, live-following the XSETTINGS/portal/settings events — the color-scheme module's shape); notifyd/trayhost scale every dimension off fdk_ui_font_px() with --font-size pinning; core chrome via fdk__ui_derived_px (14-px baseline, ratios preserved, FDK_UI_FONT_SIZE=14 = the historical pixels); tests/test_ui_metrics.c + the rigs' XS sweeps (12/16/24/32: glyph-free pads, on-screen menus, trace-math) all green; DECISIONS D-009; owner steps TESTING-ON-DESKTOP.md §8 | your eyes on the real desktop (§8) |

| id | item | status | evidence | remaining |
|----|------|--------|----------|-----------|
| A-01 | output events | **DONE** | `fdk_shell_output_*` + live sway hotplug legs (tests/test_wl.c: "hotplug ADDED fired", "mode change -> CHANGED", "unplug -> REMOVED") — d2bc0de | — |
| A-02 | output metrics | **DONE** | `fdk_shell_output_get_info`; "initial set enumerable, metrics 1024x768 scale 1" (wl) + "the screen as one output, metrics exact" (x11); X11 per-output metrics landed with M-6 (the RandR table, real CRTC geometry + mm) | — |
| A-03 | X11 multi-monitor | **DONE** | 0.3.0 (M-6): fdk-shell's own X connection carries the RandR watch (XRRSelectInput + the extension loop's fd source — D-015); real per-output records (connector names, per-CRTC geometry), output events (ADDED/CHANGED/REMOVED with gone-records enumeration), and per-output layer anchoring (output-local geometry offset into root coordinates; tests/test_x11.c's synthetic two-monitor seam reads window geometry back through the protocol); no RandR = the screen fallback (the regression pin the unchanged rigs re-proved) | owner's real multi-monitor desktop (hotplug + per-monitor struts) |
| A-04 | namespace | **DONE** | configurable namespace; "namespace -> WM_CLASS res_class" (x11 tests) + "wl namespace layer" (wl tests) | — |
| A-05 | runtime parameter changes | **DONE** | 0.5.0 (C-5): the LIVE desc — set_anchor/_margin/_exclusive_zone/_desired_size/_keyboard_mode/_level on a shown surface. Wayland: the double-buffered zwlr requests + one commit, the surface NEVER destroyed (the next configure confirms); set_level = the role swap (the WIRE ORDER the rig caught: the old role's destroy marshalled FIRST, or "Cannot reassign role" kills the connection). X11: the anchoring math recomputed + move/resize + hints + struts; set_level = the _NET_WM_STATE REMOVE/ADD exchange + _FDK_WM_LAYER rewrite. Tests: X11 4 legs (margin moves + auto strut follows; zone re-reserves; anchor TOP->BOTTOM slides with the strut edge flipping; level rewrites live); Wayland 3 legs under sway (margin/zone/size configure incl. the height honoring; the anchor flip; the role swap's new configure). D-024 | — |
| A-06 | input regions / click-through | **DONE** | S-4 shipped (2026-10-29): `fdk_window_set_input_region` + `fdk_ext_surface_set_input_region` — up to 64 window-local LOGICAL rects receive pointer input, clicks outside fall through (both backends); NULL/0 clears, a zero-area rect = `input_transparent` on demand. X11: logical→physical scaling into XShape (the clear is a full-window rect — XShapeCombineRegion NULL-Region SEGV, pinned); Wayland: straight into `wl_surface.set_input_region` (surface-local IS logical, integer and fractional scales alike; configured-surface immediate commit, first-commit otherwise). Tests: X11 readback from a second connection + REAL XTEST click-through both ways; the sway injector's real pointer (no enter into the hole, enter into the region, leave, clear); fdk-shell add-on API pinned by XShape readback. D-017 | — |
| A-07 | ext-API input+scale hooks | **DONE** (folded into S-4, core-side) | superseded by the core-side decision (D-002): the region API ships core-side on windows AND extension surfaces — `fdk_ext_surface_set_input_region` (the uniform surface the add-on-side-first judgment wanted, delivered the other way round); no ext-API version bump needed | — |
| A-08 | auto-hide / reveal | **DONE** | 0.5.0 (C-5): set_auto_hide(reveal_px, dwell_ms) — the reveal-strip model. HIDDEN = parked off the PRIMARY anchored edge with reveal_px on screen, the zone WITHDRAWN; the pointer entering the strip reveals, leaving arms the dwell (fdk_timer) whose expiry hides; reveal()/hide_now() drive it from app code; 0 disarms. The slide is backend-NATIVE (X11: a position offset — margins only push inward; Wayland: a NEGATIVE margin — zwlr margins are signed); the zone-0 rides the reconfig. Tests: X11 5 legs (hidden y=-37/size kept/strut withdrawn; reveal restores; REAL XTEST into the 3px strip reveals; leave+dwell hides; disarm stays); Wayland: the cycle protocol-clean, the role alive after. D-024 | — |
| A-09 | `_FDK_WM_LAYER` | **DONE** | set on every layer; "_FDK_WM_LAYER level names" test | — |
| A-10 | cascading submenus | **DONE** | `fdk_shell_popup_create_child` any depth; "popup cascades: 3-deep create/render + family sweep" (x11) + "2-deep cascade popup + family sweep" (wl) | — |
| A-11 | tray host | **DONE (should work; owner verification pending)** | trayhost (fdk-shell/examples/trayhost.c) — the full org.kde.StatusNotifierWatcher host on the native client: both registration dialects, the four properties (Get/GetAll/PropertiesChanged), item loading with the interface fallback chain and the a(iiay) decode, New* signal handling, vanished-item drops via NameOwnerChanged, and the complete dbusmenu round (GetLayout two-out-arg parse, AboutToShow, Event, mnemonic stripping, cascading submenus with edge flip); the rig (make -C fdk-shell test-tray) proves protocol (real dbus-daemon, libdbus+GLib cross-implementation callers, gdbus-monitor signal receipt), hostile registrations, the etiquette matrix, X11 pixels+XTEST-clicks+menu-cascade+Passive-fade/NeedsAttention under Xvfb+openbox, and sway layer-shell handshake+pixels. Owner steps: docs/TESTING-ON-DESKTOP.md §§5–7 | S-1 (D-007 records the shape decisions) |
| A-12 | notification surfaces | **DONE (should work; owner verification pending)** | notifyd (fdk-shell/examples/notifyd.c) — the full org.freedesktop.Notifications server on the native client; the rig (make -C fdk-shell test-notifyd) proves protocol (real dbus-daemon, libdbus+GLib cross-implementation callers, gdbus-monitor signal receipt), hostile/rate limits, name etiquette, X11 pixels+XTEST-clicks+EWMH under Xvfb+openbox, and sway layer-shell handshake+pixels. Owner steps: docs/TESTING-ON-DESKTOP.md §§1–4 (labwc, real incumbents, real clients, hotplug) | — |
| A-13 | outside-click dismissal, real serial | **DONE** | the core wl suite's menu leg drives xdg_popup.grab with REAL injector serials against sway: "outside click dismissed" (tests/test_wayland_integration.c) | owner's desktop: dismissal on layer popups specifically |
| B-01 | IME | **DONE** | M-1 shipped: FDK_EVENT_IME_PREEDIT/COMMIT + the focused-widget routing (Entry/TextView consume; app callback is the custom-surface seam); Wayland text-input-v3 with the v3 batch state machine + surrounding-text push + the wlroots stale-session self-heal (window switches keep IME alive); X11 XIMPreeditCallbacks with the headless-tested composition-buffer splice + the multi-codepoint commit dispatch; tests/test_ime.c (buffer math, routing, context queries) + the sway rig's input-method-v2 legs (a second connection plays fcitx5 against the REAL relay: preedit show/revise/clear, commit, the entry round, surrounding text + caret back at the IM) | level-2 fences (delete_surrounding_text, preedit spans, paragraph bidi); owner's real-IME desktop check (TESTING-ON-DESKTOP.md §11) |
| B-02 | text shaping | **DONE (level 1)** | M-2 shipped: the dlopen'd HarfBuzz + FriBidi + charset-filtered fontconfig fallback pipeline (src/text/shaping.c, docs/text.md "Complex-script shaping"); every text consumer reads shaped placements; tests/test_shaping.c pins both postures incl. the Latin compatibility re-exec; level 2 (paragraph bidi, RTL caret granularity, justification) stays open. FOLLOW-UP SHIPPED: CFF-flavored OpenType ('OTTO') renders through the native Type 2 module (src/text/cff.c, D-011) — the Arch tofu report closed; tests/test_cff.c pins dual-decoder vertex agreement, seac, and the fuzz fence. COLOR GLYPHS SHIPPED (C-3, 2026-10-29): COLRv0/v1 + CBDT/CBLC + sbix — src/text/emoji.c, the RGBA cache entry, the bitmap-only loader gate (Noto Color Emoji loads and draws), the Zsye fallback bucket; D-026 | — |
| B-03 | touch | **DONE** | M-3 shipped: FDK_EVENT_TOUCH_* slots + the core tap/two-finger-scroll engine (src/window/touch.c, the policy pinned headless by tests/test_touch.c); wl_touch on Wayland (id→window map); XI2 touch via dlopen'd libXi on X11 ($FDK_NO_XI2 opts out); both translate paths driven through the test_touch ops seam; real touch hardware = TESTING-ON-DESKTOP.md | — |
| B-04 | window icons + PID | **DONE** | fdk_window_set_icon + fdk_window_set_role (fdk_window.h); X11: _NET_WM_ICON byte-exact round-trip pinned by the integration rig (second raw-Xlib connection), _NET_WM_PID + WM_CLIENT_MACHINE + WM_WINDOW_ROLE at create ("main"/"popup" defaults, set_role rewrites); Wayland: xdg-toplevel-icon-v1 bound where offered (square-buffer letterbox, frozen-pool discipline, icon+buffer pair owned by the window), manager-absent = the documented no-op; sway rig proves connection health both worlds | real-desktop taskbars/docks (TESTING-ON-DESKTOP.md) |
| B-05 | X11 RandR (core) | **DONE** | 0.3.0 (M-6): src/platform/x11/x11_randr.c — the internal per-CRTC table (reading-order sorted) probed at connect, RRNotify-masked, refreshed in the dispatch loop; the bare-X maximize/fullscreen fallbacks fill the CONTAINING monitor (was: the whole screen). No public API (the Wayland core's no-output-API parity — D-015) | — |
| B-06 | dark mode following | **DONE** [VERIFIED-CHANGED: audit said partial] | XSETTINGS `Gtk/ColorScheme` (x11_xsettings.c) + `org.freedesktop.appearance` portal on the native client; live: "SettingChanged flips the scheme + theme live" — d2a253f | — |
| B-07 | X11 DPI awareness | **DONE** [VERIFIED-CHANGED: audit said missing] | shipped 1.4.5: Xft.dpi + physical-size fallback, integer scale [1,3]; `test_x11_hidpi_gui` end-to-end on a private 192-dpi Xvfb | — (RandR scale is N/A: X11 has no scale protocol) |
| B-08 | D-Bus foundation | **DONE** [VERIFIED-CHANGED: audit said missing] | M-7's native client (src/dbus/, 112 exported fdk_dbus_* of 757 total, commit 4d42c8c): live daemon battery incl. the ported libdbus cross-implementation leg; 20k-mutation fuzz + prefix-exhaustive truncation in `make test` | — |
| B-09 | portal FileChooser fallback | **DONE** | C-1 shipped (2026-10-29): the org.freedesktop.portal.FileChooser client on the native D-Bus client (src/desktop/portal_file_chooser.c; 848 exports) — fdk_dialog_open_file/save_file consult the portal FIRST and fall back to the toolkit's own browser on ANY failure (no bus, no portal, refused call, timeout: the GTK posture); the spec's race-free ordering (handle_token + the predictable request path, subscription armed before the call); the options dict carries the full kind contract (directory/multiple/current_folder as bytes/current_name/filters as (sa(us))); the Response decode rides the a{sv} dict lookup with the variant unwrap, percent-decoding to POSIX paths; $FDK_FILE_DIALOG_PORTAL=0/1/-auto. Tests: 5 new live-bus legs against a mock FileChooser portal (the happy path with the call-shape asserts — multiple on, the folder hint, 2 filters — and the decoded paths incl. a percent-encoded one; response=1 -> CANCELLED; the folder kind's directory choice; the AUTO-mode refusal -> the own browser really took the pick; the FORCED-mode refusal -> ERROR with no fallback). D-022 | owner's real portal desktop (TESTING-ON-DESKTOP.md §15) |
| B-10 | AT-SPI interop | **DONE (level 1)** | S-8 shipped (2026-10-29): the AT-SPI2 bridge on the native D-Bus client (src/desktop/atspi.c) — the two-bus handshake (session-bus org.a11y.Bus.GetAddress -> the a11y bus address, RETRY on a 2 s throttle — a screen reader toggled on mid-session is a real case; $FDK_ATSPI=0 off), the object tree served at /org/freedesktop/atspi/accessible/<id> (id 0 = the application root; virtual children of painted-row containers get their own (container,index) entries — a screen reader sees list ROWS), the full level-1 surface: Accessible (AtspiRole-numbered Role, RoleName, the 64-bit State pair, ChildCount/GetChildren/GetChildAtIndex/GetIndexInParent/GetApplication/GetInterfaces/GetAttributes + the Name/Description/Parent/ChildCount properties), Component (GetExtents/GetPosition/GetSize in desktop coordinates), Action (DoAction(0) == the widget's ACTIVATE — the same programmatic path a keyboard activation takes; virtual rows through virtual_perform); events: Event.Object Focus + StateChanged on focus moves, StateChanged/PropertyChange/ChildrenChanged for the tree; discovery through the ChildrenChanged announcement on the root (the at-spi2 registry's own mechanism — the sender's unique name is the probe address); the pointer-keyed registry with diff-and-backstop GC (address-only walks; dead pointers never dereferenced). Tests: 5 live-bus legs against the mock stack (A serving org.a11y.Bus answering the session bus's own address — the rig collapses the two-bus architecture; B as the screen reader): the discovery, the tree walk (app -> window -> {button 43, label, frame -> entry 78}), the Name property + the State bitfield + the Component extents, the FOCUS event on the entry when the tree's focus moved, and Action.DoAction activating the button's real callback. D-023. LEVEL 2 SHIPPED (C-2, 2026-10-29): the Text interface + relations export — see the C-2 row and D-027. | owner's real screen-reader desktop (TESTING-ON-DESKTOP.md §16) |
| B-11 | idle/screensaver inhibition | **DONE** | S-3 shipped: fdk_idle.h (fdk_idle_inhibit/uninhibit/status — one handle, either mechanism); Wayland zwp_idle_inhibit_manager_v1 through the window-scoped platform op (window-owned engagement, sever-at-destroy); org.freedesktop.ScreenSaver on the native D-Bus client with the DEFERRED release (uninhibit-before-cookie still sends the matching UnInhibit — no orphaned inhibition); the window-dies-inhibited release; tests/test_idle.c + the mock-ScreenSaver legs in the live D-Bus rig (cookie round trip, deferred release, refusal) + the sway rig (engage/release/destroy-while-inhibited) + the X11 no-bus fallback | GNOME's org.gnome.ScreenSaver variant not probed (documented); owner's real-desktop check |
| B-12 | cursor shape set | **DONE** | S-5 shipped (2026-10-29; CORRECTED 2026-10-30 — see the post-CI-repair re-verification above: the shipped Xrender.h include was a forbidden build dep that broke every CI job for 15 pushes, and it hid PictStandardARGB32 spelled as A1's 4 — the themed cursors were garbage pictures behind async BadMatch; both fixed, the wire values pinned, the error-counting fence added): `fdk_cursor_shape` 2 -> 22 shapes (the cursor-shape-v1 semantic space, append-only; 802 exports unchanged); ONE resolution point (`fdk__cursor_resolve`: app request > widget hover (Entry/TextView/Spin I-beam) > compass > arrow) with the transition cache; Wayland: set_shape when the manager exists (the sway suite pins the full enum table row by row) + the shared XCursor theme loader fallback (src/platform/cursor_theme.c, extracted from the 1.1.4 Wayland parser); X11: the dlopen'd libXrender.so.1 RENDER lane (theme images, no new build dep — TRUE after the correction: the wire types are spelled in the platform header) over the core font glyph table with the documented substitutions (D-016); fdk-shell 0.4.0 mirrors the set | — |
| B-13 | drag icons + drop targets | **DONE** | S-9 shipped (2026-10-29): fdk_drag_begin_ex (the icon rides the drag — Wayland: the protocol's own start_drag icon surface, ARGB8888 SHM, committed pre-start, destroyed at end; X11: the GTK-shaped override-redirect window tracking the pointer at the hotspot offset, alpha-shaped via XShape, skipped by the source's own target walk) + fdk_widget_set_drop_target / fdk_widget_get_drop_target / fdk_widget_set_drag_callback (widget-level targets with scoped ENTER/MOTION/LEAVE/DROP delivery; acceptance PER-POSITION through fdk__core_drag_accepts_at — the XDND Status and wl_data_offer.accept re-evaluate at every position, window-mask fallback where no target owns it; the window callback still sees everything). SAME-CONNECTION drags fixed (the ::send-served-by-our-own-dispatch read + the drag_drop_active fence against mid-read teardown); the 250 ms EOF never-arriving wart closed (our pipe write-end closed at the receive request); the X11 DROP event carries the last Position's coordinates; 847 exports (+4). Tests: X11 — 7 new legs (icon window exists/tracks/alpha-shapes/destroys through a REAL drag into the external sink; files ON the zone -> scoped widget delivery + window unchanged; text on the fallback -> window-mask acceptance with NO widget events; scoped ENTER/LEAVE across zone boundaries through synthetic XDND); Wayland — the interop leg's zone (a URI drag landing on a TEXT-only window through per-position acceptance) + the FDK-source drag-with-icon leg (icon surface live during / destroyed after, the zone received the drop with the full payload, connection healthy). D-021 | owner's real-desktop check (TESTING-ON-DESKTOP.md §14) |
| B-14 | image formats | **DONE** [animated GIF shipped] | C-4 landed (2026-10-29): fdk_gif + fdk_gif_player + fdk_picture_set_animated_file — the spec-native decoder (LZW incl. KwKwK, disposal 1/2/3, interlace, NETSCAPE loops, the PIL embedded-stream resync), the one-frame-at-a-time player, the Picture delay-paced timer with pause; tests/test_gif.c + the X11 GUI leg (real-encoder fixtures in tests/data/gifgen/); D-019. Static raster + SVG were already present. Fenced: animated WebP/APNG stay future families on the same shape | — |
| B-15 | GPU on Wayland | **DONE** | EGL present lane shipped (2026-10-30, D-1) — see the B-15 register row | — (D-1 closed it) |
| B-16 | subsurfaces | **DONE** | = D-2 (both halves: wl_subsurface + X11 child windows; D-029 records the parent-managed tree-state lesson) | — |
| B-17 | tablets / gestures | **DONE** (gestures; tablets stay D-3) | C-6 shipped (2026-10-29): FDK_EVENT_GESTURE_PINCH + FDK_EVENT_GESTURE_SWIPE (fdk_gesture_event: focus/scale/delta/fingers/direction) on the pure touch engine — the pinch rides the two-slot posture the scroll uses (scale cumulative since the posture began, delta incremental, the centroid focus; coexists with the scroll — centroid vs span; a finger swap re-baselines), the swipe is an END-gesture verdict (every lift within FDK_GESTURE_SWIPE_MS, EVERY finger's dominant-axis travel >= FDK_GESTURE_SWIPE_MIN_PX — the per-finger minimum the rig caught: two lazy fingers do not sum into a swipe — opposing travels reject, fingers = the peak membership). No backend moved: the emissions ride the engine's emit sink; both backends identical by construction. Tests: 6 engine legs (the span arithmetic incl. the coexisting scroll + the re-baseline numbers 2.0-not-8/3; SWIPE(RIGHT,2) + SWIPE(UP,3); the four rejects — slow lift window, short travel, single finger, disagreement; CANCEL wipes the gesture state). D-025 | tablets remain D-3 |
| B-18 | widget residue | **DONE** | editable combo (`fdk_combo_set_editable`) + S-10 shipped (2026-10-29): fdk_calendar (the month grid — Monday-lead, six rows, adjacent-month cells, keyboard walking, today marker), fdk_date_picker (formatted field + popup calendar, the combo guard discipline), fdk_time_picker (segmented stepper with wraparound + 12/24h cycle); the i18n calendar math underneath; D-018 records the posture (no clock reads, no parse). tests/test_calendar.c + the X11 GUI leg + the sway leg (real-seat day pick through the compositor); 825 exports | — |
| B-19 | session restore protocols | **PARTIAL** | typed prefs store; no WM_SAVE_YOURSELF / xdg session | D-5 |
| M-1 | IME both backends | **DONE** | = B-01 (both backends; the sway rig's two-way protocol proof) | — |
| M-2 | shaping level 1 | **DONE** | = B-02 (level 1; level 2 open) | — |
| M-3 | touch basics | **DONE** | = B-03 (slots, tap→click, two-finger scroll; both backends; policy battery + seams) | — |
| M-4 | window identity | **DONE** | = B-04 (icons/PID part); the activation half closed as S-6 | — |
| M-5 | fdk-shell outputs | **DONE** (rc2, 0.2.0) | live sway output hotplug/metrics legs + namespace + `_FDK_WM_LAYER` — d2bc0de | — |
| M-6 | X11 multi-monitor | **DONE** | = A-03/B-05 (both halves; D-015) | owner's real-desktop check |
| M-7 | D-Bus foundation + dark mode | **DONE** (foundation 4d42c8c; dark mode d2a253f) | live-bus battery green incl. cross-implementation; wire bytes pinned vs captured reference output; fuzzed; CI makes skips fatal | — [VERIFIED-CHANGED: the row's "rides on top (next)" is stale — dark mode landed] |
| M-8 | popup cascades | **DONE** (rc2, 0.2.0) | 3-deep x11 + 2-deep wl family sweeps — d2bc0de | — |
| M-9 | the release gate | **IN PROGRESS** | discipline verified: no tags beyond v1.0.0, rc line held, per-push CI, per-item docs | owner's real-desktop sign-off before any tag |
| S-1 | tray host | **DONE (should work; owner verification pending)** | = A-11 (trayhost + the rig; D-007 records the shape decisions) | owner verification via docs/TESTING-ON-DESKTOP.md |
| S-2 | notification display | **DONE (should work; owner verification pending)** | = A-12 (notifyd + the rig; D-006 records the shape decisions) | owner verification via docs/TESTING-ON-DESKTOP.md |
| S-3 | idle inhibition | **DONE** | = B-11 (both mechanisms; the live-bus mock + the sway rig) | — |
| S-4 | input regions | **DONE** | = A-06 (fdk_window_set_input_region + fdk_ext_surface_set_input_region, both backends, XTEST + sway-injector proof; D-017) | — |
| S-5 | cursor shape set | **DONE** | = B-12 (both backends; the hermetic RENDER proof on Xvfb, the enum-table pin on sway) | — |
| S-6 | xdg_activation + startup notification | **DONE** | fdk_window_activate + fdk_window_activation_token (fdk_window.h's activation section is the reference; 843 exports, +2): Wayland xdg-activation-v1 — the token mint through a PRIVATE event queue + wl_display_roundtrip_queue (reentrancy-safe from inside any handler — the buffer-release pattern applied to a protocol object), set_serial/set_app_id/set_surface attached, activate on the window; the XDG_ACTIVATION_TOKEN launch voucher captured at connect, unset immediately, spent by the first activate (one launch one token). X11 — _NET_ACTIVE_WINDOW with source indication 1 and the LAST USER EVENT timestamp (tracked in the dispatch pump from key/button/motion), bare-X raise+XSetInputFocus fallback; the full startup-notification wire CLIENT-SIDE (no D-Bus, no library): spec-format ids (TIMESTAMP-SCREEN-pid-seq-HOST), the chunked "new:" state on mint (_NET_STARTUP_INFO_BEGIN + continuations, 20 bytes each), _NET_STARTUP_ID on every window created while DESKTOP_STARTUP_ID is held, the one-shot "remove:" at the first show. Tests: X11 — 6 new legs (env capture unset, _NET_STARTUP_ID round-trip, spec-format id + chunked new: reassembled, unique re-mint, one-shot remove:, bare-X XGetInputFocus proof) + the fake-WM _NET_ACTIVE_WINDOW legs (app source, CurrentTime, then a REAL XTEST click's timestamp); Wayland — 5 new sway legs (capture-unset, compositor-minted unique tokens through the private queue, injected click focus state, bogus voucher REFUSED — focus-stealing prevention IS the assertion, and the full TWO-PROCESS launcher flow: held-button serial on the parent's surface, mint, re-exec'd child spends the voucher, sway flips focus to the child); the fake WM now handles MapRequest (windows actually map under it — a real WM's defining duty). D-020 | owner's real-desktop check (TESTING-ON-DESKTOP.md §13) |
| S-7 | X11 DPI awareness | **DONE** [VERIFIED-CHANGED: audit said missing] | = B-07 (1.4.5) | — |
| S-8 | AT-SPI2 level 1 | **DONE** | = B-10 (level 1; D-023; level 2 = C-2, also DONE) | — |
| S-9 | drag icon + drop targets | **DONE** | = B-13 (both halves; the rigs' standard on each backend; D-021) | owner's real-desktop check |
| S-10 | widget residue | **DONE** | = B-18 (the calendar family, D-018) | — |
| C-1 | portal FileChooser | **DONE** | = B-09 (the client + the fallback; D-022) | owner's real portal desktop |
| C-2 | AT-SPI text interfaces | **DONE** | C-2 shipped (2026-10-29): the org.freedesktop.atspi.Text interface on the bridge — GetCharacterCount/GetCaretOffset/SetCaretOffset/GetText/GetTextAt-Before-AfterOffset/GetCharacterAtOffset/GetStringAtOffset/the selection quartet/the attribute family/GetCharacterExtents/GetRangeExtents/GetOffsetAtPoint/the scroll pair — with the CODEPOINT<->BYTE conversion the wire requires (the rig pins "h\xC3\xA9llo": count 5, byte length 6, SetCaretOffset(2) lands at byte 3); per-character extents + hit-test through a new internal seam (Entry: the caret-width arithmetic; TextView: the line cache + page origin; read-only classes: the whole-bounds fallback); the whitespace-hop rule for word runs; the CARET_MOVED notification (the signal source — one notify point at the editors' selection chokepoints) mapping to TextCaretMoved/TextSelectionChanged, VALUE_CHANGED-on-text mapping to TextChanged; GetRelationSet (a(ua(so)), the AT-SPI relation numbers pinned against at-spi2-core's own constants header). Tests: 3 new live-bus legs (the full method surface incl. the multibyte pin + the signal receipts; the LABEL_FOR/LABELLED_BY pair both directions + symmetric removal) + 2 headless legs (the notification semantics incl. the no-op rule; the extents seam). D-027 | — |
| C-3 | color emoji | **DONE** | C-3 shipped (2026-10-29): COLRv0/v1 + CPAL, CBDT/CBLC, and sbix — all three color-glyph families the Linux desktop actually meets, parsed natively with field layouts pinned against fontTools (the fixtures' own generator); the RGBA glyph (a color glyph's cache entry carries straight-RGBA8 pixels, composited through fdk_surface_blend_rgba — the public API unchanged at 848 exports); the BITMAP-ONLY LOADER GATE (Noto Color Emoji — no glyf, no CFF, 10.5 MB of PNG strikes — loads: stbtt_InitFont's outline requirement bypassed, the info hand-filled with stb's own cmap selection, the outline rasterizer fenced off); strike selection by log-ratio em-pixels, PNG via the clipboard's stb path, box-filter downscale + bilinear upscale; the COLRv1 paint engine (gradients in paint space through the inverse transform, affine coverage via a 16x supersampled nonzero-winding rasterizer for rotate/skew, all 28 W3C blend modes, depth- and cycle-capped); the Zsye fallback bucket; $FDK_NO_COLOR_GLYPHS. Tests: test_emoji — COLRv0 layer/alpha math, every v1 paint family against fontTools-built fixtures, CBDT strikes (formats 1 + 4, downscale), the real Noto smoke leg, the truncation sweep. D-026 | — |
| C-4 | animated images | **DONE** | = B-14 (GIF; D-019) | — |
| C-5 | re-anchor + auto-hide | **DONE** | = A-05/A-08 (both halves, both backends; D-024) | — |
| C-6 | basic gestures | **DONE** | = B-17 (the pinch/swipe half; D-025) | — |
| D-1 | EGL/GPU Wayland | **DONE** (2026-10-30) | the GLX lane's contract on EGL: CPU rasterization unchanged, the lane replaces only present()'s "pixels → compositor" half — texture upload (BGRA/UNSIGNED_BYTE, the staging buffer's little-endian XRGB8888 bytes in place) + fullscreen NEAREST quad + eglSwapBuffers through a wl_egl_window-backed EGL surface; dlopen("libEGL.so.1") + dlopen("libwayland-egl.so.1"), ZERO new build deps, hand-declared EGL/GLES2 subset through eglGetProcAddress (GLVND does not promise dlsym for core GLES); policy mirrors x11_gl.c (hardware yes, llvmpipe/softpipe/swrast no, FDK_GL_FORCE overrides, FDK_NO_GL opts out); the ALPHA decision is correctness not style — the staging buffer's X byte is ZERO, so the quad shader pins alpha 1.0 and the config prefers an alpha-less EGLConfig (an XRGB8888 swap buffer, opaque by protocol like wl_shm's; passthrough alpha would be a fully transparent window — the pre-merge review caught it); first-frame self-check glReadPixels (RGBA/UNSIGNED_BYTE — the ONLY GLES2-guaranteed default-framebuffer readback) vs the staging buffer, mismatch = REAL failure, process-wide refusal, FORCE overrides on a fresh connection, fdk__wayland_egl_test_reset() seam; failure = DOWNGRADE never a broken frame (any lane error or FDK_GL_FAIL_AT poison salvages the staging frame into a wl_shm slot and re-presents it NOW, permanent — fdk__wayland_surface_downgrade_from_lane); deferred first frame rides lane_frame_deferred and the configure handler flushes it (the CPU path's deferred render_pending contract mirrored); small-surface gate FDK_WL_GL_MIN_PIXELS = the X11 value; lazy per-connection load at the FIRST acquisition of ANY window (no-window apps never touch EGL). Tests (test_wayland_integration.c, the x11_gl group's shape): the policy matrix (FDK_NO_GL strict CPU; small windows CPU by the size gate — deterministic; large default OBSERVED), present + health under FORCE (two windows, one connection, full + partial damage frames, interleaved, connection healthy after every one), the downgrade leg (poisoned present #2 salvaged, permanent, frames keep flowing), the self-check REAL-failure leg (per-connection refusal, one warning per connection, the FRESH-connection FORCE override, the reset seam). Client-side pixel fidelity pinned by the self-check itself; compositor-side has no XGetImage equivalent a client may call — pinned by the sway rig's screenshot smoke (byte-exact probes through grim, passed) + the desktop sign-off recipe (TESTING-ON-DESKTOP.md). Real hardware GPUs: implemented — should work (untested on real hardware; the rig exercises it through Mesa's swrast/WL-SHM EGL path under FDK_GL_FORCE) | — |
| D-2 | subsurfaces | **DONE** (2026-10-30) | fdk_ext_subsurface_create/_ex + set_position/resize/set_desync/place_above/place_below (extension API v2, 855 exports, +7): X11 — real child windows (server-clipped, XConfigureWindow sibling restack, XTEST-routed input, XDND aware, themed pre-paint background, the creation XSync barrier); Wayland — wl_subsurface DESYNC by default, the ext-adopt field set + output-enter scale tracking + fractional preference, configured from birth (no handshake exists). THE protocol lesson (D-029): position/stacking/tree-membership are PARENT-managed state — a desync child of a quiet parent is invisible (sway never answers its frame callback; the rig caught it in the first run); FDK issues the applying EMPTY ancestor commit (gated on configured) at create/set_position/place so moves are immediate on both backends. Keyboard follows the parent's focus, pointer routes to the child (child-local coords, both backends by construction); popups on subsurfaces: X11 yes, Wayland the loud UNSUPPORTED (xdg_popup's parent rule); nesting legal on both, sweep depth-first; resize dispatches CONFIGURE (client owns the size). Tests: the X11 leg (second-connection window-tree truth, pixel readback over-parent, real XTEST click, the counting error fence around both sweep orders) + the sway section (injected clicks, hit-testing-proven move/resize/stacking-flip, grim+PIL desync pixel proof) | — |
| D-3 | tablets / stylus | **DEFERRED** | by scope discipline | — |
| D-4 | foreign-toplevel | **DEFERRED** | by scope discipline | — |
| D-5 | xdg session save/restore | **DEFERRED** | by scope discipline | — |

---

## Part A — fdk-shell vs the layer-shell world

What 0.1.0 already does (verified in `fdk-shell/src/`, pinned by
`tests/test_x11.c`, `tests/test_wl.c`, `tests/bar_rig.sh`):

- **Layers** background / bottom / top / overlay — protocol truth on
  Wayland (wlr-layer-shell v4, version-clamped, on-demand keyboard
  degraded honestly on pre-v4 compositors); EWMH mapping on X11
  (dock type + per-level `ABOVE`/`SKIP_*`, sticky pre-map AND
  post-map, the fullscreen-over-docks ceiling documented).
- **Anchors / margins / exclusive zone** — all four edges, OR-combined,
  corner and centered cases; zone `-1` (auto) / `0` / `>0`; the
  wlroots XOR-anchoring reservation rule mirrored in
  `_NET_WM_STRUT_PARTIAL` (plus the legacy 4-card strut).
- **Keyboard interactivity** — none / exclusive (a real `XGrabKeyboard`
  on X11) / on-demand (v4 on Wayland; best-effort focus on X11).
- **Configure negotiation** — ack + `notify_configure` into FDK, with
  the present gate (no commit before the first configure; the labwc/
  sway protocol-killer is structurally impossible now).
- **Per-output selection** (Wayland; index or "every output"), output
  enumeration with connector names, output-removal → `closed` →
  close-request delivery.
- **Popups on any layer** — full grab/dismissal contract, the
  pointer-only grab lesson, desktop-switch dismissal, parent sweep.
- **Fractional scaling / HiDPI** — adopted surfaces get
  `wp_fractional_scale_v1` + output-enter scale tracking at adoption
  (`wayland_ext.c`), same as windows.
- **Hover cursors** (arrow / hand, re-applied on enter), self-
  positioned dock hints (`USPosition`), EXPOSE repaint contract,
  five-family packaging with a versioned FDK dependency, the
  prefix-isolation build proof, and the WM-side contract for a true
  overlay layer (the three-step private-atom spec in the README).

### fdk-shell gap register

| id | gap | verdict | effort | note |
|----|-----|---------|--------|------|
| A-01 | **Output events** — no notification when outputs are added / removed / resized. The registry listener tracks it; the app can only poll `fdk_shell_output_count`. A panel cannot react to a monitor being plugged in. | missing | S–M | the data exists (wl.c keeps live records); an event callback is the missing piece |
| A-02 | **Output metrics** — no size / scale / position query per output. Apps need this to size panels per-output before the first configure, and it is the substrate A-06 (X11 multi-monitor) builds on. | missing | S | pair with A-01 |
| A-03 | **X11 multi-monitor** — exactly one output (the screen); no RandR. Per-monitor docks are impossible; a top bar spans all monitors with one strut. | missing | M | RandR 1.3 output enumeration + CRTCs; hotplug = RRNotify; per-output geometry feeds the same anchoring math **[DONE 2026-10-29 — see the A-03 verified-status row; M-6]** |
| A-04 | **Namespace** — hardcoded `"fdk-shell"`; the spec's namespace is what compositor rules (sway `for_window`-style layer criteria) match on. gtk-layer-shell exposes `set_namespace`. | partial | S | one desc field |
| A-05 | **Runtime parameter changes** — `set_layer` / `set_anchor` / `set_margin` / `set_exclusive_zone` / `set_size` / `set_keyboard_interactivity` are all runtime-capable in the protocol (double-buffered state); fdk-shell exposes them create-time only (destroy + re-create is the documented idiom). | **DONE** [VERIFIED-CHANGED: 0.5.0/C-5 shipped the full runtime set — see the A-05 verified-status row] | M | the protocol allows live re-anchor without losing the surface; gtk-layer-shell allows pre-map changes of everything |
| A-06 | **Input regions / click-through** — no `wl_surface.set_input_region` on Wayland, no XShape on X11. Transparent margins of a notification overlay eat clicks; auto-hide reveal strips are impossible without it. | missing | S–M | needs a small extension-API addition (A-07) or an fdk-shell-side call on the native handle **[DONE 2026-10-29 — see the A-06 verified-status row; S-4]** |
| A-07 | **Extension-API surface hooks for input+scale region control** — the add-on can already do both itself through the native handle (documented accessors); an ext-API v2 helper would make it uniform. Judgment: do it add-on-side first. | missing | S | append-only ops fields **[VERIFIED-CHANGED: the add-on-side-first judgment is SUPERSEDED - S-4 owns input regions core-side (windows AND extension surfaces) per the 2026-10-27 owner decision, docs/DECISIONS.md D-002]** **[DONE 2026-10-29 — folded into S-4 core-side; see the A-07 verified-status row]** |
| A-08 | **Auto-hide / reveal** — no API. Possible today only via destroy/recreate churn (Wayland) or hide (X11); a real answer needs A-06 plus zone animation. | **DONE** [VERIFIED-CHANGED: 0.5.0/C-5 shipped the reveal-strip model on A-06's regions — see the A-08 verified-status row] | M | panels want it; gtk-layer-shell doesn't provide it either (it is panel-toolkit territory, and fdk-shell IS the panel toolkit here) |
| A-09 | **`_FDK_WM_LAYER` property** — the README's custom-WM contract (option 2) says fdk-shell "would set it the day a WM adopts it"; setting it unconditionally is harmless on WMs that don't know it and lets the owner's WM adopt without an fdk-shell release. | partial | S | property + README note |
| A-10 | **Cascading submenus** — core popups parent to any extension surface (verified: `fdk_ext_popup_create` → `fdk__window_create_popup_ex` accepts a popup parent), but fdk-shell's API only takes a LAYER parent, so bars can't build launcher menus with submenus. | partial | S | expose popup-of-popup in the fdk-shell API |
| A-11 | **StatusNotifier tray host** — no StatusNotifierItem/Watcher. A panel without a tray is half a panel; this is the single most-requested panel feature after the bar itself. Rides the D-Bus foundation (B-08). | missing | M–L | fdk-shell add-on surface + fd sources is exactly the architecture for it | **[DONE 2026-10-27: trayhost — should work; owner verification pending (docs/TESTING-ON-DESKTOP.md)]** |
| A-12 | **Notification surfaces** — no org.freedesktop.Notifications client. The overlay layer exists; the protocol client doesn't. Rides B-08. | missing | S–M | natural fdk-shell example (OSD/notification shell) | **[DONE 2026-10-27: notifyd — should work; owner verification pending (docs/TESTING-ON-DESKTOP.md)]** |
| A-13 | **Wayland outside-click dismissal** — machinery exists (xdg_popup grab, menu-tested) but is untested against a real seat serial; headless rigs can't produce one. | partial | S | owner's machine verification; rig with wlroots virtual pointer can synthesize real serials |

Not gaps (checked, fine): exclusive-zone edge semantics including the
partial-strut ranges; keyboard on-demand downgrade honesty; surface
lifecycle when an output disappears (closed → close request);
lifecycle when the *compositor* disappears (bar rig kills it);
popups on overlay surfaces; app_id/namespace collision behavior;
multi-shell (one per context, second init refused); docs quality.

---

## Part B — FDK core vs a complete toolkit (GTK4 / Qt6 reference)

What is already present and tested (the short version — each of these
has an API surface, tests, and docs in the tree):

- **Windows**: create/configure/close, maximize/minimize/fullscreen,
  decorations (xdg-decoration negotiation + CSD title band +
  interactive move/resize through `xdg_toplevel.move` /
  `_NET_WM_MOVERESIZE`), popups with grab + dismissal, title +
  app_id/WM_CLASS identity, multi-window, event callbacks, shortcuts
  (spec parse/format, window accelerators, menu accelerators).
- **Rendering**: software surface API — fills, rects, lines, circles,
  rounded rects (aliased + AA), vertical gradients, clip stack, blit
  / blend / transformed blit, damage tracking, PNG encode, PNG
  screenshot-verified examples; the X11 GPU lane (GLX, runtime
  dlopen'd, per-window context, size-aware lane policy, three-verdict
  self-check).
- **Text**: stb_truetype glyph rasterization + cache, metrics,
  measure/draw single-line LTR with pair kerning, markup, layout
  helpers (wrap/ellipsize/align), system font discovery via
  runtime-dlopen'd fontconfig, ttc first-face, untrusted-font
  validation.
- **Widgets** (33 create functions + dialogs): label, button, toggle,
  checkbox, radio, progress, separator, frame, entry (selection, undo,
  clipboard, preedit-render groundwork), scrollview, list, iconview,
  textview (editable multiline, 256 KiB cap, undo), picture, tree,
  slider, spin, toolbar, notebook, canvas, menu + menu bar + submenus,
  combo, spinner, paned, expander, statusbar, search entry, levelbar,
  revealer, stack + switcher, menu button, tooltip; dialogs: message,
  prompt, open/save file (icon/list views, filters), about, font
  chooser, color chooser; public generic undo stack; layout engine
  (box + grid); themes (own format, discovery, shipped faces, live
  XSETTINGS icon-theme following, icon themes incl. SVG via nanosvg).
- **Clipboard**: CLIPBOARD + PRIMARY on both backends; UTF-8 text +
  image/png; ICCCM TARGETS on X11, wl_data_device on Wayland.
- **DnD**: receive + source on both backends (XDND / wl_data_device),
  text + URI lists, percent-decoding, honest failed-transfer delivery.
- **i18n**: own catalog format (contexts, CLDR plurals), locale
  parsing, number/currency/percent/date/time formatting, plural
  operands, CLI tools; docs/i18n.md.
- **A11y (internal)**: role/name/description tree, subscribe/notify,
  actions, text-at-offset/caret/selection, relations, virtual
  containers, keyboard navigation, the narrator with its own catalog.
- **Animation**: tick/done API with easing; Wayland frame-callback
  pacing; X11 motion coalescing + release-wait collapse + startup
  trace (`FDK_STARTUP_TRACE`).
- **HiDPI (Wayland)**: fractional-scale-v1 + viewporter + output
  enter/leave scale tracking, for windows AND extension surfaces.
- **Prefs / session state**: typed key-value store per app_id, save/
  load, documented format + CLI tools.
- **Extension API v1**: kinds, adoption, event routing, render path,
  popups + present gate, fd loop sources, capability queries,
  documented native accessors; fdk-shell as the reference consumer.
- **IME groundwork** (widget level): preedit string API, inline
  preedit rendering with underline at the caret — the platform layer
  just doesn't drive it yet (B-01).

### FDK core gap register

| id | gap | verdict | impact on real apps | effort | ABI risk |
|----|-----|---------|---------------------|--------|----------|
| B-01 | **Wayland IME — no text-input-v3.** The seat binds pointer + keyboard only (`wayland_seat.c`). On X11, XIM runs `PreeditNothing\|StatusNothing` (compose keys work; no preedit). | partial (X11) / missing (Wayland) | CJK/input-method users cannot type on Wayland at all — the single largest population-level break in the toolkit | M | none — new `FDK_EVENT_IME_*` types append to the numbered enum; the union is delivered by pointer and never app-allocated | **[DONE 2026-10-28: M-1 — events + routing both backends; text-input-v3 with the v3 state machine and the stale-session self-heal; XIM preedit callbacks with the headless-tested splice; the sway rig plays a real input-method-v2 and proves the whole loop]** |
| B-02 | **Text shaping — no HarfBuzz, no FriBidi, no per-script font fallback.** Shaping v1 is codepoint-at-a-time LTR with pair kerning (docs/text.md); unmapped codepoints render `.notdef`; one font per draw call. | **DONE (level 1)** [VERIFIED-CHANGED: M-2 shipped — the dlopen'd pipeline (shaping.c) with bidi reordering, GSUB/GPOS shaping, mark attachment, per-script charset-filtered fallback; Entry/TextView/Label/markup/Entry-caret all consume shaped placements; Latin metrics pinned identical across postures; `fdk_text_shaping_available()` + `fdk_font_has_glyph()` appended (797 exports)] | Arabic/Hebrew/Persian/Urdu now render in visual order with joining forms and ligatures; mixed-script strings pick fonts per segment; level 2 (paragraph bidi, RTL caret granularity, justification, emoji) open | M–L | none — as audited; 2 functions appended |
| B-03 | **Touch input — entirely absent.** No `wl_touch` binding (Wayland), no XI2 touch (X11); the event stream has no touch types. | missing | touchscreen laptops: no tap, no scroll, no drag on Wayland (wlroots does not emulate a pointer); X11 survives only via the server's touch→pointer emulation | M | none — `FDK_EVENT_TOUCH_*` append; `fdk_pointer_button_event`-style appended fields |
| B-04 | **Taskbar/window identity — no icons.** No `_NET_WM_ICON` (X11), no xdg-toplevel-icon-v1 (Wayland); no `_NET_WM_PID` / `WM_CLIENT_MACHINE` / `WM_WINDOW_ROLE`; no startup notification / xdg_activation. | **DONE** [VERIFIED-CHANGED: icons/PID as M-4; activation + startup notification as S-6 (2026-10-29) — see the S-6 verified-status row] | every FDK app shows a generic taskbar icon; docks/panels can't match windows to apps; no focus-stealing protection | S (icons/PID) / M (activation) | none |
| B-05 | **X11 multi-monitor / RandR — absent in core too.** One screen; no output model, no RRNotify. (Wayland core: outputs tracked, scale follows; no app-facing output API.) | missing | multi-monitor X11 desktops: windows span/clamp to the whole screen; per-monitor behavior impossible | M | none |
| B-06 | **Dark mode / system theme following — icon theme only.** XSETTINGS reads exactly `Net/IconThemeName`; no `Gtk/ColorScheme` (X11), no org.freedesktop.appearance color-scheme (Wayland). | partial | apps don't follow the desktop's light/dark preference — a visible polish break on every themed desktop | S–M | none **[VERIFIED-CHANGED: CLOSED - XSETTINGS Gtk/ColorScheme AND the org.freedesktop.appearance portal via the native D-Bus client shipped; live-proven by the portal color-scheme legs in make test-dbus]** |
| B-07 | **X11 DPI/HiDPI awareness — none.** No Xft.dpi, no RandR scale awareness; text renders at 1x. (Wayland is fully covered.) | missing | HiDPI X11/XLibre users get tiny UI | M | none **[VERIFIED-CHANGED: the audit row was WRONG - Xft.dpi + physical-size detection with integer scale shipped in 1.4.5; test_x11_hidpi_gui proves it end to end on a 192-dpi Xvfb]** |
| B-08 | **The D-Bus foundation.** No D-Bus client of any kind (by design, zero-dependency). Blocks: dark mode on Wayland (B-06), tray (A-11), notifications (A-12), portals (B-09), AT-SPI (B-10), idle inhibition on X11 (B-11). | missing | one architectural decision gates five features | L | none — new module, dlopen posture like GLX/fontconfig (libdbus-1.so.3 present in the target sandbox) **[VERIFIED-CHANGED: CLOSED by M-7 - implemented NATIVELY in raw C from the spec; no libdbus dlopen at all, no new dependency of any kind (docs/DECISIONS.md D-001); the dlopen speculation above predates the decision and never applied]** |
| B-09 | **Portal dialogs** — own widgets everywhere; no xdg-desktop-portal FileChooser fallback. | **DONE** [VERIFIED-CHANGED: C-1 shipped the client + the fallback — see the B-09 verified-status row] | functionally fine everywhere; required only under Flatpak-style sandboxes | M | none **[superseded: D-002's CORE-on-native-client decision; landed as C-1]** |
| B-10 | **AT-SPI2 interop** — internal a11y tree exists; Orca and other OS screen readers cannot see FDK apps (no D-Bus). Deliberate no-daemon-dependency posture so far. | **DONE (level 1)** [VERIFIED-CHANGED: S-8 shipped the bridge — see the B-10 verified-status row] | blind users on real desktops get nothing from FDK apps even though the internal tree is rich | L | none |
| B-11 | **Idle/screensaver inhibition** — no org.freedesktop.ScreenSaver, no idle-inhibit-unstable-v1. | missing | video players keep the screen blanking mid-play | S–M | none | **[DONE 2026-10-28: S-3 — both mechanisms, the deferred-release lifecycle]** |
| B-12 | **Cursor shape set** — 2 shapes (arrow/hand) + per-edge resize cursors; Wayland rides cursor-shape-v1 (all names available), X11 uses core `XCreateFontCursor` (no themes). | partial | standard shapes (text beam, wait, crosshair, grab) missing from the public surface | S | none |
| B-13 | **Drag icons + widget-level drop targets** — DnD is window-level (apps hit-test); no drag icon on either backend. | **DONE** [VERIFIED-CHANGED: S-9 shipped both halves — see the B-13 verified-status row] | usable but visibly rough; file managers do manual hit-testing today | S (icon) / M (targets) | none |
| B-14 | **Image formats** — stb_image decode (JPEG/PNG/BMP/TGA/PSD/GIF-static/HDR/PIC/PNM) + PNG encode + nanosvg. No animated GIF/WebP/APNG/AVIF; SVG without filters/masks/text. | partial | static raster + SVG solid; animated content missing | S–M | none **[DONE 2026-10-29 — animated GIF shipped (C-4, D-019); WebP/APNG fenced as future families; SVG filters/masks/text unchanged]** |
| B-15 | **GPU rendering on Wayland** — CPU SHM only; the GL lane is X11/GLX. | **DONE** [VERIFIED-CHANGED: D-1 shipped the EGL present lane (2026-10-30) — see the D-1 register row for the full account] | Wayland renders software (the toolkit's design center; the GL lane was an X11 perf answer) | L | none |
| B-16 | **Subsurfaces** — no wl_subsurface; no X11 child-window compositing. | **DONE** [VERIFIED-CHANGED: D-2 shipped both halves (2026-10-30) — see the B-16 verified-status row] | video-in-UI, offscreen compositing overlays | M | none — rides extension API v2 (shipped: FDK_EXT_API_VERSION 2, +7 exports) |
| B-17 | **Tablets / stylus / gestures** — no zwp_tablet, no pinch/swipe anywhere. | **DONE (gestures)** [VERIFIED-CHANGED: C-6 shipped pinch+swipe on the touch engine — see the B-17 verified-status row; tablets/stylus remain D-3 by scope discipline] | drawing tablets dead; pinch-zoom absent | M–L | none |
| B-18 | **Widget-set residue** — no date/time picker, no editable combo; everything else on the GTK4 checklist has an FDK counterpart (menus, lists, trees, tables via grid/tree, text editing with selection and undo, tooltips, pickers, scroll views, tabs). | partial | small, visible gaps | S each | none **[DONE 2026-10-29 — see the B-18 verified-status row; S-10]** |
| B-19 | **Session restore protocols** — prefs cover app state; no xdg_session/WM_SAVE_YOURSELF. | partial | niche on the Linux desktop today | M | none |

Also checked and fine: fractional scaling plumbing for extension
surfaces; clipboard manager interop basics; keymap handling (xkbcommon
on Wayland, evdev-offset contract both backends); key repeat;
scancode/codepoint dual identity; decoration negotiation; CSD drag;
expose/repaint contracts; startup tracing; the five-family packaging
and CI matrix; security posture (bounded reads, validated fonts,
64 KiB caps); threading model (documented UI-thread-only).

---

## The proposed 1.1.0 scope

### MUST — blocks calling 1.1.0 complete

| # | item | gaps closed | effort |
|---|------|-------------|--------|
| M-1 | **IME, both backends**: bind text-input-v3 (enable/surrounding-text/commit/preedit/deactivate on focus changes), deliver new IME events, wire Entry/TextView preedit (the groundwork API already renders it); X11: XIM preedit callbacks (`XIMPreeditCallbacks`) driving the same events. | B-01 | M | **[DONE 2026-10-28 — see B-01's register row]** |
| M-2 | **Text shaping level 1**: HarfBuzz via runtime dlopen (the GLX/fontconfig posture) + FriBidi (dlopen) + per-script font fallback chain through fontconfig; Entry/TextView/Label/markup all draw shaped runs. Color emoji explicitly NOT in M-2 (see C-3). **[DONE — shipped with the placement seam (one shaped structure every consumer reads), the 26.6-scaled stb-backed hb font funcs, cluster-attributed wrap/ellipsize, the shaped Entry caret + hit-test, and tests/test_shaping.c running both postures]** | B-02 | M–L |
| M-3 | **Touch basics**: wl_touch binding → touch events (slots, tap → click synthesis policy documented, two-finger scroll); X11 XI2 touch via dlopen'd libXi with the same event model. Gestures/tablets stay deferred. **[DONE 2026-10-28: the core slot engine with tap-click synthesis + natural-delta two-finger scroll + the multi_ever phantom-tap latch; wl_touch bound + the id→window map; XI2 touch via dlopen'd libXi with $FDK_NO_XI2; tests/test_touch.c pins every policy rule headless, both translate paths driven through the test_touch seam, XI 2.2 negotiated on the rig's Xvfb]** | B-03 | M |
| M-4 | **Window identity**: `_NET_WM_ICON` + xdg-toplevel-icon-v1 (set both, harmless where unsupported) + `_NET_WM_PID` + `WM_CLIENT_MACHINE` + `WM_WINDOW_ROLE`; `fdk_window_set_icon` API. **[DONE 2026-10-28: both setters + create-time PID/machine/role; X11 property round-trip rig; Wayland protocol wiring + honest no-op]** | B-04 (icons/PID part) | S |
| M-5 | **fdk-shell outputs**: **DONE (rc2, fdk-shell 0.2.0)** — live output events (ADDED/REMOVED/CHANGED; proven against sway's create_output/mode/unplug IPC) + per-output metrics (position, logical size, scale, physical mm — fdk_shell_output_get_info); configurable namespace (Wayland's zwlr namespace / X11's WM_CLASS res_class); `_FDK_WM_LAYER` set on every layer. X11 side: the screen as one output with real metrics; per-output X11 records land with M-6's RandR. | A-01, A-02, A-04, A-09 | M |
| M-6 | **X11 multi-monitor**: RandR output enumeration + change events in core (app-facing output API, shared with M-5's fdk-shell surface) + per-output docks in fdk-shell. | A-03, B-05 | M | **[DONE 2026-10-29 — see the A-03 verified-status row]** |
| M-7 | **The D-Bus foundation + dark mode**: **DONE (the foundation, rc2)** — a native client in raw C from the public specification (src/dbus/, 115 exported symbols), zero new dependencies of any kind, riding the FDK event loop; wire bytes pinned against captured reference-implementation output; fuzzed in `make test`; live-bus suite `make test-dbus` (real dbus-daemon: calls, errors, timeouts, match rules/signals, properties, name watching, introspection, close-from-callback reentrancy; CI makes skips fatal). Dark-mode following rides on top (next). **[VERIFIED-CHANGED: it no longer rides on top - dark mode (B-06) LANDED at d2a253f; M-7 is complete]** | B-08, B-06 | L |
| M-8 | **fdk-shell popup cascades**: **DONE (rc2, fdk-shell 0.2.0)** — fdk_shell_popup_create_child, any depth (xdg_popup-on-xdg_popup / the X window tree), family teardown sweeps children with their parent (the re-scan sweep — a saved-next chain dangles). | A-10 | S |
| M-9 | **The release gate**: every MUST lands with its rigs in this repo's style (in-test compositors/WMs, pixel readbacks, protocol traces), CHANGELOG + EXTENDING.md + README updates, extension API v2 bump if ops grow, five-family packaging version bumps, full local battery + CI green — and the owner's sign-off before any tag. | — | S |

### SHOULD — the polish tier (land if the schedule holds after MUST)

| # | item | gaps closed | effort |
|---|------|-------------|--------|
| S-1 | **StatusNotifier tray host** in fdk-shell (rides M-7's D-Bus) | A-11 | M–L | **[DONE 2026-10-27: trayhost, the rig, D-007 — should work; owner verification pending]** |
| S-2 | **Notification display** — org.freedesktop.Notifications client as an fdk-shell example/add-on on the overlay layer | A-12 | S–M | **[DONE 2026-10-27: notifyd, the rig, D-006 — should work; owner verification pending]** |
| S-3 | **Idle inhibition** — idle-inhibit-unstable-v1 (Wayland) + org.freedesktop.ScreenSaver (X11, rides M-7) | B-11 | S–M | **[DONE 2026-10-28 — see B-11's register row]** |
| S-4 | **Input regions / click-through** — wl_surface.set_input_region + XShape, exposed on windows and extension surfaces | A-06, A-07 | S–M | **[DONE 2026-10-29 — see A-06's register row]** |
| S-5 | **Cursor shape set** — full named set over cursor-shape-v1 + core font cursors | B-12 | S | **[DONE 2026-10-29 — see the B-12 verified-status row]** |
| S-6 | **xdg_activation + startup notification** | B-04 (activation part) | M | **[DONE 2026-10-29 — see the S-6 verified-status row; D-020]** |
| S-7 | **X11 DPI awareness** (Xft.dpi + RandR scale) | B-07 | M |
| S-8 | **AT-SPI2 level 1** — roles/names/states/focus/activate over M-7's D-Bus; text interfaces later | B-10 | L | **[DONE 2026-10-29 — see the B-10 verified-status row; D-023]** |
| S-9 | **Drag icon + widget-level drop targets** | B-13 | S+M | **[DONE 2026-10-29 — see the B-13 verified-status row; D-021]** |
| S-10 | **Widget residue** — date picker, editable combo | B-18 | S each | **[DONE 2026-10-29 — see the B-18 verified-status row]** |

### COULD — if the MUST/SHOULD tiers close early

| # | item | gaps closed |
|---|------|-------------|
| C-1 | Portal FileChooser fallback | B-09 | **[DONE 2026-10-29 — see the B-09 verified-status row; D-022]** |
| C-2 | AT-SPI text interfaces + relations export | B-10 | **[DONE 2026-10-29 — see the C-2 verified-status row; D-027]** |
| C-3 | Color emoji (COLRv0/v1 after HarfBuzz lands) | B-02 |
| C-4 | Animated images (GIF first, WebP via dlopen) | B-14 | **[DONE 2026-10-29 — GIF native, no dlopen; see the B-14 verified-status row]** |
| C-5 | fdk-shell runtime re-anchoring + auto-hide/reveal mode | A-05, A-08 | **[DONE 2026-10-29 — see the A-05/A-08 verified-status rows; D-024]** |
| C-6 | Basic gestures (pinch/swipe) on the touch event model | B-17 | **[DONE 2026-10-29 — see the B-17 verified-status row; D-025]** |

### DEFER — 1.2.0, by scope discipline

| # | item | gaps closed |
|---|------|-------------|
| D-1 | **EGL/GPU lane on Wayland** (SHIPPED 2026-10-30 — the full account is the D-1 verified-status row) | B-15 |
| ~~D-2~~ | Subsurfaces (needs extension API v2 parent-surface kinds) — **SHIPPED 2026-10-30 as the v2 subsurface API** | B-16 |
| D-3 | Tablets / stylus | B-17 |
| D-4 | foreign-toplevel management | — |
| D-5 | xdg session save/restore protocols | B-19 |

---

## ABI impact

**Nothing in this roadmap forces an ABI break.** Every gap closes
append-only, which docs/abi-policy.md already governs:

- New event types (IME, touch) append to the numbered `fdk_event_type`
  enum; new payloads append to the `fdk_event_data` union, which is
  always delivered by pointer from FDK and never allocated by
  applications — growth is binary-compatible for old callers.
- New APIs (icons, outputs, input regions, D-Bus surfaces) are new
  symbols on `libfdk.so.1`; the SONAME does not move.
- The extension API may grow ops fields (input-region hook); that is
  the documented `struct_size` append pattern and bumps
  `FDK_EXT_API_VERSION` to 2 — old add-ons (fdk-shell 0.1.0 binaries)
  keep working, exactly the compatibility case the versioning was
  designed for.
- `fdk_shell_layer_desc` grows (namespace, and whatever A-05 needs)
  behind its `struct_size` — same rule, no rebuild required for old
  callers.

The honest ABI caution in the whole plan: M-2 (shaping) changes what
pixels text produces. That is a rendering-behavior change inside the
same API, not an ABI change — but golden-image tests and the CJK
probe corpus should be re-baselined deliberately, and theme authors
should be told text metrics can shift by a pixel.

## What this audit could NOT assess from the sandbox

- **IME end-to-end**: no fcitx5/ibus daemon here. text-input-v3 can be
  tested against a scripted compositor-side peer (the sway headless
  rig can speak the server side), and XIM preedit against a synthetic
  XIM server — but real-input-method behavior needs the owner's
  desktop (labwc + fcitx5 recommended).
- **Real portal/AT-SPI daemons**: not installed. The D-Bus layer can
  be tested against `dbus-run-session` (present here) with mocked
  services; real xdg-desktop-portal and at-spi2 daemons are not
  available.
- **NVIDIA GL-lane behavior** (the 1050 Ti): only the owner's machine
  exercises the real driver path; the sandbox covers Xvfb/software GL.
- **Multi-monitor hardware**: emulated only — sway headless can create
  multiple outputs (wl side testable); Xvfb RandR emulated CRTCs give a
  partial X11 story. Real hotplug unplug-replug timing needs hardware.
- **Touch hardware**: XI2 events can be synthesized; wl_touch needs a
  compositor-side injector (wlroots virtual-pointer style rig).
- **CI status at the audit base** was not re-verified during the audit
  (API rate-limited anonymously); the push of this document includes
  an authenticated check. **[VERIFIED-CHANGED 2026-10-27: the
  verification pass DID check CI authenticated — and found the base's
  own run red (all five packaging jobs at d2bc0de: fdk-shell's
  hand-maintained Makefile version). Root-caused and fixed at
  4d86464; two further CI defects found and fixed the same day (the
  present-then-readback race that flaked alpine, and the void
  mirror outage). Job-level green verified per push since.]**
- **[VERIFIED-CHANGED 2026-10-27]** Two premises above are stale: the
  headless rigs CAN produce real input serials now (the
  fdk-wl-inject virtual-pointer/keyboard rig — A-13 was verified
  against sway with them), and labwc IS runnable in the sandbox
  (bar rig A3/A4 legs pass on it). Real-input-method (fcitx/ibus),
  real portal/AT-SPI daemons, NVIDIA, physical hotplug, and touch
  hardware still need the owner's desktop — TESTING-ON-DESKTOP.md
  is the checklist.

## The release bar, restated

1.1.0 gets called complete when the MUST tier is green in CI and on
the owner's two real desktops (labwc/Wayland, Openbox+NVIDIA/X11),
the SHOULD tier is either green or explicitly cut by the owner, and
the CHANGELOG reads like the audit above never had to say "missing"
about anything a real application hits in daily use. Until then the
version stays `1.1.0-rc2` (rcN bumps with the candidate's own
history - docs/versioning.md), no tags, no releases.
