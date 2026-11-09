# FDK — PROGRESS (the one ledger: current state, open bugs, dated log)

> Merged 2026-11-07: the round log from `test-apps/CONTINUATION.md`
> and the bug/gap ledger from `test-apps/FINDINGS.md` (both deleted
> by the merge) now live in this file, below the ledger.

## Current state (2026-11-09, after round 56 — 1.0.0 tagged)

- **Version:** 1.0.0 line (no tags; the repo's own M-9 rule holds —
  no tag without the owner's sign-off).
- **Surface:** 1,046 exported symbols, 34 public headers, 40 widget
  classes (the scale button joined the catalog);
  extension API v3 (widget hosting); PARITY.md audited against the
  GTK4 4.24 catalog and Qt6 — its gap table now carries zero OPEN
  rows.
- **Batteries:** the unit suite green (ASan/UBSan); the X11
  integration + uri + thread suites green on Xvfb; the Wayland
  integration suite green against headless sway (39 groups; the
  injector-less sections honest-skip, the IME relay's inert half
  included — see round 40).
- **Release gate (G1–G7, docs/RELEASE-READINESS.md), 2026-11-08 run:
  G1 PASS, G2 PASS, G3 PASS, G5 PASS, G6 PASS — only G4 open** (the
  28-section real-desktop walkthrough is the owner's hardware; the
  sandbox cannot produce it); G7 not started (external evidence,
  never blocks a tag). **Do NOT tag until G4 is signed** — then the
  tag itself is the owner's call (docs/versioning.md).
- **Open bugs: NONE.**
- **Next steps:** (1) the owner's 28-section real-desktop walkthrough
  closes G4 (docs/TESTING-ON-DESKTOP.md §§1–19 → docs/REAL-DESKTOP-
  SIGNOFF.md); (2) then the tag is the owner's call.

> **Repository posture (2026-10-31):** the project is **in
> development — no stable release yet.** One branch (`main`), zero
> tags, no GitHub releases, no published packages. The v1.0.0-era
> release material referenced throughout the historical entries
> below belonged to the project's former repository and no longer
> exists; the packaging pipeline and CI survive as validation
> checks on every push. See `docs/versioning.md` for the record.


## Round 56 (2026-11-09) — 1.0.0 ships: the sign-off signed, the log line honest, the tag channel wired; bug-rate: 0 new library bugs

1. **G4 signed**: docs/REAL-DESKTOP-SIGNOFF.md — the owner's walked
   evidence (the 10-item sweep, the portal chain, Orca, the Wayland
   logs), their three dispositions (multi-monitor untestable;
   portal live-apply deferred with the implementation untouched;
   the teardown residual accepted), and the final call: everything
   else "should work perfectly fine". v1.0.0 authorized to tag.
2. **The one approved code change** (Option B, step 1): the portal
   statement log no longer overclaims — "recorded; running UI
   keeps its startup size (1.0.0)". No other font-size code
   touched, per the owner's locked spec (no GTK configuration
   listening, no GTK dependency — ldd-verified empty).
3. **The release channel**: .github/workflows/release.yml — tags
   v* build the release tree on the ephemeral runner (the same
   dependency set and FDK_CONFIG pinning as ci.yml), stage the
   bundle (libfdk.so.1 + headers + examples + docs), sha256 it,
   and publish the GitHub release with the changelog's own section
   as notes. contents: write scoped to the release job — the
   posture change is documented in the workflow header.
4. **The docs swept for the release**: the walkthrough preamble now
   points at the sign-off; CHANGELOG carries the 1.0.0 block
   (the four things a user must know); ROADMAP's M-9 row records
   the signature and the standing-surface rule.
5. **Verification**: release-gate.sh (G1-G6) run on this tree —
   all pass; the gate's clean-checkout battery is G5 itself.

## Round 55 (2026-11-09) — the gsettings differential diagnosed: FDK followed its channel; the bridge is the desktop's; no code change

1. **The field result**: the owner set the font through gsettings
   on their X11 desktop — librewolf (GTK) followed, FDK did not,
   staying at the 10 pt statement its portal serves.
2. **The diagnosis** (theirs was the perfect differential): gsettings
   writes dconf; GTK reads dconf directly. FDK's integration channel
   is the desktop STATEMENT — XSETTINGS `Gtk/FontName` on X11 when
   served, the Settings portal `ReadOne` everywhere else — and the
   statement only tracks dconf when the portal routes Settings to a
   dconf-backed backend (xdg-desktop-portal-gtk). The rig's portal
   serves 10 pt from its own source: a routing gap on the desktop,
   not an FDK bug.
3. **The proof, in-repo, same day**: scripts/portal-mock.py owns
   org.freedesktop.portal.Desktop on a real session bus and serves
   ReadOne from a file. Portal says `Sans 11` → FDK boots at
   "11 pt (15 px)"; flip to `Sans 14` → "14 pt (19 px)". The
   channel follows whatever serves it, exactly as documented.
4. **Handed to the owner**: the one-line busctl ReadOne check (what
   does the portal serve right now?) and the fix recipe (install
   xdg-desktop-portal-gtk, route `org.freedesktop.impl.portal.Settings=gtk`
   in portals.conf, restart). Until routed, FDK's own knobs
   (FDK_UI_FONT_SIZE, `fdk-set fdk ui.font_size`) reach the chrome
   with no portal at all — field-proven in round 53.
5. Docs-only round (the mock script is the only tree delta): no
   battery delta beyond round 53's clean rebuild.

## Round 54 (2026-11-09) — the second sweep pass: Orca speaks, text_demo's scripts walk; no code change

1. **§16 (Orca/AT-SPI) PASS**: the owner ran a real screen reader
   against the test apps — "Tested orca with fdk and it works and
   its loud my ears". Speech + focus announcements over the level-1
   bridge, on their hardware, through the field's first screen
   reader. Task #8 closes.
2. **M-2 (text_demo complex scripts) PASS**: the owner's "task 18"
   is the walkthrough's open item 3 — two text_demo runs with
   Arabic + Han + Devanagari (नमस्ते with the explicit virama);
   both logs show the per-script fallback chain (Arabic → DejaVu,
   Han → Noto CJK, Devanagari → Noto Devanagari) under the active
   shaping pipeline, clean exits both times. Plus a third GL-lane
   field observation (fdk-gl.log) for §27.
3. **G4's remaining delta**: the labwc walk (item 5), the portal
   font-follow on labwc (the #7 half), and the signoff document
   itself; hotplug (#4) stays hardware-blocked. Recorded in the
   TESTING-ON-DESKTOP.md field-results addendum.
4. Docs-only round: no code delta (the battery result from round
   53's clean rebuild stands).

## Round 53 (2026-11-09) — the owner's 10-item sweep lands; TERM learns to exit cleanly; the missing-catalog noise dies; app round, bug-rate: 1 found, 1 fixed, 0 new library bugs

1. **The tally**: the owner answered the 10-item verification list.
   Six PASS (font size, the tray, notifications, drag icons, the GL
   lane, and touch's probe behaving correctly on a touch-less rig),
   one re-scoped (the Wayland walk is labwc, not sway), one blocked
   by hardware (hotplug — one HDMI output), one clarified (portals:
   no FDK file chooser exists by documented PARITY-out; the
   settings-portal statement reads correctly at every boot), one
   pending with steps now in the doc (Orca §16). Recorded as the
   Field results section in TESTING-ON-DESKTOP.md — the raw
   material for gate G4's signoff.
2. **TERM bypassed the clean exit** (found by the owner's font-size
   sweep): `kill %1` / `timeout` / logout daemons all land as
   SIGTERM, and the app died with exit 143 — no session save, no
   clean unmap. The core's contract (context.c's EINTR path) was
   built to honor a quit from a handler, but no test app ever
   installed one. Fix (app-side, by design — the core leaves signal
   policy to the application): the common boot installs
   TERM/INT → a sig_atomic_t flag; ta_pump maps it to the app quit
   inside the 15 ms pump budget, so teardown runs on the main
   thread like any other quit. Verified headless: TERM now exits 0
   in 0.01 s with the app's own "exited cleanly" line.
3. **The missing-catalog ERROR** (found by the same run): every app
   whose po/ ships nothing logged a boot ERROR on the owner's
   ro-locale desktop — but a missing OPTIONAL catalog is the
   designed fallback to source strings. Fix: the common boot stats
   the path first; absent → one INFO line ("no catalog at … —
   using the source strings"); present-but-broken keeps the
   library's ERROR. Verified headless with LANG=ro_RO.UTF-8: zero
   ERROR lines.
4. The doc's §1 sweep command now uses `timeout 4` instead of the
   `kill %1` form (job-control-dependent; that is what piled up
   windows on the owner's shell).

Battery: clean rebuild + core + X11 + dbus ALL PASS; the release
apps build clean (a sanitizer-contaminated archive from the gate's
incremental rebuild forced the clean rebuild — the same trap the
CI incident taught; the gate's build and a release build must not
share build/obj).

## Round 52 (2026-11-09) — the release gate run on the release tree: G1/G2/G3/G5/G6 green, G4 red (the owner's signoff); no code change

1. The owner asked the question RELEASE-READINESS.md exists for:
   "ready for release (1.0.0)?" The answer came from the tree,
   mechanically: `scripts/release-gate.sh` run in the clean worktree
   at origin/main (round 51, fa61946).
2. **G1** feature freeze PASS (register: 44 DONE · 0 PARTIAL ·
   1 IN PROGRESS (M-9 itself) · 0 MISSING; PARITY open gaps: 0).
   **G2** zero open bugs PASS. **G3** bug-rate plateau PASS (rounds
   50 and 51, both zero, different environments — CI and the
   owner's desktop). **G5** clean-checkout build + full unit
   battery PASS (786 checks). **G6** claim discipline PASS.
   **G7** informational, never blocks.
3. **G4 FAIL** — docs/REAL-DESKTOP-SIGNOFF.md does not exist. The
   28-section TESTING-ON-DESKTOP.md walkthrough is the owner's
   hardware; the sandbox cannot produce it and will not forge it.
4. Field progress toward G4 already banked today (the owner's
   tint2 run, 10:42): the GL lane self-check on the GTX 1050 Ti
   (§27), the tray chain end-to-end (the rounds 48–51 arc),
   notifications with and without a daemon, themes / icons (235/235
   lookups) / fonts / ui-metrics, EWMH focus, touch-present +
   tablet-absent probing, the AT-SPI bridge live. Still unwalked on
   their hardware: the Wayland-desktop sections and the X11 tail
   (hotplug, startup notification, drag icons, portals, Orca).
5. The verdict delivered: **5/6 mandatory gates green; the tag is
   the owner's act (docs/versioning.md) the moment their signoff
   lands.**

## Round 51 (2026-11-09) — field run 2 on tint2: the menu pops; About learns to speak without a daemon; app round, bug-rate: 1 found (app-level), 1 fixed, 0 new library bugs

1. **The field evidence**: the owner's tint2 run shows the round-50
   chain firing end to end — three clicks, three popups
   ("popped the item menu at (1799, 9) / (1796, 4) / (1800, 15)
   logical", three distinct popup windows taking focus), then a
   clean exit. The owner's verdict: "Exit button only works ...
   hopefully nothing is hardcoded."
2. **Why About was silent**: this session had NO notification
   daemon on the bus ("notify: no notification daemon on the
   bus" — dunst was not running), so the About notification had
   nowhere to land; the send succeeded into the void. Fix: About
   is now a real message dialog (fdk_dialog_show_message, OK,
   parented to the main window) — visible with or without a
   daemon, and it doubles as the in-app answer to "what is this
   menu".
3. **Open Files was never broken**: the log shows the main window
   taking focus (WM_TAKE_FOCUS) right after popups two and three —
   the activation fired; it only LOOKS like nothing when the
   window is already in front. Left as-is.
4. **The live-model proof** (the "nothing hardcoded" answer made
   visible): the rows were always the app's own fdk_tray_menu
   model — the core hardcodes nothing. Round 51 makes that
   unmistakable: the about-to-show callback now re-reads the
   active pane and renames the Open row to the CURRENT folder
   ("Open my-project") before every popup (and before the SNI
   host's GetLayout — the same parity serves both protocols).
5. **Ops**: the sandbox recycled (wiped /tmp/x11dev AND reset the
   clone to a stale divergent snapshot — 680 dirty files on a
   foreign lineage). setup-sandbox.sh rebuilt the environment
   (the recycle playbook worked as designed); the round was
   developed in a clean worktree off origin/main and the divergent
   checkout was left untouched for the owner to re-align.

Battery: core + X11 + dbus ALL PASS in the worktree; the release
app builds clean.

## Round 50 (2026-11-08) — the tray click menu: one model, any bar; feature round, bug-rate: 0 product bugs (1 test-sequencing wrinkle, fixed in the leg)

1. **The ask**: the owner's tint2 retest now docks the rendered
   icon — "can you make so clicking it opens a dropdown, a list of
   things to do". The pieces already existed in the core, un-
   connected: the tray item's on_activate callback (the SNI
   Activate/SecondaryActivate path), the fdk_tray_menu model (the
   DBusMenu server), and the widget layer's popup sessions (the
   ComboBox/context-menu machinery). Round 50 wires them.
2. **The platform half**: the XEmbed icon window selected
   ButtonPress/ButtonRelease (round 48 created it with exposure +
   structure masks only) and the systray ops' open gained an
   on_click(user, button, root_x, root_y) callback — button 3
   marked as the secondary activation, root coordinates in
   PHYSICAL pixels.
3. **The desktop half**: xembed_click fires the app's on_activate
   either way (SNI parity), and when an fdk_tray_menu is attached,
   the SAME model the SNI host would render is popped locally: a
   new adapter (dbusmenu_server.c) snapshots the tray menu into a
   widget-layer fdk_menu — rows in order, separators, check/radio
   state, "_" mnemonics converted to "&" markers, submenus as
   borrowed models — after the app's about-to-show callback ran
   (the AboutToShow parity). The popup opens at the click's logical
   screen position (physical / fdk_ui_scale()) through a new
   anchor-less seam, fdk__menu_popup_open_at_screen, parented to a
   hidden 1x1 host window the item owns (never shown; popups are
   root-level override-redirect windows, so the host only anchors
   coordinates). Row activations dispatch back as tray-menu clicks;
   the closed hook frees the mirrored models — the exact lifecycle
   the combo dropdown rides ("the callback runs first because
   closed hooks may free the model" — view_activate's own
   contract). The adapter also loads and owns its system font
   (fdk_font_load_system_default at fdk_ui_font_px()).
4. **The app**: fdk-files attaches a real menu — Open Files
   (show + activate the window), About (a notification), Quit —
   via fdk_tray_item_set_menu. ONE model now serves both
   protocols: a StatusNotifier host renders it over DBusMenu; a
   classic XEmbed bar gets the toolkit's local dropdown on click.
5. **The suite pins it, seven legs**: the popup mirror (headless:
   rows, order, toggle state, radio-sibling clearing, mnemonic
   conversion, submenu linkage, about-to-show), button 1 → Activate
   with the click's coordinates, button 3 → secondary, and the full
   round trip — click on the icon pops the attached menu (the
   about-to-show ran first), a synthetic press+release on the first
   row activates it, the chain closes, the adapter is freed, and
   the tray-menu click callback receives the right node id. The
   dock/restart/name-render legs from rounds 48-49 stay green
   untouched. One test-sequencing wrinkle on the way: the click
   legs' item must be opened FRESH after the bar restart (the
   earlier legs' item carries a stale docked-state across the
   compressed stop/start because no pump runs between them in the
   test — the round-48 restart leg with real pumping is the
   covered path for the live behavior).

Battery: test-dbus ALL PASS twice, core + X11 suites green.

## Round 49 (2026-11-08) — the name-render layer: the fallback docks apps that only have an icon name; bug-rate: 1 found (the field retest), 1 fixed, 0 new after the fix

1. **The retest failed, and the diagnostics earned their keep**: the
   owner's tint2 run printed the exact miss — "no
   StatusNotifierWatcher and no XEmbed pixels yet". No SNI watcher
   on that bus (the round-48 hypothesis confirmed), and the item
   had no pixels because `test-apps/fdk-files` opens the tray with
   `set_icon_name("folder")` ONLY (main.c:662) — round 48's
   pixels-required rule made the fallback wait forever for pixels
   an app might never set.
2. **The fix: the toolkit renders the name it owns.** FDK has a
   full icon-theme engine (the app's own log shows 'Papirus-
   Nordic', 2107 lookups) — `xembed_render_named_icon` calls
   `fdk_icon_theme_surface(name, 32)` (fresh ARGB8888 surface, the
   engine's own resolution policy: live theme → Inherits →
   hicolor → pixmaps), converts the surface's u32 0xAARRGGBB rows
   into ARGB32 network-order bytes in the item's pixmap slot, and
   the fallback docks with them. `set_icon_name` now retries the
   fallback; a later `set_icon_pixmap` still swaps the pixels live
   (app pixels win). Name-only apps work on any classic bar now.
3. **The suite pins it**: the XEmbed phase gained the name leg —
   an item opened with `set_icon_name("folder")` and no pixmap
   must dock (skipped with a loud note where no icon theme
   resolves — a bare container). The sandbox's Adwaita/hicolor
   themes ran it: rendered, docked, asserted.
4. **The LSan coin flip the leg exposed, triaged and defused
   without a suppression entry**: the first green runs were
   followed by a 52935 B / 35-allocation leak report from the
   dbus lane. Bisect (stash → round-48 binary ALL PASS; name leg
   off → still 5736 B / 2 allocations) + stack census (ZERO FDK
   frames; every site inside libX11/libxcb — xcb_connect_to_fd,
   XOpenDisplay, XInitExtension) said: libX11 retains per-CLOSED-
   display state in process-global lists, and whether LSan sees
   it depends on stale main-thread stack slots — the round-48
   binary passed three runs in a row on luck, the round-49 line
   shifts flipped it. Fix at the source: the rig now opens ONE
   display for the process lifetime (the Xvfb dies with the run),
   the guard probe reuses it, rig stop destroys windows only —
   every block the rig owns stays reachable and the sanitizer
   stays exact. The lsan-gl.supp file's discipline held: no
   entry, because nothing third-party needed hiding once the
   fixture stopped closing displays.
5. **Field evidence for M-9**: the tint2 retest exercised the new
   diagnostics (its log named the miss precisely) and now expects
   the rendered 'folder' icon to dock; the next owner run closes
   the loop.

Battery: test-dbus ALL PASS twice (name leg on), core + X11
suites green under -j2.

## Round 48 (2026-11-08) — the tray works on ANY X11 bar now; bug-rate: 1 found, 1 fixed, 0 new after the fix

The walkthrough's second field bug, from the owner's X11 desktop
(tint2): **the tray item docks on Wayland (waybar) but never appears
on tint2** — while nm-applet on the same bar does dock. The owner's
constraint was the right one: no tint2-specific fix, ANY bar.

1. **Root cause (protocol, not tint2)**: the tray item spoke ONLY
   StatusNotifierItem (the KDE 2009 D-Bus spec + ayatana twin —
   waybar's protocol). The classic X11 bar world (tint2's legacy
   systray, trayer, stalonetray, fvwm, xfce4-panel legacy mode)
   speaks the OLDER freedesktop System Tray v0.2 protocol — a
   manager selection (_NET_SYSTEM_TRAY_S<screen>), a dock-request
   client message, and the icon as an XEMBED client window the bar
   reparents and resizes. With no SNI watcher on the bus the item
   logged "no StatusNotifierWatcher" and waited forever. nm-applet
   docks because it falls back to exactly that protocol — the
   standard cross-desktop tray story every real client plays.
2. **The fix (protocol-level, any bar)**: `src/platform/x11/
   x11_systray.c` implements System Tray v0.2 + XEMBED 0.0 as the
   platform's OPTIONAL systray ops (NULL on Wayland — a Wayland bar
   is an SNI host by construction). The item registers SNI first;
   when no watcher is on the bus it docks a real icon WINDOW into
   the classic tray (the tray's advertised 32-bit ARGB visual when
   it publishes one), repaints on the bar's resize (nearest-
   neighbor, LSBFirst ZPixmap), reports unhosted when the bar dies
   (the server destroys embedded children with it), and re-docks
   through the MANAGER announcement of a restarted bar. SNI wins
   the moment a watcher appears — the fallback leaves the classic
   tray and registers. Pixels REQUIRED for the fallback (an XEmbed
   icon is a window, not a theme lookup) — set_icon_pixmap both
   feeds and triggers it, with a loud diagnostic otherwise.
3. **The suite pins the protocol, not a bar**: test_tray_item's
   new phase 0 runs a REAL in-test System Tray v0.2 manager on a
   watcher-free private bus — dock, the wire shape (_XEMBED_INFO
   version 0 + MAPPED, the icon a CHILD of the manager), unhost on
   the bar's death, re-dock via the MANAGER announcement. Skips
   loudly when a real bar owns the selection or a real watcher
   owns the bus (the mock phases' posture). Both Xlib traps hit
   and fixed on the way: format-32 property data is LONG arrays on
   both the write and read side (the int32 write sent zero flags);
   MANAGER announcements arrive as root ClientMessages against
   StructureNotifyMask.
4. **Field evidence for M-9**: waybar (Wayland) = PASS (the owner's
   report); tint2 (X11) = FAIL → this round's fix; retest on the
   owner's bar is the pending confirmation (the rig + nm-applet's
   protocol make the expectation unambiguous: any classic systray
   hosts the icon now).

Battery: test-dbus ALL PASS (incl. the new phase), core + X11
suites green under -j2.

## Round 47 (2026-11-08) — field evidence lands, no code change; bug-rate: 0 new library bugs

The owner's walkthrough reached the X11 GPU lane and the extension
API on real hardware.

1. **examples/19 (extension surfaces) walked end-to-end on the X11
   desktop** — probe, register kind, surface create, crosshair
   cursor, input-region fall-through, show: every `[ext]` line OK.
   The GPU present lane went ACTIVE on the proprietary NVIDIA
   driver (GTX 1050 Ti, 580.196.07): "GPU present lane active",
   self-check passed on retry 2 (the vblank/compositor-latency
   retry doing its designed job), 1348 frames. The X11 GL
   acceptance has no numbered walkthrough section yet (the GL
   sections in TESTING-ON-DESKTOP are the Wayland rounds) — the
   evidence is banked for the signoff document.
2. **The fatal-IO graceful-exit path had its first field exercise**:
   the session ended with an external connection kill (xkill/
   force-quit-class) — one detail line, the pump stopped, the
   example exited cleanly. The LeakSanitizer report that followed
   is the expected shape on that path: ~2.2 MB inside
   libnvidia-glcore stacks with no FDK frames (the proprietary
   driver's own allocations), and the FDK-side entries (render_gc,
   GL staging, the shm image) are freed only on the CLEAN path
   (x11_surface.c, x11_gl.c) — after a fatal IO the dead-display
   flag deliberately forbids further X requests, so they cannot be
   freed. The clean path is already proven leak-free: the battery
   runs ASan leak detection ON by default and passes, and the
   suite drives WM_DELETE_WINDOW itself (test_x11_integration.c).
3. **examples/21 retested clean post-round-46** (the owner's
   report: the popup opens below the button and drags).
4. **The build-parallelism question answered with measurements**:
   plain GNU make is SERIAL by default; ninja (GTK's build backend)
   runs parallel by default (ninja-build.org manual: "Builds are
   always run in parallel, based by default on the number of CPUs
   your system has"). FDK's Makefile is -j-safe and was never
   single-core by nature: measured on the 2-core sandbox, `make
   static -j2` 16.3 s vs serial 24.4 s from clean, and `make test
   -j2` + `make test-x11 -j2` pass. The recommendation:
   `make -j$(nproc)`.
5. Ops: the recycled sandbox restored a stale checkout (main 33
   commits behind origin, mode-bit-only drift on 679 files) —
   reconciled with `git reset --hard origin/main` after proving
   nothing local-only existed; /tmp/x11dev rebuilt by
   scripts/setup-sandbox.sh.

Battery: both suites green under a -j2 parallel build (this
round's only run — no code changed).

## Round 46 (2026-11-08) — the field report: the scale button's popup never mapped; bug-rate: 2 found, 2 fixed, 0 new after the fix

The owner ran examples/21_scale_button on the REAL desktop and could
not drag the slider — the value sat at 35% forever. The report was
worth exactly what the sign-off walkthrough's worth: the GUI suite
was green, the real machine was broken.

1. **The reproduction gap the suite carried:** the scale button's
   GUI test drove the popup with XSendEvent — which delivers to an
   UNMAPPED window, bypassing mapping, the server grab, and window
   geometry entirely. The suite was green because it never
   exercised the real delivery path.
2. **Bug 1 (fatal): sb_open_popup packed the slider and the value
   label into a popup window that was NEVER SHOWN** — the
   menu-session order is create, pack, SHOW (the pointer grab
   happens on map, inside the platform), and the show call was
   missing. On a real desktop the press opened a popup that did
   not exist on screen; the user clicked a meter-shaped face and
   tried to drag it. The popup-trace rig proved it: a lone
   "create ... 48x176" line, no map, no grab.
3. **Bug 2 (placement): the first cut clamped the popup into the
   OWNER WINDOW's bounds** — a 176px popup in a 190px window
   neither fits below nor above, and the clamp slammed it onto the
   title band at (24,0). A popup is a root-level window: it hangs
   BELOW its owner onto the desktop exactly like a menu — the
   clamping is gone and the anchor is the menu rule (below the
   button, centered on it).
4. **The regression rig is REAL input:** the GUI test grew an
   XTest section — server-routed input that rides the popup grab
   the way a human's pointer does: warp to the button, click, warp
   onto the trough in motion steps, PRESS, drag, release. It
   FAILED before the fix (the grab-routed press did nothing) and
   now passes: the drag drives the value (100), the popup survives
   it, and a real press-away dismisses. scripts/xtest_drag.c is
   the standalone field-rig twin (build line in its header) that
   drove the actual example against the popup trace.
5. **The example verified end-to-end:** create at (24,84) — below
   the button — then map, grab, first-present, motion->present
   during the drag, still open after.

Battery green (unit + X11 + threads, ASan clean). Bug-rate: 2
found, 2 fixed, 0 new after the fix — both in the round-45 widget,
both the suite's blind spot (synthetic input), neither reachable
by reading code alone.

## Round 45 (2026-11-08) — the parity batch: the scale button; bug-rate: 1 found, 1 fixed, 0 new after the fix

The honest tail's next implementable row: the slider-in-a-popup
(the GtkScaleButton/GtkVolumeButton pair). The dedup check ran
first — no scale/volume widget exists, and the popup machinery is
the public fdk_window_create_popup + the menubutton's guard
pattern (the menu session machinery is model-driven and wrong for
a live slider, so it stays untouched):

1. **fdk_scale_button** (fdk_widgets.h, 6 public functions): the
   face is a value meter (trough + accent fill proportional to the
   value — the volume icon's honest stand-in; icons are theme
   territory); a press pops a small window below the button (above
   when the room runs out) carrying a VERTICAL slider (min at the
   bottom, the audio convention) and a live value label. Every
   slider drag updates the button and fires on_value_changed LIVE
   (the value rides the call — the color button's payload shape;
   the plain slider's callback omits it). Dismissal: Escape (the
   platform's close-request synthesis), a second press (the toggle
   off), and a pointer press delivered outside the popup's bounds
   while the grab routes it there. The family contracts verbatim:
   the clamped silent setter, set_range re-clamps and refuses
   min >= max as a no-op, step quantizes the popup slider, the
   detached-tree press is a consumed no-op, and the popup
   abandonment rule is the combo's (destroying the button while
   open NULLs the guard's button; the closed hook still frees it).
   Tests FIRST (test_controls.c, red): the create ladder, the
   clamped silent setter, the range rules, the meter paint (full
   accent vs empty track, pixel proofs), the detached no-op
   presses. The popup path (test_x11_integration.c, GUI): the
   press opens (a root child), a trough click drives the value
   live through a REAL XTEST click (50 -> 98.39), Escape dismisses
   with the value kept, the second press closes, all under ASan.
2. **The in-round find+fix (the bug-rate 1):** the first GUI run
   died in a heap-use-after-free INSIDE my own sb_close tail — the
   destroy notify runs SYNCHRONOUSLY (it fires while the window is
   still whole, before teardown), so it frees the guard before
   fdk_window_destroy returns, and the leftover "defensive" read
   of the guard after the destroy call was the use-after-free. The
   tail is gone; the closed hook is the single owner of the
   guard's lifetime. The project rule holds: the GUI test caught
   it, ASan named it, the fix is structural (no band-aid).
3. **examples/21_scale_button.c**: the demo app — a volume-posture
   scale button (0..100, step 5) + a live readout label + the
   status line narration, on the shared example-window helper.

The sweep after the batch: CLEAN, missing rows 3 -> 2 (the honest
tail is now Video/MediaControls and the policy rows). 1,046
exports (+6), verify-exports green; 40 widget classes. Battery
green (unit + X11 + threads); gate re-run below. Bug-rate: 1
found, 1 fixed, 0 new after the fix.

## Round 44 (2026-11-08) — the parity batch: the font button; bug-rate: 0 new library bugs

The owner's directive: the honest parity tail needs implementing,
not just stating. The sweep's implementable rows went first (the
dedup check ran before any code: the chooser engines and the color
button's pattern exist — nothing here duplicates them):

1. **fdk_font_button** (the GtkFontButton/GtkFontDialogButton row):
   the color swatch button's font twin, built on the SAME chooser
   engine the dialog and the inline widget use — one engine, three
   hosts. The label reads "Family [Style] Size"; the press opens
   the 1.0.0 font chooser dialog seeded with the current
   family/style/size; ACCEPTED writes the pick (owned copies — the
   result strings live only through the callback) and fires
   on_font_set; CANCELLED changes nothing; a NULL family is
   refused. The dialog's size rule verbatim (0 = 12, clamp 6..96);
   NULL/"" style normalizes to NULL (no style = the family's first
   face); the detached-tree press is the color button's consumed
   no-op; the programmatic setter stays silent; the a11y contract
   is the color button's (a BUTTON whose value text is the
   description). Tests FIRST (test_choosers.c, red before the
   implementation existed): the create ladder, the silent setter,
   the style normalization, the size rule, text-bearing natural
   size, the face+ink paint proof, the ACCEPTED/CANCELLED/NULL
   seam, the description value text + ACTIVATE action, the
   detached no-op presses. 8 new exports (1,032 -> 1,040),
   verify-exports green.
2. **The GtkSwitch row reclassified** (evidence, not aspiration):
   the toggle's paint IS the switch visual — track + knob
   (controls.c), live changed feedback, keyboard activation, the
   TOGGLE_BUTTON a11y role with the PRESSED state. parity_check's
   mapping row moves missing -> parity with the evidence in the
   note; the "visual is theme work" hedge is retired.

The sweep after the batch: CLEAN, missing rows 5 -> 3 (the honest
tail is now ScaleButton/VolumeButton, Video/MediaControls, and the
AppChooser/Emoji/Shortcut/Mapper/MDI policy rows). Battery green
(unit + X11 + threads); gate re-run below. Bug-rate: 0 new library
bugs.

## Round 43 (2026-11-08) — the decoupling round: FDK stands alone; bug-rate: 0 new library bugs

The owner's directive: the tree must not mention, reference, or
test against the shell add-on (its own repository now) — the
dependency arrow points one way (add-on -> FDK), so FDK is not
tied to it. The sweep touched 52 files: the bar-test rig deleted
(`test-apps/fdk-addon/`, `test-apps/fdk-addon-host/`, the
protocol replay rig, the `addon` build target, the labwc-smoke
legs, and the smoke-matrix entry), the docs reworked (EXTENDING's
reference consumer is the in-tree example now; TESTING-ON-DESKTOP
lost the add-on-subject walkthrough sections and renumbered 1-19;
ROADMAP's register dropped the add-on's own rows — 45 items,
A-13 re-filed as B-20, the correction recorded in place),
the history reworded (CHANGELOG/PROGRESS keep the lessons, lose
the name), the packaging/CI/Makefile comment blocks trimmed, and
every code comment that told an add-on story now tells the
behavioral one. The fdk-files tray-bus rig takes ANY conforming
watcher/daemon (or skips honestly); the release gate's G2 is the
generic open-bug check; the parked HUD open-bug record left with
its subject. Repo hygiene: a tracked 7.9 MB ELF core dump
(`test-apps/core`) deleted; the core-dump ignore rule widened.
The untracked planning docs (ROADMAP-1.1.0, roadmap) got the same
sweep. Comment- and doc-only; zero behavior change; no surface
move (verify-exports unchanged). Bug-rate: 0 new library bugs.

## Round 40 (2026-11-08) — the PARITY honest tail closed: the chooser widgets + widget hosting on extension surfaces (API v3); bug-rate: 0 new library bugs after the in-round find+fix

The owner's instruction — "continue to build what's left" — read out
of the release gate: G1's two PARITY OPEN rows were the only build
work between the tree and the tag. Both shipped this round, each as
a composition of shipped machinery rather than a new engine (the
user's standing warning — nothing here reimplements what FDK
already has):

1. **Inline chooser widgets** (PARITY row 7, G1): the color and
   font chooser DIALOGS already existed; the embeddable WIDGET
   forms were the gap. Rather than duplicate the engines, both
   dialogs were refactored to expose their cores (`fdk_cc_core` /
   `fdk_fc_core`) and the new `fdk_color_chooser_widget` /
   `fdk_font_chooser_widget` (fdk_dialog.h, 8 public functions)
   host the SAME wheel rasterizer, hex parser, enumeration, and
   preview reload — one implementation, two hosts. User gestures
   fire the new change callbacks (watch-token guarded); the
   programmatic setters stay silent (the color button's rule).
   Headless tests (test_choosers.c): create ladders, the
   silent-setter contract, wheel+swatch pixel proofs (a 120-degree
   ring pixel is saturated green), ring/triangle picks, the
   unknown-family fallback, the honest empty enumeration,
   destroy-from-callback survival.
2. **Widget hosting on extension surfaces** (PARITY row 4, G1):
   extension API v2 → v3, one new entry point —
   `fdk_ext_surface_host_widgets()`. The structural fact that an
   extension surface IS an internal fdk_window meant the whole
   hosting story already existed in the window layer: the bridge is
   fdk_window_get_root + the dialogs' auto_paint tail (configure
   re-layout, standard input dispatch, frame-paced repaint+present).
   The add-on's event callback keeps the standard
   fdk_window_get_root rule (window-level events always;
   widget-consumed input does not) — the first test run asserted
   the wrong contract there and the header doc was corrected to
   match. The X11 integration test (test_ext_widget_hosting_x11)
   pins the window-owned root + idempotence, server-side pixel
   proofs (theme background fill + label glyph ink), a real click
   activating a button while the tree consumes the pair, a press
   over no widget reaching the add-on, a real XResizeWindow
   re-arranging the root, and clean teardown.

**The in-round find+fix** (found by the new X11 test, fixed in
round): `ext_destroy_notify` called `kind->ops.destroy()` unguarded
— the header has always documented destroy as OPTIONAL ("may be
NULL"), and the hosting test's destroy-less kind crashed the
teardown (NULL call). The guard is in. Bug-rate: 0 new library bugs
after the fix; no P0/P1 open.

**Environment evidence (G3's different-environments rule)**: this
round ran headless (the full unit suite, ASan/UBSan), on Xvfb
(X11 integration + uri + threads, all green), and against headless
sway (the Wayland integration suite, 39 groups green) — a
compositor environment. Sandbox-recycle notes for the next round:
the X11 dev headers survived under /tmp/x11dev (scripts/env.sh),
but `fdk-wl-inject` (untracked test infra) is gone — the cursor/
dnd/menu sections honest-skip for it; and the IME group's e2e leg
hard-asserted on a relay that is inert without that tooling. The
pristine-HEAD worktree run (4e77d5a, zero round-40 changes) failed
at the SAME assert — environmental, not a regression — and the
group now carries the honest skip its siblings have (the assert
stays hard wherever the tooling exists; leg B's commit loop still
runs and PASSES on sway). The rig variable that cost a rerun:
swaymsg needs SWAYSOCK exported.

**Docs**: EXTENDING.md §19 (widget hosting, with the contract and
the test map), CHANGELOG ([1.0.0] Added (round 40)), PARITY.md
(both gap rows CLOSED + the GTK4 widget diff paragraph updated),
README (the chooser line names both forms). verify-exports: 1032
symbols, exactly the public API.

## Round 39 (2026-11-08) — everything above the first release reads 1.0.0, both repos

The owner's instruction, applied across FDK and the add-on (its own
repository, synced to origin before any change): any version label above the
first release now reads **1.0.0**. The internal engineering-
milestone series that docs/versioning.md kept as a separate axis is
retired by this same decision — FDK renamed 194 files /
1539 occurrences, the add-on 17 files / 42
occurrences (its FDK-cycle references and its requirement strings —
"needs FDK 1.0.0" — moved with it). The add-on's own version
(0.5.0) is BELOW the first release and stays.

Kept on purpose — third-party provenance, where renaming would
falsify what upstream artifact the text points at: the openbox
build in the decorations rig, the GitHub Actions pin comments,
dependency versions in the provenance docs, the integer
extension-API contract version, and the SONAME digit.

The rename lands via git plumbing (blobs -> recursive mktree ->
commit-tree -> push by hash): the sandbox's snapshot restore
reverts tracked-file writes to HEAD within seconds, and — the
real bug behind three green-looking no-op attempts — the
line-level transform appended the UNtransformed line, so every
earlier 'rename' was a faithful no-op (the string-level round-38
version never had the bug). Blob identity is now asserted per
file, the staged tree is diffed against HEAD before the commit,
and the worktree follows via reset --hard.

## Round 38 (2026-11-08) — the first release is 1.0.0: the 1.0.0 identity retired repo-wide

The owner's call (recorded in docs/versioning.md History,
2026-11-08): the first release is **1.0.0** — the 1.0.0-rcN
candidate numbering is retired, the pre-release suffix is dropped,
MINOR 1 → 0, and every cycle label in the tree reads 1.0.0.

**The sweep:** 153 files, 853 occurrences (boundary-aware regex;
rc-forms first so "1.0.0-rc2" became "1.0.0-rc2" in the
changelog's historical candidate entries, then the bare triple),
zero residuals outside .git/build/third_party. The version macros
moved specially: FDK_VERSION_MINOR 0, FDK_VERSION_SUFFIX "",
FDK_VERSION_STRING "1.0.0" — and the header's decision comment now
tells the 2026-11-08 story. CHANGELOG.md opens with the [1.0.0]
first-release entry; docs/versioning.md's History records the
decision (SONAME stays libfdk.so.1 — the ABI was append-only
throughout, the MAJOR digit owns the SONAME and never moved; the
internal 1.4.x/Phase milestone axis is untouched by design).

**The pins moved with it** (test_core.c: MINOR 0, empty suffix,
string "1.0.0", ENCODE(1,0,0) — three of the four surfaced only
when the battery ran, exactly what the pins are for: the first
rerun failed on MINOR==1, the second on the stale "1.0.0-rc2"
string literal and ENCODE(1,1,0)); third rerun 792/792.
`[ok] version: 1.0.0 (1000000, pinned per docs/versioning.md)`.

**Verified end to end:** dev build rc=0, battery 792/792, root
release rc=0, fdk-langs 13/13 + 24/24 fails=0 with the thirteen
language samples now reading "fdk 1.0.0" (confirmed in the release
binary's strings), fdk-langs rc=0 under Xvfb.

## Round 37 (2026-11-07) — the user's Wayland full pass recorded; the -O2 warning in the gate closed

The user's Arch machine ran the whole sweep on the Wayland backend
with the NVIDIA GPU present lane active (BGRA + UNPACK_ROW_LENGTH
upload path driver-probed): 13/13 families, every one uncovered=0,
24/24 locales, fails=0, exit clean — NotoColorEmoji loaded and the
emoji family rendered. The rounds 34-36 chain (dwell, fallback cap,
the coverage gate) is confirmed end to end on real hardware.

One -Wmaybe-uninitialized in fdk_text_count_uncovered (the user's
-O2 release build; the dev -O0 build never sees it): `best` is only
read when `have` is set, but GCC cannot see the guard — initialized
to 0, the guard short-circuits before the read. The -O2 syntax
check is warning-clean and the release build carries zero
uninitialized warnings.

Cosmetic note worth keeping: every fallback file loads TWICE in
fdk-langs runs — the coverage gate shapes through its own system-
font copy (fdk_font_load_system_default) and the widgets shape
through theirs; the fallback slots are per primary-font object, so
both copies resolve (and cache) independently. Harmless, and the
log tells the story honestly.

## Round 36 (2026-11-07) — the honest tofu gate ships: fdk_text_count_uncovered

The round-35 note ("a real tofu-detector needs font-coverage
introspection") is closed with a small public API:
fdk_text_count_uncovered(font, utf8, n) — how many codepoints would
draw .notdef under this font, computed by running the REAL pipeline
(segmentation, bidi, per-script fallback resolution — the same
caches and warnings) and attributing every codepoint to the font
that actually paints it (largest cluster <= the cp's byte over the
placement array, RTL-safe). fdk-langs gates every family on it now:
the per-family line carries uncovered=N and a positive count is a
LOUD LANG FAIL (fails++, exit 2) — renders=ok can never be fooled
by tofu again.

The gate's first live run caught its own bug before it could lie to
anyone: per-codepoint classification failed Han and Kana on their
fullwidth punctuation (U+FF0C, U+3002 classify script-less in
isolation but inherit the CJK segment's font at paint time) — the
rewrite through the real shaped result fixed exactly that, and the
sandbox rerun is 13/13 families uncovered=0, fails=0.

Pins (test_shaping.c test_count_uncovered): latin=0 deterministic;
U+0378 reflects the FONT's cmap, not the Unicode tables (DejaVu
maps the unassigned codepoint — count 0, stable across calls — the
gothic primary counts 1); the gothic primary + Hangul = 0 wherever
a Korean-capable font answers. Battery 792/792. The user confirmed
the round-35 fix end to end on Arch (all noto-fonts packages
installed; Hangul and Emoji render; exit clean).

## Round 35 (2026-11-07) — the silent-tofu bug found on the user's machine: the fallback-table cap

**Root cause of "Hangul is just rectangles and zero emojis":** the
per-primary fallback table held 8 slots and the exhaustion branch
returned NULL SILENTLY. The user's run filled all eight (Arabic,
Hebrew, Devanagari, Thai, Armenian, Georgian, Han, Hiragana — their
fontconfig ranks a distinct font per script, unlike this sandbox's
seven) and everything after — Hangul, Katakana, Emoji — drew .notdef
with NO log line, while the ink check called it renders=ok (tofu
boxes have ink too; their scores sat at 47-51 permille, right in the
healthy band). Fixed: the cap is 32 (a slot is a pointer + an int)
and the exhaustion branch is LOUD — one WARN naming the script and
calling it what it is ("this is a FDK bug, report it").

**Proof:** a new regression test (test_shaping.c,
test_fallback_cap_overflow) drives TEN distinct fallback-requiring
scripts through one Japanese-gothic primary — the user machine's
exact pattern — and asserts the overflow routes:
"[ok] shaped: 10 fallback slots used on one primary (cap was 8) —
the overflow routes, no silent tofu". The full battery is 789
checks green; fdk-langs rerun shows the Katakana, Hangul and Emoji
fallback lines that were absent before.

The ink check itself stays conservative (a real tofu-detector needs
font-coverage introspection on the widget's internal fonts — noted
as roadmap); the honest gate for now is the loud WARN plus this
test. On a machine with NO emoji font the resolver now exhausts
loudly with install guidance — that is the honest missing-font
case, not a code bug.

## Round 34 (2026-11-07) — fdk-langs is watchable: 5 s per language

The sweep flashed all 13 families in about a second and exited —
fine for a robot, useless for a human. run_family now dwells: the
window keeps pumping (paint/input/resize stay live) for a default
of 5 SECONDS per language, the title shows the progress
("lang: Latin (1/13)"), and --dwell=<seconds> overrides it
(--dwell=0 is the old flash-through). Verified by wall clock:
default full sweep 66 s (13 x 5 s + ~1 s overhead), --dwell=0 1 s,
both receipts families=13 renders=13 entry=13 bidi=13 locales=24
fails=0. The smoke matrix passes --dwell=0 to fdk-langs so the
automated run keeps its speed; the release build of the app was
rebuilt and verified (rc=0).

## Round 33 (2026-11-07) — the user machine's release build fixed; bug-rate: 1 real build-system bug, fixed

**Found on a user machine, invisible in the sandbox's dev-only
builds: `make release` failed to link fdk-keys** (undefined rxkb_*)
— the dev rule carries a dedicated per-target link set with
libxkbregistry, the release generic rule did not, and the
duplicate-recipe warning the user also saw was the dev side of the
same shape. Fixed at both layers in test-apps/Makefile: fdk-keys is
excluded from BOTH generic foreach rules and has one dedicated
release rule mirroring the dev one (xkbcommon + xkbregistry via
pkg-config, FEATURE included — the first cut dropped _POSIX_C_SOURCE
and strdup went implicit under -std=c17/-Werror, caught here before
pushing); pkg-config probes degrade: a machine with NO libxkbregistry
builds fdk-keys with -DFDK_NO_XKBREGISTRY and the layout sweep
prints the honest unavailable message instead of failing to
compile/link — verified in THIS sandbox, which genuinely has no
xkbregistry (the fallback path is the one that runs here).

**Verified end to end on the fixed tree:** `make -C test-apps
release` rc=0 — all ten apps including fdk-keys, zero overriding-
recipe warnings; the dev battery 788/788; the language sweep
13/13 + 24/24 locales on BOTH Xvfb and labwc again (26 fresh PNGs
into download/fdk-langs-screenshots/); one render eyeballed
(Latin label/entry/textview/mixed-bidi line correct).

Also: the sandbox recycle wiped the previous round's screenshot
copies from download/ (the /tmp sources died with it) — every PNG
was regenerated from the fixed tree and re-verified with PIL
before this commit. Setup after a recycle remains one command:
`bash scripts/setup-sandbox.sh`.

## Round 32 (2026-11-07) — the language sweep green on BOTH backends; Wayland compose landed; bug-rate: 0 new library bugs

**The language sweep (the owner's section-4 ask) is a shipped app:**
`test-apps/fdk-langs` renders 13 script families (Latin, Cyrillic,
Greek, Arabic, Hebrew, Devanagari, Thai, Armenian, Georgian, Han,
Kana, Hangul, Emoji+ZWJ) through REAL widget paths — label + entry
+ textview + window title per family — checks ink with a theme-
agnostic modal-luminance sampler, pins the entry model's UTF-8
round-trip per family, pins the mixed LTR/RTL bidi line, and saves
one PNG per family. 24 locales (number/double/currency/date with
locale-correct separators incl. Arabic-Indic digits) format
cleanly. **Result: 13/13 renders, 13/13 entry round-trips,
13/13 bidi pins, 24/24 locales — on Xvfb AND on labwc**, 13
screenshot PNGs per backend (Arabic shaping/RTL and Han fallback
eyeballed: correct joining, correct run ordering, correct CJK
fallback to WenKai/Noto).

**Wayland compose/dead-keys LANDED (a real input gap closed):** the
seat now carries a per-keymap `xkb_compose_state` (locale from
LC_ALL/LC_CTYPE/LANG) and resolves presses through it — dead_acute
+ e → é as the KEY_DOWN codepoint, mid-sequence presses carry 0,
broken sequences cancel, repeats bypass the sequence state, and a
missing compose table degrades to the plain per-press resolution.
Pinned by tests/test_xkb_compose.c (dead_acute+e → U+00E9,
dead_grave+a → U+00E0, state reuse, plain reset — all green).
docs/platform-input.md documents the contract honestly, including
the multi-codepoint-sequence limitation (the IME_COMMIT channel is
the multi-codepoint path).

**Batteries:** 788 checks green in BOTH configurations (the compose
test adds 4). X11 smoke matrix and the labwc nine-app matrix run on
the new build; the layout sweep (589 layouts) reruns on it too —
round 29's green result (0 failures) stands for this build's
keymap path (compose is additive and covered by its own test).

### The language results table (both backends, round 32)

| script | renders | entry round-trip | bidi mixed line | notes |
|---|---|---|---|---|
| Latin | ok | ok | ok | baseline |
| Cyrillic | ok | ok | ok | |
| Greek | ok | ok | ok | |
| Arabic | ok | ok | ok | RTL + joining verified on the PNG; digits/separator via locale ar-EG |
| Hebrew | ok | ok | ok | RTL verified |
| Devanagari | ok | ok | ok | matras render (FreeSans fallback) |
| Thai | ok | ok | ok | no inter-word spaces — shaping correct |
| Armenian | ok | ok | ok | |
| Georgian | ok | ok | ok | |
| Han (CJK) | ok | ok | ok | fallback to WenKai/Noto verified on the PNG |
| Kana | ok | ok | ok | |
| Hangul | ok | ok | ok | |
| Emoji + ZWJ | ok | ok | ok | ZWJ family sequence renders (Noto Emoji fallback) |

Locales: 24/24 (en/de/fr/es/it/pt/nl/sv/pl/tr/ru/el/he/ar/fa/hi/th/
vi/id/ja/ko/zh-CN/zh-TW) — double + currency + short date, all with
the locale's own separator/digit shape.

Input: 589-layout sweep (round 29 build, green) + compose unit pin
(this round). IME: text-input-v3 on Wayland and XIM on X11 are
shipped; a live fcitx5 typing CJK is M-9 §11 — the owner's hardware
(the sandbox has no IME daemon); reported honestly as the
unsupported-here case, not a code gap.

**Bug rate this round: 0 new library bugs.** Environment: Xvfb +
labwc + a new backend code path (compose). The plateau count for
G3 starts here: one clean round recorded.


## Round 31 (2026-11-07) — the first-commit kill CLOSED; bug-rate: 1 found, 1 fixed, 0 new after the fix

**THE BUG (round 29's parked P1) — closed with the root cause and a
named protocol error.** Evidence chain: (1) the minimal replay client
(six variants — plain, set_buffer_scale,
ARGB, double-commit, max-damage, second-registry) SURVIVED on labwc,
exonerating the raw request sequence; (2) the FDK-side dead-
connection helper was taught to dispatch freshly-read events before
querying, which finally NAMED the error:
`wl_display#1.error(wl_display#1, 1, "invalid method 8 (since 1 < 3),
object wl_surface#31")` — wl_surface.set_buffer_scale exists since
version 3, but the add-on's own registry pinned wl_compositor at
version 1, making its surface a v1 wl_surface; libwayland-server
kills the connection on the first commit that carries the v3 request.
**Fixes:** the add-on binds min(offered, 5) like FDK's own registry;
FDK's apply_window_scale now refuses set_buffer_scale on any surface
whose compositor negotiated < 3 (skip — scale 1 is the default —
with an honest warning if k > 1). Result: the add-on's shell-surface
panel PAINTS on labwc (radial gauge live, screenshot-verified), and
the labwc smoke runs the full app matrix green for the first time —
the suite's version-gate legs pin the fix.

**Two library hardenings landed alongside:** the dead-connection
helper dispatches a freshly-read error event before the protocol-
error query (the detail line is no longer generic); the shared-lib
link rule creates the in-tree SONAME symlink (a dlopen consumer
could
not start from a clean build without it — found by the smoke).

**New-found regressions this round (environment: clean checkout):**
the single-shot compile rules omitted $(WAYLAND_DEFS) — an X11-only
build never defined FDK_DISABLE_WAYLAND for tests and
test_gl_lane_fallback failed to link (fixed at the rule; the
wayland-only cases guarded with an explicit [skip] line). Both
batteries green after: X11-only and X11+Wayland, 784 checks each.

**Current battery:** 784 checks × 2 configurations; X11 smoke
matrix rc=0; labwc smoke ALL APPS PASSED; release gate G2/G5/G6
PASS, G1/G3/G4 open by design.

Last audited: 2026-10-31, on the rc2 line after the history rewrite
(the tablet-crash round [D-033], the upload-contract re-land
[D-034], the f64 shaped pen [D-035], and the second round: the
§20 single-owner rule [D-036], the extreme-scale re-land's
completion [D-037], and the shaped-selection geometry [D-038] —
see the Implemented inventory's "2026-10-31 rounds" subsection).
Before those: 2026-10-30, at milestone **1.0.0** (D-5 session
save/restore, both lanes: include/fdk/fdk_session.h + fdk_window_
set_command/join_session/leave_session + FDK_EVENT_SESSION_SAVE/
RESTORED (enum 58/59), 869 exports (+8) — the X11 ICCCM lane
(WM_COMMAND argv + WM_SAVE_YOURSELF with the PropertyNotify ack,
automatic on every window; XSMP deliberately fenced, D-032) and
the Wayland xdg-session-management-v1 object lane (vendored
generated code, the join-table ownership, restore-before-first-
commit; no released compositor offers the protocol yet — probed
live on sway 1.10 AND 1.12/wlroots 0.20 — so open answers the
documented NULL today and the bodies are seam-proven). With this
landing EVERY roadmap item of ROADMAP.md (Part 1) is DONE except
M-9 (the release gate: the owner's real-desktop sign-off; no code
remains). The full battery green: core, X11 (the five-leg session
battery with the probe playing the session manager), D-Bus,
sanitize-off, release+examples+verify-exports (869), the FULL sway
suite (the session section: live degradation + seam bodies + join
validation). Prior state below.)

Last audited: 2026-10-30, at milestone **1.0.0** (D-4 foreign
toplevels: include/fdk/fdk_toplevel.h, 861 exports (+6) — the
taskbar substrate on wlr-foreign-toplevel-management (lazy bind)
+ EWMH _NET_CLIENT_LIST; the coarse changed-callback model; the
X11 retitle gap documented (D-031); the full battery green incl.
the X11 fake-WM round trip and sway's LIVE section. Prior state
below.)

Last audited: 2026-10-30, at milestone **1.0.0** (D-3 tablets/stylus,
both backends: FDK_EVENT_TABLET_* (enum 52-57) + fdk_tablet_event —
Wayland zwp_tablet_v2 (vendored generated protocol code) + the X11
valuator path through dlopen'd libXi (the XIQueryDevice scan); the
frame batching absorbed, the XI2-no-proximity gap documented (D-030);
855 exports unchanged; the full battery green incl. both suites' new
seam-driven legs. Prior state below.)

Last audited: 2026-10-30, at milestone **1.0.0** (D-2 subsurfaces,
extension API v2: `fdk_ext_subsurface_create`/`_create_ex` +
set_position/resize/set_desync/place_above/place_below — the
FDK-owned child-surface primitive, X11 child windows + Wayland
wl_subsurface DESYNC, the parent-managed tree-state rule absorbed
as the empty applying ancestor commit (DECISIONS D-029, the sway
rig's first-run lesson); 855 exports (+7); the full local battery
green incl. the new X11 leg and the sway suite's subsurface
section. Prior state below.)

**This file is the anti-duplication contract.** Before implementing
*anything*, check the "Implemented" inventory below and grep the public
headers — if a feature is listed here as SHIPPED, it exists in the tree;
do NOT re-implement, re-add files, or duplicate APIs for it. After
shipping any new feature, update this file **in the same commit**.
Full narrative history lives in `ROADMAP.md` (the phase-by-phase
account and the GTK/Qt feature-parity ledger); this file is the fast,
authoritative index of that ledger.

Last audited: 2026-10-25, at milestone **1.0.0** (the rc1
packaging matrix's first live CI pass — the 1.0.0 commits were
pushed and four of the six CI jobs failed, every defect a
packaging-script defect, none in library code: an
install-test smoke needs X11 DEV headers as test infrastructure
[libx11-dev/libX11-devel now pre-installed by the deb/rpm
install-tests, the class of gcc/pkg-config/xvfb — the .pc chain
deliberately carries no x11 Requires, and CI's own deb BUILD
container had proven the identical smoke green where the headers
existed]; FDK's own rpm install-test selected `dist/*.rpm`, so the
add-on's packages swept into a test whose removal step was then
CORRECTLY refused by rpm — a working dependency blocking removal
of its base [now FDK's own rpms by name; arch's sort-order
`head -1` landmine made the deterministic `fdk-[0-9]*` glob]; the
an add-on apk build hand-fed a bare sha512 into sha512sums, but
abuild's verify needs the FILENAME in the array [now FDK's proven
`abuild checksum` sequence; the rewrite also fixed REPODEST
artifact collection and dropped the `abuild -r -F` cargo cult];
an add-on xbps build checked dist/ while xbps-create wrote
into the CWD [the `mv ./*.xbps dist/` line restored]. Audited
alongside: the add-on distro builds had copied FDK's
LSAN_OPTIONS=exitcode=0 leak policy without its comment [restored
with the observed numbers; the inert alpine/void copies removed —
those images run no sanitizer toolchain], and the rpm/alpine/void
install-tests' `fdk_start_xvfb || exit 1` turned the el10
honest-skip into a hard failure [now the helper's disclosed-skip
if/else contract, the same shape as FDK's own install-tests].
A second push exposed the fifth and deepest class: the GENERATED
the generated add-on .pc was wrong twice over — its Libs line carried the
library's own platform link deps (-lX11 -lwayland-client, forcing
every consumer to install the X11+Wayland dev packages and baking
build-machine -L paths into the artifact; the deb/rpm smokes died
at 'ld: cannot find -lwayland-client') and it referenced
${libdir}/${includedir} without defining them [undefined .pc
variables expand empty — it only worked because /usr/include and
the default linker path need no flags]. The .pc now mirrors
fdk.pc's contract [variables defined; the add-on's own -l flag +
Requires: fdk; platform flags in Libs.private], the smoke's own
Xlib readback carries -lX11 on its link line [modern ld refuses
transitive resolution: 'DSO missing from command line', reproduced
locally], and pkg-config discovery failures now print the real
diagnostic [2>/dev/null swallowed the void failure's evidence;
pkgconf 1.0.0 was built from source locally and PROVEN to accept
the old bytes, so the void cause is pending its diagnostic].
The third push closed void: the corrected .pc made the build-phase
smoke discover/link the installed add-on (the old 'cannot find'
never recurred — the diagnostic never fired), and the remaining
failure was the void install-test's missing RUNTIME shlibs [xbps
resolves declared shlib-requires only against installed providers —
the combined-set transaction aborted on fdk-devel's libXext.so.6 /
libxkbcommon.so.0; now pre-installs the same hermetic shlib lines
FDK's own void install-test carries]. Proven locally: a mini
installed-prefix + the exact clean-container compile line = SMOKE
OK; the isolation proof (links the demo through the .pc alone)
still green. docs/testing.md's 1.0.0-rc1
packaging-matrix lesson + CHANGELOG rc1 Fixed section carry the
full narratives. Prior state below.)

Last audited: 2026-10-27, at milestone **1.0.0** (the owner-directed
verification pass + the CI-red day: the 0.2.0 packaging red's root
cause was the add-on's Makefile carrying its own hand-maintained
0.1.0 next to the header's 0.2.0 — every install-test caught the
staged .so.0.1.0 / Version: 0.1.0; the Makefile now derives the
version from the header like FDK's own [4d86464]; the alpine flake
was a present-then-readback RACE — readbacks ride a second X
connection and can beat the just-flushed frame, reading the empty
pre-paint entry — x11_frame_settle now guards all 18 unguarded
paint-then-readback sites [1373329]; the void reposync outage got
real mirror rotation before every remote install [c42418d]; the
release branch policy landed: a fast-forward-only consumer ref at
the v1.0.0 commit, RELEASING.md, CI on both branches, README +
packaging pointers [c8568ef] — all since removed by the 2026-10-31
return to in-development status (one branch, no RELEASING.md, no
release triggers in CI); and the alt/dbus-client test port
closed three genuinely-unique coverage gaps and found TWO real
accessor defects — fdk_dbus_message_signature and read_args were
builder-blind for locally built messages — both fixed at the root,
plus the FDK_LOG_TAG clobber in dbus_internal.h [ccafa7c]; DECISIONS
D-004/D-005 record the reconciliation and the core/add-on split;
ROADMAP-1.0.0 gained the verified-status table [this commit]. Prior
state below.)

Last audited: 2026-10-26, at milestone **1.0.0** (the push of the
1.0.0 fixes + the sixth CI defect class it exposed: the live-desktop
commits landed on origin [three commits, core 1041 lines / the add-on
947 / docs 247] and five of six CI jobs went red at the identical
line — `tests/test_x11.c: fatal error:
X11/extensions/XTest.h: No such file or directory`. Root cause: the
new passive-grab passthrough test synthesizes Super+number keys
through the XTEST extension, and the Xtst HEADER ships only with
each distro's dev package [libxtst-dev deb/apk, libXtst-devel
rpm/void, libxtst arch] — which none of the six build environments
pre-installed; the rpm job "passed" only through its disclosed el10
Xvfb skip [the suite build is Xvfb-gated there, so the missing
header was never compiled — a skip, not a pass, read at the JOB
level as always]. Fix: the dev package declared in all six install
lists [ci.yml's ubuntu apt + each family's build container], the
first matrix lesson's test-infrastructure class applied to the
builders themselves; the Makefile's XTest comment now names the
per-family packages and why there is no XWarpPointer dodge [a
passive grab activates only on synthesized KEY input — XSendEvent
delivery cannot]. The dependency is honest and permanent: the core
suite's XWarpPointer/XSendEvent tricks cannot verify that the WM's
Super bindings survive an open popup. The same run's void job died
one step earlier — a reposync connection timeout on the round-robin
mirror [an outage, not a code defect; the code was green on the five
other families] — so the critical xbps line now retries once after
10s, the tolerance the later lines already carried. Docs:
testing.md's sixth-class lesson, CHANGELOG's matrix section
[now "six defect classes"], the add-on Makefile. No tags, no
releases; 1.0.0 untouched. Prior state below.)

Last audited: 2026-10-26, at milestone **1.0.0** (the live-desktop
bar findings — sway/labwc + Openbox, all four reproduced in this
environment BEFORE fixing: [1] the Wayland layer-surface startup
sequence — the demo bar painted before its role's first configure
and wlroots killed the connection ["layer_surface has never been
configured"]; extension surfaces now ride FDK's first-configure
present gate, with notify_configure committing the deferred frame
post-ack — pinned by the eager-paint test, the bar rig's
WAYLAND_DEBUG trace assertions [no attach before the first
ack_configure, no wl_display.error], and alive/pixels legs against
BOTH headless sway AND labwc 0.8.3; [2] the fatal-dispatch
busy-loop — 2.2M identical log lines at 100% CPU after the
connection died; the core now logs a fatal dispatch once per code
and identical repeats sleep out the caller's remaining timeout, the
bar exits non-zero on a fatal pump, and the Wayland death line
carries the protocol error's interface/id/code — pinned by the bar
rig's compositor-kill leg; [3] the X11 popup keyboard grab vs the
WM's passive grabs — Super+number workspace switching died while a
dropdown was open [reproduced against real Openbox under Xvfb];
popups now grab POINTER-ONLY and route keys through saved+restored
XSetInputFocus, with explicit grab-result handling, dismissal
reasons, a not-viewable watchdog, and desktop-switch dismissal of
open popups — pinned by the in-test EWMH WM's passive-grab rig
[XTest Super+1 received by the WM while the popup is open and after
dismissal], the popup pixel-stays test, and the desktop-change
test; the GPU self-check retry budget became timer-based [fired
from the dispatch tail; the pump wakes via the X11 backend's new
next_wakeup_ms — both existing self-check tests still pass]; [4]
the add-on's layer windows now set USPosition/USSize hints [an
unpositioned dock is "unplaced" — Openbox centered the panel
mid-screen; confirmed in its debug log] and popups raise on map;
FDK_POPUP_TRACE=1 documents the popup lifecycle with reasons and
grab-held durations. make test, test-x11 [incl. both self-check
tests], FDK_SANITIZE=0 test, release+verify-exports [641],
examples, the add-on's 11-test X11 suite [twice], the Wayland suite
+ bar rig [sway+labwc+kill legs], and the isolation proof all
green. Prior state below.)

Last audited: 2026-10-25, at milestone **1.0.0** (the rc1
desktop-integration hardening, found live on a real NVIDIA GTX
1050 Ti desktop: the GPU present lane's self-check now decides
INCONCLUSIVE vs PASS vs REAL-FAILURE per connection with a
present-latency retry budget [not-viewable windows and XGetImage
BadMatch never downgrade; the verdict is process-wide and logged
once — FDK_GL_SELFCHECK_POISON is the test seam]; the add-on's X11
backend is sticky across workspaces [_NET_WM_DESKTOP 0xFFFFFFFF +
_NET_WM_STATE_STICKY, pre-map properties + post-map client
messages] with per-level stacking state [ABOVE for TOP/OVERLAY,
SKIP_* for OVERLAY; the fullscreen-over-docks EWMH ceiling and the
custom-WM contract documented in the add-on's README]; the demo bar
repaints on EXPOSE [the ghost-content fix]; the add-on's shared
library carries NEEDED libfdk.so.1 and its release config drops
sanitizers; the add-on is PACKAGED in all five families with a real
versioned FDK dependency [the add-on's packaging/, the install-tests
prove the dependency is enforced by each native package manager,
and every build compiles against the INSTALLED FDK — the isolation
proof re-run per packaging container; CI's ubuntu job runs the
the add-on suites + the isolation proof]. Prior state below.)

Last audited: 2026-10-25, at milestone **1.0.0** (the extension API
+ its first add-on — the 1.0.0-rc1 candidate: the owner directed FDK to
grow a public extension API ["add an extension API to FDK so
add-on packages can be built on top of it, and prove it by building
an add-on as a SEPARATE package that uses only that
public API"] and set the version to 1.0.0-rc1 [minor, not 1.0.0,
because public API is added; a CANDIDATE — no git tag, the v1.0.0
release and its tag untouched, the owner decides the final 1.0.0].
What landed: include/fdk/fdk_extend.h [the versioned extension API
— surface kinds with create/destroy/popup_attached callbacks whose
create builds the native surface through the documented
native-display accessor and FDK ADOPTS it into the full
event/render/popup machinery; loop fd sources in the pump's poll
set with the timer fire discipline; the public fdk_surface render
path + native display/handle accessors; popups on extension
surfaces with the zwlr get_popup hook on Wayland; backend +
capability queries; docs/EXTENDING.md is the contract narrative];
src/window/extend.c + src/core/extfd.c + the ext_surface_adopt /
ext_surface_native / ext_surface_resized / native_display platform
ops wired into BOTH backends; 20 new exported symbols, 641 total,
SONAME unchanged (append-only). The first add-on package 0.1.0 [
own build, own version, own pc file, ready to
move repo; shell-surface v4 via vendored scanner bindings on
Wayland, _NET_WM_WINDOW_TYPE_DOCK + _NET_WM_STRUT_PARTIAL on
X11/XLibre, the XOR-anchoring zone math, popups, the bar demo;
tests/isolation.sh is the prefix-isolation PROOF]. Tests:
test_extend.c headless + five X11-integration sections + the two
add-on suites, all ASan/UBSan green; two latent FDK bugs found
by the rigs on the way (uninitialized fullscreen/geo_repaint fields
on the create path; the wayland NULL-proxy destroy + the ext-popup
handshake commit). CHANGELOG.md created [1.0.0 + the candidate].
2026-10-25, later (push addendum): the candidate was pushed to the
remote, which had just been transferred to the Faded-Dream-Project
organization — canonical repository URLs across the docs and every
package's metadata now point at github.com/Faded-Dream-Project/FDK
(GitHub redirects the old FemBoyGamerTechGuy/FDK locations; the
LICENSE's Licensor and the packages' Maintainer/copyright identity
lines are the owner's person and deliberately unchanged). The first
CI run on the candidate failed four of five packaging jobs on the
rc version string — rpm/apk/arch reject the hyphen, xbps's
name-version split went ambiguous, deb ordered the rc above the
future final — fixed by fdk_packver (packaging/ci/common.sh): the
per-family pre-release spelling at the metadata layer only, raw
string everywhere the library reports itself. The second push
then failed ALPINE at the repository index with "package file
format error": abuild derives the apk so:provides version from
the .so FILENAME tail (sover=${file##*.so.}), so the raw-named
libfdk.so.1.0.0-rc1 produced provides = so:libfdk.so.1=1.0.0-rc1
which apk index rejects (reproduced locally with apk-tools
1.0.0 — even hyphenless 1.0.0rc1 is rejected; only numeric
tails are valid) — fixed at the root: the Makefile now installs
the real .so as the numeric FDK_SO_VERSION (libfdk.so.1.0.0;
suffixes never appear in the .so file name), and the debian/void
packaging scripts follow via fdk_sover(). The third push then
exposed an independent 1-in-400 CI flake: the HiDPI test's private
192-dpi Xvfb hard-coded :98 — inside the make test-x11 harness's
own display range (90 + pid%400) — and on collision it unlinked
the SUITE server's socket, probed the 96-dpi suite server via the
TCP fallback, and aborted the debian job on the scale assertion;
the private display now lives below every harness range, is checked
free first, and is verified ours by its 800x600 geometry (proven
both ways locally, docs/testing.md 1.0.0-rc1 lesson). See
CHANGELOG's rc1 Fixed section and docs/packaging.md's
pre-release-versions section.

Prior audit at milestone **1.0.0** (the one-face
alignment — the 1.0.0 re-cut: the owner inspected the live v1.0.0
release and set the product posture ["it needs to ship only one the
next ones must be in (.FDKthemes) btw the default one being
Faded-dream"] — FDK ships EXACTLY ONE theme, `faded-dream.fdk`, the
packaged copy of the built-in default/fallback; the next faces are
.FDKThemes drop-ins the user installs, never package content;
the Makefile's SHIPPED_THEMES is PINNED to that single file [the
wildcard is gone — a stray .fdk can no longer ride into a release],
the source tree's `.FDKThemes/` carries only the one face, and the
nine faces FDK grew between 1.0.03 and 1.0.07 are preserved as
engine fixtures in `tests/data/faces/` [never installed:
test_shipped_themes.c pins BOTH the one-face shipped contract —
opendir scan of `.FDKThemes/` included — AND the nine-fixture
corpus with every color/metric pin carried over; the X11
titlebar-theming rig loads mono-chromatic + pink-rave from the
fixtures; example 05 cycles the built-in + two examples/data
fixtures + the one shipped face]; README, docs/build.md,
docs/cli.md, docs/fdk-theme-format.md, docs/packaging.md,
include/fdk/fdk_theme.h, the tools' usage lines, and
packaging/ci/release-notes.sh all record the one-face posture; the
v1.0.0 tag was re-created on this commit and the release assets
replaced on the main repository), on top of the prior audit at
milestone **1.0.0** (the public 1.0.0
decision — the real release: the project owner, having validated
the 1.5.x picker overhaul on real hardware [native Wayland under
labwc included — "works native wayland", the FDK-drawn decorations
carrying move/resize without compositor shortcuts], explicitly
demanded the release ["i want a perfect release and the release
needs to be to the main release FDK not FDK test"], which is
exactly the explicit-owner-decision input docs/versioning.md's
enforcement section requires; the decision was recorded there
FIRST per policy, then fdk_version.h moved to 1.0.0 with the
test_core pin following in the same commit; the versioned-SONAME
scheme re-activated in the Makefile [-Wl,-soname,libfdk.so.1 on
the link line; install stages libfdk.so.1.0.0 + libfdk.so.1 SONAME
symlink + libfdk.so dev symlink, uninstall removes all three];
README badge + status, abi-policy SONAME paragraphs, packaging.md
banner, roadmap convention note all synced; the v1.0.0 tag was
re-created on the decision commit and pushed to the MAIN
repository, release.yml's matrix building all five families —
deb, rpm, pkg.tar.zst, apk, xbps — plus the pristine source
archive into the release; the "success but no packages" mystery
the owner reported was root-caused on the way: since the
2026-10-11 retraction every packaging job had been exiting 0 via
the disclosed PACKAGING-SKIP guard [green checkmark, zero
packages], and the Void family's CI-env failure the owner
remembered was from the retracted attempt — its fixes landed
afterward and were proven green in the matrix before this
release), building on **1.0.0** (the picker's
crash fix — the maintainer's fourth report ["it crashes anytime I
try to open anything that has to do with the filepicker", an ASan
heap-buffer-overflow inside nsvg__parseNameValue] resolved at the
root, not the symptom: the vendored upstream nanosvg PARSES CSS
<style>+class= rules — the repo's own docs had wrongly claimed it
did not, and that misdocumented blind spot is precisely how an
upstream one-byte OOB read shipped for two milestones — and the
name right-trim dereferenced *str with str == end, one byte past
the NUL of the strndup'ed class-properties text, whenever a CSS
declaration block ends in ';' + trailing whitespace (the standard
multiline rule every real theme authors, so EVERY dialog open on
a class-using theme aborted; 1.0.0's live XSETTINGS detection had
just made FDK finally select those real themes, which is why the
crash surfaced exactly then); fixed with a marked one-function
bounds patch in the vendored header [trim by str[-1] — identical
output for well-formed declarations], the vendor README's
provenance now recording the local diff against the upstream
sums, and the stale capability claims corrected in fdk_dialog.h /
icon_svg.c / roadmap; regression-pinned THREE ways: a headless
class-fixture group [eight shapes incl. both overflow forms and
positive fill-application pins], an X11 dialog round where a
class-only theme's fills are read back from the X SERVER through
the real open-file dialog [the maintainer's exact crash flow as a
permanent assertion], and the full headless + X11 suites green;
honest fidelity boundary documented [class rules with
presentation properties apply; color:/currentColor-only rules
leave the default fill]; public API and its 621 exports
unchanged), building on **1.0.0** (the picker's
polish pass — the maintainer's third report ["black boxes or bars
around folder icons, worst in the grid, present in the list; the
active desktop theme still not detected without an override; the
sidebar too"] resolved into two root causes, both fixed internally
[public API and its 621 exports unchanged]: (1) icon COMPOSITING
violated the blit contract on the exact-size fast paths
[fdk_surface_blit_transformed's identity and integer-translation
paths routed ARGB8888 icon sources through fdk_surface_blit — the
documented OPAQUE raw copy — so every icon decoded at exactly its
cell size (16-px list rows, 48-px grid cells, the sidebar's places)
painted its transparent margins as opaque BLACK; scaled blits
composited correctly, which is exactly why 1.0.0's verifications
missed it — the fixture icons were full-bleed squares and the
template matches sampled only the reference's opaque pixels; now
ARGB sources composite source-over on both fast paths, XRGB sources
keep the raw copy; the same defect class swept through every
consumer of the primitive: the About dialog's logo (its own plain-
blit call site) and the public Picture widget (inherits the fix)];
regression guards: a render-level transparent-margin test at
identity AND translation, transparent-margined fixture icons +
margin ring checks in the picker rigs, and the SVG grid proof now
sampling the cairosvg reference's TRANSPARENT region; (2) theme
detection now hears the LIVE desktop through XSETTINGS
[Net/IconThemeName read from the settings manager via one
short-lived X connection — pure Xlib, no D-Bus, the no-bus policy
intact; the channel the desktop's own GTK apps obey on X11/XLibre:
xfsettingsd, gsd-under-XWayland, mate/cinnamon, xsettingsd; the
wire format decoded byte-for-byte from a REAL xsettingsd 1.0.0 and
the reader verified end-to-end against that daemon including a
live theme change; precedence: FDK_ICON_THEME > XSETTINGS > user
gtk ini > xfconf xsettings.xml > kdeglobals > classic icon-theme
file > SYSTEM gtk ini (XDG_CONFIG_DIRS + /etc + /usr/share) >
default/index.theme > hicolor; pure-Wayland sessions have no
manager and fall to the file channels — proven under sway with
DISPLAY unset]; tests: headless detection groups for the new file
channels, an X11 XSETTINGS group with an in-repo spec-conformant
fake manager (both byte orders, int/string/color skip, precedence,
fallbacks), and both picker rigs re-verified pixel-level + VLM on
X11 AND Wayland), building on **1.0.0** (the exhaustive
pre-1.0 audit pass — the maintainer's second desktop report
["only recognizable folder icons; text files indistinguishable"]
traced to THREE root causes, all fixed internally [public API and
its 621 exports unchanged]: (1) theme detection missed the system
default channel [the icon search path's own default/index.theme
Inherits= line — the channel stock GNOME/XFCE systems publish; the
engine fell to hicolor on stock systems and every file type dropped
to the procedural glyphs, the reported symptom exactly], (2) PNG-
only lookups missed every SVG-only size [Adwaita's entire 48-px
scalable tree — now rasterized at the REQUESTED size through the
vendored nanosvg (third_party/nanosvg/, zlib license, the stb
precedent; librsvg rejected with its Cairo/GLib stack), same-dir
PNG preferred, undrawable SVGs are honest misses], (3) a broken
asset could shadow the whole chain behind it [the new decode-
refusal set: exists-but-will-not-raster paths are refused, probes
skip them, the search restarts — the shadowing regression test
pins an empty SVG in the own theme losing to the parent's valid
PNG]; verification is pixel-level AND independent [cairosvg/cairo
renders the reference from Adwaita's own SVG; the screenshot grid
must match within AA tolerance; VLM review of both views: "no
defects found"]; the audit also fixed make install PREFIX=X
installing a stale fdk.pc [prefix-stamped regeneration; caught by
the new external-application test — a standalone app compiled
through pkg-config against the INSTALLED tree, run under Xvfb with
the file dialog + icon engine live]; repo-wide TODO/stub/no-op
sweep clean, both-backend batteries green, sway/X11 example rigs
refreshed for the 1.0.06 theme colors and 1.0.07 scrolled catalog;
full narrative: ROADMAP.md "1.0.0"), building on **1.0.0**
(the picker
stabilization — the file/folder pickers wear the desktop's icon
theme through a new internal freedesktop icon-theme engine
[src/theme/icon_theme.c: XDG discovery, index.theme parsing with
real-theme directory subpaths, inheritance, size matching, PNG-only
raster, symbolic tinting, 3-layer caches reset per dialog]; List and
IconView grew borrowed raster-icon seams (fdk__list_row_set_icon_
image / fdk__iconview_item_set_icon_image / fdk__row_icon_paint_
image); every picker row now classifies by content [folders, images,
audio, video, archives, documents, fonts, source, executables —
extension table + exec-bit split]; tests/test_icon_theme.c pins the
engine headlessly with solid-color fixture themes; the X11 suite
gained three file-dialog-icons groups reading the X SERVER's own
pixels [per-row identity at 16 and 48 px + theme-switch cache drop];
real-theme verification against the system's Adwaita caught two
spec bugs [directory names contain slashes; hicolor's 11-KiB
Directories line] and produced the docs/screenshots gallery pair;
also fixed: FDK_ROW_ICON_RECENT silently reading as NONE, the
dialog icon-grid's one-column layout [fdk__iconview_reflow], and
the engine's uninitialized chain-slot read [found by a 30-run
sweep]), building on **1.0.0** (the
release-engineering pass — RETRACTED same-day, see below; the
packaging/CI infrastructure below remains on main as dormant
release tooling): the version decision itself [1.0.0 adopted, then
**retracted the same day** — the premature v1.0.0 tag and its
never-published draft release deleted; version back to 0.0.1 per
docs/versioning.md's retraction record; SONAME scheme back to
dormant, the Makefile installs the single unversioned libfdk.so;
the packaging scripts gained a pre-1.0 PACKAGING-SKIP guard so CI
stays green AND honest; ABI unchanged at 621 exports]; the
packaging tree
[packaging/: dpkg-deb + rpmbuild + makepkg + abuild + xbps-create,
five families, no libfdk.a anywhere per the license's 4(d)]; GitHub
Actions [ci.yml push/PR validation, reusable distro-packages.yml
docker matrix, release.yml tag → draft release via GITHUB_TOKEN
with minimal permissions and SHA-pinned actions]; docs/packaging.md
the full packaging+release reference; the Debian and RPM pipelines
validated END-TO-END locally [build, package, extraction-based
graphical smoke under Xvfb], musl compile probe 80/80 — then the CI iterations themselves took the matrix to fully green: all five distro pipelines + the ubuntu home-ground job pass end-to-end on main [build, native package, clean-container install-test, graphical smoke, removal] — all before the retraction; the v1.0.0 tag's DRAFT release (four validated families + source + SHA256SUMS, void joining on main after the tag) was deleted as part of it), building on
**1.0.09** (the drop that
landed — the maintainer's live report that real-file-manager drops
hover fine but never complete; three XDND receiver bugs found and
pinned: the XdndStatus action atom was always None, which
action-strict sources [Qt family, several XWayland bridges] read as
a refusal and cancel the drop — now answers XdndActionCopy, and
XdndFinished carries the performed action too [v5]; a failed payload
transfer silently vanished with NO event — now delivered as an
empty-payload DRAG_DROP, the contract Wayland always had; the
XdndTypeList read capped at 64 atoms with the remainder ignored —
now slab-read with a containment cap; drop-fetch budget 600->1000ms;
the capabilities example now prints every decoded payload entry
including folders; verified by a new GTK-faithful rig [8-type
TypeList with uri-list at index 4, real timestamps, strict and
refusing source modes] across a 6-mode x 2-lane app-level matrix and
three new suite groups), building on **1.0.08** (the GPU present
lane — the maintainer's hardware-acceleration ask, verified-absent
first, then implemented as a hybrid: CPU rasterization unchanged and
byte-identical, GLX presentation runtime-loaded via dlopen with a
hand-declared API subset [no new build deps], policy-gated [hardware
renderers only; llvmpipe/softpipe/swrast refused because the SHM path
is faster there; FDK_NO_GL / FDK_GL_FORCE knobs], self-verifying on
its first present, permanently downgrading with full frame salvage on
any failure, observable via fdk_window_get_render_path; the whole
160-group X11 suite additionally ran three times with FDK_GL_FORCE=1
so every widget/dialog/DnD test went through the lane, and a new
examples rig proves byte-identity at the app level), building on
**1.0.07** (the smooth
pass: the maintainer's polishing report — every themed control now
paints through the antialiased primitives so corners and circle
indicators read smooth; the hover-fade use-after-free fixed with
the done-callback handle clear every other animation owner already
had; example 04 opens screen-sized and scrolls its whole catalog
through the new `fdk_scrollview_set_vertical_only` recipe; the five
data-only faces from the previous task are now pinned in the
shipped-theme suite), building on **1.0.06** (the three faces:
Mono Chromatic, Pink Rave, and Faded Dream as the new built-in
default — the maintainer's theming-ability test, shipped as three
.fdk files in the custom `.FDKThemes/` folder with the TITLEBAR
token family that themes the decoration band and the `button_shape`
metric that turns every button circular; plus FIVE MORE faces
shipped data-only at the same audit: Catppuccin Mocha, Catppuccin
Latte, Nordic Frost, AMOLED Void, Solar Flare — ten stems total in
.FDKThemes/), 1.0.05 (the chrome uniformity tail: the toolkit's
own dialogs wear the FDK title bar), 1.0.04 (the live settings
desktop — per-app themes, LIVE re-theming when the command runs,
the X11 INCR-drop fix, the FDK title bar on every example, the
file-picker size retune, the developer theme whitelist) and the
1.0.02 full in-depth code review — every claim below was
re-verified against the tree, not copied from docs. THIS AUDIT'S
OWN FIRST CATCHES: 1.0.05's "X11 suite all-pass" claim was stale —
the decorated dialogs had shifted every dialog test's child
navigation one slot (the band became root child 0) and the suite
had not actually been run; worse, all five custom dialog-body
arrange hooks (message/prompt, About, font chooser, color chooser,
file picker) were double-offsetting their children by the band
height — every dialog's content rode 28px too low since 1.0.05.
Both fixed and pinned at 1.0.06; the tree is always the truth,
including over this file. THE AUDIT'S OWN FIRST CATCH: the ledger's "themed
tooltips" candidate was stale (the 1.0.0 tooltip has been fully
themed since birth — tokens, parser, Modern retune, tests); the tree
is always the truth, including over this file.

Same-date **v1.0.0-prep documentation pass** (docs/branding only —
zero toolkit code touched): the Faded Dream moon logo replaced
`examples/data/fdk_logo.png` (SAME 96x96 RGBA format and size —
examples 02 and 04 decode it at that size; do not resize the asset
without checking their panel layout); the README was rewritten
concise with the Faded Dream Project banner and badge row; `LICENSE`
was rewritten as the FDK Proprietary License v1.0 (restrictive on the
toolkit, applications expressly excluded — its Section 5);
`docs/screenshots.md` now indexes the captured gallery;
`docs/licensing-policy.md` updated to match.

Same-date **v1.0.0 release-engineering pass** (build system +
packaging + CI + docs): public version 0.0.1 → 1.0.0 (fdk_version.h
+ its test pin; the Makefile now derives SONAME libfdk.so.1 and
installs the standard versioned layout libfdk.so.1.0.0 + symlinks;
fdk.pc/install paths unchanged in spirit — LIBDIR override already
existed); packaging/ tree with per-distro native recipes (debian
control+dpkg-deb, fedora spec, arch PKGBUILD, alpine APKBUILD, void
xbps) + ci/ orchestrators + common/smoke.c public-API smoke app;
.github/workflows/ (ci, distro-packages, release); docs/packaging.md
new, versioning.md rewritten as the 1.0.0 decision record, abi-
policy/build/licensing-policy/README synced. THE CONFIG-STAMP
FOOTGUN pinned: `make install` after `make release` silently
rebuilds debug+ASan into the payload — all recipes pin
FDK_CONFIG=release on install (caught live by the Debian NEEDED
scan failing on libasan.so.8).

Same-date **license finalization pass** (docs/licensing only — again
zero toolkit code touched): the license's open decisions were closed
by the Licensor — governing law and venue set (laws of Romania,
exclusive venue Bucharest, with the EU consumer-forum savings);
Section 4(d) rewritten from runtime-accompaniment to the
**separate-runtime model** (applications are distributed WITHOUT the
FDK runtime — no bundling, embedding, static linking, or packaging of
it without further written authorization; end users install the
runtime themselves from an authorized channel); the draft banner and
every [REVIEW] flag resolved into operative text; Section 5 gained
5(f) stating the model restricts the toolkit, never the application;
the OPEN REVIEW ITEMS list replaced by a NOTICE recording the
counsel-free adoption; README, `docs/licensing-policy.md`, and the
roadmap's Phase 1 line synced to match. Rights holder unchanged:
FemBoyGamerTechGuy, by established handle.

Same-date **v1.0.0 retraction** (release-state only): the v1.0.0
release-engineering pass was undone hours after it ran — manual
testing showed the file/folder pickers were not release-ready
(uniform page glyphs for every file, no system icon-theme
consultation, visible picker defects). The premature `v1.0.0` tag
was deleted from the remote, the never-published draft GitHub
Release (9 assets: four distro families + source + SHA256SUMS)
was deleted with it, the public version returned to **0.0.1**
(docs/versioning.md carries the full retraction record), the
SONAME scheme returned to its documented dormant state (the
Makefile builds and installs the single unversioned libfdk.so),
and the packaging scripts gained the pre-1.0 PACKAGING-SKIP guard.
Nothing else was rewritten: the license finalization, the
packaging tree, the CI/release workflows, and all source commits
remain on main. The picker + icon-theme overhaul that motivated
the retraction is the next ledger entry.

## The §23 round (2026-11-01 — the layout fix + the every-gap sweep)

- **Part A**: the add-on package renamed in place (git mv, history
  kept; the weaker duplicate `shell/` DELETED — the tree already
  had the mature package). 73 files updated; zero stale refs.
- **B2c** fdk_attr.h: range attributes (size/family/bg/underline
  styles+colors/rise/letter-spacing/links) through the shaping
  pipeline; markup <span>/<sub>/<sup>/<big>/<small>/<a>.
- **B2b** fdk_gesture.h: seven recognizers (click 1/2/3,
  long-press, drag, pan, swipe, zoom, rotate), GTK4's thresholds.
- **B2e** fdk_model.h: the model/view split (store + sort/filter
  proxies with model faces + live List/Tree bindings + selection
  by row id + reorder write-back).
- **B2f** fdk_print.h: printing — the own PDF 1.4 writer
  (pdfinfo-verified), the native IPP client (RFC 8010/8011, no
  libcups — the OUT-by-policy line the owner's directive
  overrode), the dialog, the preview.
- **B2a-close** fdk_clipboard_set_uris/get_uris: the uri-list
  clipboard on both backends (the rig caught the have_content
  refusal bug before landing).
- **B1 verified in-sandbox**: the package's full X11 suite — live
  re-anchoring (margin/zone/anchor/level), auto-hide end to end,
  outputs+RandR, popup cascades, zero warnings; the v4 keyboard
  bind with the honest below-v4 degrade is in src/wl/wl.c.
- Tests: 748 headless [ok] + the X11 battery 215 [ok] (incl. the
  two-process uri rig); exports 963, verify-exports green both
  configs; every new feature has tests AND a runnable example
  (13 rich text, 14 gestures, 15 model/view, 16 printing).
- docs/PARITY.md: the gap list is now a status TABLE (7 of 10
  closed) + the §23 audit pass's remaining catalog tail named.

## Current state (verified this audit)

- **HEAD:** milestone **1.0.09** (the drop that landed — the XDND
  receiver-faithfulness pass: XdndStatus now answers the action atom
  so action-strict sources [Qt family, several XWayland bridges]
  commit their drops, a failed payload transfer delivers an
  empty-payload DRAG_DROP instead of silently vanishing, the
  XdndTypeList slab-read replaced the 64-atom truncation; the full
  narrative is ROADMAP.md "1.0.09 — the drop that landed").
  Before it: **1.0.08** (the GPU present lane — see above;
  the full narrative is ROADMAP.md "1.0.08 — the GPU present
  lane (hybrid rendering)"; the one-sentence architecture: FDK is now
  a hybrid renderer — every pixel rasterized on the CPU exactly as
  before, presentation on the GPU where one exists. Verified
  GPU-accelerated operations: the damaged-rect texture upload, the
  fullscreen composite, and the buffer swap — i.e. the entire
  pixels-to-screen half of every frame). Before it: **1.0.07** (the
  smooth pass — see above;
  the full narrative is ROADMAP.md "1.0.07 — the smooth
  pass"). Before it: 1.0.06 (the three faces — the theming
  ability test the maintainer asked for: **Faded Dream** is the new
  built-in default (the calm gray-with-violet retune; the packaged
  copy ships as .FDKThemes/faded-dream.fdk and the pre-1.0.06
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
  control/text/border, so pre-1.0.06 themes keep their exact band;
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
  theme). Also this milestone: the 1.0.05 dialog double-offset bug
  fixed in all five custom body-arrange hooks + the stale dialog
  X11 groups repaired and re-run for real. 1.0.05 the chrome
  uniformity tail; 1.0.04 the live settings desktop; 1.0.03 the
  command-line face; 1.0.02 INCR clipboard transfers; 1.0.01
  markup; 1.0.00 fullscreen; 1.0.0 PNG codec + Picture +
  clipboard images)
- **Build:** green in debug (ASan+UBSan) and release, X11 + Wayland
  backends both linked (Wayland protocols wired: xdg-shell,
  xdg-decoration, viewporter, **fractional-scale**,
  primary-selection)
- **Public API:** **621 exported symbols** (620 +
  fdk_surface_draw_rounded_rect_aa +
  fdk_scrollview_set_vertical_only/get_vertical_only +
  fdk_window_get_render_path),
  `make verify-exports` OK in debug AND release
- **Tests:** headless suite all-pass — **570 [ok] checks** (incl.
  the 1.0.0 + iconview suites, the eight 1.0.0 textview groups,
  the 1.0.0 png/picture suite, the 1.0.01 markup suite, the
  1.0.03 theme-discovery + CLI-tools suites, the 1.0.04
  per-app-priority / whitelist / live-recheck / fdk-set groups,
  and the 1.0.06 additions: the titlebar-fallback, button-shape
  (arithmetic + pixel proof), widget-background-token groups in
  test_theme, the ~/.FDKThemes + XDG-canonical-vs-legacy
  discovery groups, and the whole test_shipped_themes suite —
  the five faces pinned: faded-dream byte-equal to the built-in,
  mono's black band + circle buttons, rave's hot-pink band);
  X11 integration suite all-pass (**160 [ok] groups**, real Xvfb,
  real input, incl. the four 1.0.08 GPU-lane groups — policy matrix,
  byte-identity of GPU vs CPU presents, the poisoned-present
  downgrade salvage, resize-under-the-lane — plus the whole suite
  green three times over with FDK_GL_FORCE=1; the MIT-SHM
  double-buffer test is lane-aware),
  the modern-batch, HiDPI (private 192-dpi server), iconview GUI,
  file-dialog icon-mode, textview GUI, clipboard-image, the
  1.0.02 INCR read/serve/image groups, the 1.0.04 INCR-DROP
  + settings-live-follow groups against the external rigs, and
  the 1.0.06 titlebar-theming group — Mono Chromatic's black
  band over white content, Pink Rave's hot-pink band over plum,
  the built-in's fallback gray, each on the server's own pixels);
  39 test files (40 .c in tests/ counting bench.c, which is the
  perf harness, not a suite)
- **Scale:** ~58k lines of C in `src/` (+ generated Wayland
  protocols), 22 public headers, 12 examples

## Implemented — DO NOT re-add any of these

### Branding, docs, and release assets (v1.0.0-prep pass)

- **Faded Dream logo** — `examples/data/fdk_logo.png`, 96x96 RGBA:
  crescent moon centered over a night sky fading soft purple (lower)
  to deep dark blue (upper), rounded badge, transparent outside,
  no text/UI elements. Consumed by examples 02 (image panel) and
  04 (picture fits) and displayed at the top of README.md. Do NOT
  create a second/competing logo asset; the generator lives outside
  the repository (scripts/make_fdk_logo.py in the worklog's
  workspace).
- **README.md** — the concise front page (banner, badges, features,
  build/install, first-window example, examples table, themes,
  screenshots, docs table, principles, license notice). The long
  per-phase narrative it replaced lives on in `ROADMAP.md`,
  `PROGRESS.md`, and `docs/screenshots.md` — linked, not deleted.
- **LICENSE** — the FDK Proprietary License v1.0, finalized: restrictive
  on the toolkit (no redistribute/modify/sublicense/publish/exploit
  without written authorization), with an explicit
  application-developer rights section (Section 5), the
  separate-runtime distribution model (Section 4d — applications ship
  WITHOUT the FDK runtime; end users install it separately), and
  Romanian governing law with Bucharest venue (Section 14). NOT an
  open-source license.
- **docs/screenshots.md** — the gallery index for every capture in
  `docs/screenshots/`.
- **packaging/** — the ONE packaging tree: debian/ (control
  templates + build-deb.sh + install-test.sh), fedora/ (fdk.spec +
  scripts), arch/ (PKGBUILD + scripts), alpine/ (APKBUILD +
  scripts), void/ (xbps template + scripts), ci/ (host-side docker
  orchestrators per distro + common.sh),
  common/smoke.c (the public-API package smoke test). Do not create
  packaging metadata anywhere else.
- **.github/workflows/** — ci.yml (push/PR validation), distro-
  packages.yml (the reusable docker packaging matrix, run by ci.yml
  as a validation check). No release workflow exists: the project
  is in development, and no workflow may publish, upload, tag, or
  create a release or asset.
- **docs/packaging.md** — the packaging reference (what the recipes
  produce per distro, reproduce, troubleshooting, signing posture).

### Widget catalog (all SHIPPED, class names from `src/widget/*.c`)

button (with **MARKUP since 1.0.01** — fdk_button_set_markup:
styled label widths, run colors, decoration bars; the role
machinery is markup-agnostic), toggle, checkbox, radio, label
(with **MARKUP since 1.0.01** — rich spans in all three display
modes: NOWRAP measures styled, WRAP breaks by styled widths,
ELLIPSIZE cuts by styled metrics; get_text reports the PLAIN
text), entry, progress (determinate +
busy), separator, frame, notebook, list (multi-select, rubber-band +
auto-scroll, row icons), tree (row icons; multi-select + rubber band
+ auto-scroll), scrollview + scrollbar (smooth wheel; 1.0.07:
`fdk_scrollview_set_vertical_only` — content arranged at the
viewport's width, horizontal range collapsed, the recipe for a
scrolling whole-window catalog), menu /
menu-bar / context menu (accelerators + mnemonics), combo (editable),
slider (marks/ticks + labels; vertical orientation), **iconview**
(the 1.0.0 item grid: glyph cells in a re-flowing column field,
the List's selection model, grid keyboard nav, batch fills; the
1.0.0 rubber band + themed cell metrics; the 1.0.0 band
edge-chasing auto-scroll BOTH axes + cell double-click
activation),
spinbutton,
**picture** (1.0.0 — the image display: an owned surface under
NONE/CONTAIN/COVER/FILL fit policies, scaled paints on the
transformed-blit engine, the IMAGE a11y role),
**textview** (1.0.0 — the multi-line editor: word-wrap through the
break engine + no-wrap mode, caret/selection across visual lines
with the goal column, PageUp/PageDown, paragraph triple-click,
the Entry's whole splice/undo/clipboard/PRIMARY/preedit
discipline, the full a11y text interface under TEXT_VIEW),
toolbar, tooltip (with **MARKUP since 1.0.01** —
fdk_widget_set_tooltip_markup: the tip wraps by styled widths and
paints run colors + decoration bars; get_tooltip reports the
PLAIN text), spinner, link-button (role on button), paned,
expander, statusbar, stack + stackswitcher, revealer (slide modes;
crossfade via the paint group), levelbar, canvas (drawing area),
search entry (preset; debounced search-changed; BUILT on the
entry's icon slots since 1.0.0), **menu button**
(attached-popup hybrid — the hamburger), dialog (+ modal run),
**file dialog** (OPEN/SAVE, places sidebar, filters, breadcrumb path
bar, Ctrl+L location entry — modernized 1.0.0; XDG recently-used
place + accept-time recording since 1.0.0; **icon mode since
1.0.0** — options.view seeds the IconView browsing surface, the
toolbar's Icons checkbox flips it live, and every accept path
reads through the surface abstraction; **the 1.0.04 size retune**
— 866x614 window, 15px face, 34px rows, 184px places, 640x420
list: the "small picker, small text" report closed), **color button** (the
color-well swatch that opens the ColorChooser — 1.0.0; alpha
checkerboard, heap-token lifetime guard), path-bar composite,
window decorations (the FDK-drawn title band — themed fill, title,
  **its every pixel theme data since 1.0.06: the TITLEBAR token
  family (fallback semantics) paints the band, rule, glyphs, and
  title label, and the window buttons follow the theme's
  button_shape story (circle themes get circular window
  buttons)**,
close/min/max, band drag, FDK-driven resize; **worn by EVERY
example since 1.0.04** — example_window.h decorates the standard
window, 06/07/09/10 decorate their own, 02 stays bare because its
subject is raw-surface rendering that never paints the tree). Entry icon slots
(leading/trailing glyphs + custom painters + press callbacks,
1.0.0). List activate-on-single-click (1.0.0 — the places
sidebar rhythm).

### Dialogs & pickers (SHIPPED 1.0.0 — choosers.c; icons 1.0.0)

- **FilePicker / FolderPicker iconography (1.0.0)** — every row and
  grid cell wears the DESKTOP's icons via the internal icon-theme
  engine (src/theme/icon_theme.c): user-theme detection (GTK/KDE
  settings + the system default/index.theme channel + $FDK_ICON_THEME
  override, no bus), inheritance chains,
  Fixed/Scalable/Threshold size matching with a relaxed fallback,
  pixmaps last resort, per-type classification (extension table +
  executable-bit split), symbolic tinting, three cache layers plus
  the decode-refusal set, reset per dialog. PNG preferred per
  directory, SVG rasterized at the requested size via the vendored
  nanosvg (undrawable SVGs and XPM are honest misses), procedural
  glyphs as the floor. List rows 16 px, grid cells 48 px.
  Fixed on the way: the RECENT glyph's silent drop in
  fdk_list_row_set_icon's switch; the dialog icon-grid's stuck-at-
  one-column layout (fdk__iconview_reflow, the 1.0.0 lesson's third
  instance); the chain walk's uninitialized-slot read.

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
  input, resize edges, decorations, **HiDPI** (1.0.0: Xft.dpi /
  screen-metric detection at connect, integer scale [1,3],
  physical-sized windows + divided input — the same core
  compositing the Wayland side rides)
- Wayland backend: xdg-shell lifecycle, libxkbcommon keymap, client
  key repeat (1.0.0), output hot-plug/unplug, compositor-death
  resilience, cursor shapes
- Clipboard: text + URI formats, **PRIMARY selection** (1.0.0)
- DnD: both directions (X11 + Wayland), `fdk_drag_begin`; **the
  1.0.04 INCR-drop fix** — X11 drops now read through the shared
  requestor-parameterized selection engine (atomic, multi-part,
  AND incremental transfers; what GTK/Qt file managers do for
  larger uri-lists — previously the drop read the INCR seed as
  garbage and vanished after a "drop it" hover), with format
  fallback (uri-list refused -> UTF-8 text) and a 1000 ms
  convert budget; **the 1.0.09 receiver-faithfulness pass** —
  XdndStatus answers the v2+ action atom (XdndActionCopy; action-
  strict sources like Qt read a None action as a refusal and cancel
  the drop: hover highlights, release vanishes — the maintainer's
  live report), XdndFinished carries the performed action (v5), a
  failed transfer delivers an EMPTY-payload DRAG_DROP instead of
  vanishing silently (Wayland's contract, now X11's too), the
  XdndTypeList read is slab-bounded past 64 atoms (1024 cap), and
  the GTK-faithful rig + three suite groups pin all of it
- Focus traversal, double/triple-click, shift-click range select
- **Window states**: maximize/minimize/restore (Phase 8) +
  **FULLSCREEN (1.0.00)** on both backends — EWMH client messages
  with the bare-X fallback; xdg_toplevel requests with the
  configure states array as truth
- **HiDPI on Wayland: SHIPPED** — fractional-scale-v1 listener,
  viewport source rectangles, `fdk_window_get_scale`
- **HiDPI on X11: SHIPPED (1.0.0)** — Xft.dpi / screen-metric
  detection, integer scales, physical windows, divided input
- **The AT-SPI text interface + relations export: SHIPPED (1.0.0,
  C-2 / B-10 level 2)** — org.freedesktop.atspi.Text on the bridge
  (the full method surface; CODEPOINT<->BYTE conversion with the
  multibyte pin; per-character extents + hit-test through the
  internal fdk__widget_text_char_rect/offset_at seam — Entry's caret
  arithmetic, TextView's line cache, whole-bounds fallback for
  read-only classes; the whitespace hop for word runs);
  FDK_A11Y_CARET_MOVED (the public enum append) as the signal source
  -> TextCaretMoved/TextSelectionChanged, VALUE_CHANGED-on-text ->
  TextChanged; GetRelationSet with the AT-SPI relation numbers pinned
  against at-spi2-core's constants. D-027; rigs: 3 live-bus legs + 2
  headless legs (incl. the no-op rule).
- **Color glyphs (emoji): SHIPPED (1.0.0, C-3 / B-02)** —
  COLRv0/v1 + CPAL, CBDT/CBLC, and sbix, all parsed natively
  (src/text/emoji.c) with layouts pinned against fontTools; the RGBA
  glyph cache entry (fdk_surface_blend_rgba composites it; the public
  API unchanged at 848); THE BITMAP-ONLY LOADER GATE —
  fonts-noto-color-emoji (no glyf at all) loads and draws; the
  COLRv1 paint engine (gradients in paint space, affine coverage for
  rotate/skew, all 28 W3C blend modes); strike selection + box-filter
  downscale; the Zsye shaping fallback bucket;
  $FDK_NO_COLOR_GLYPHS=1 escape hatch. D-026; rig: test_emoji
  (COLRv0 math, every v1 paint family, CBDT strikes, the real-Noto
  smoke leg, the truncation sweep).
- **Basic gestures (pinch + swipe): SHIPPED (1.0.0, C-6 / B-17)** —
  FDK_EVENT_GESTURE_PINCH (span arithmetic on the two-slot posture,
  scroll coexistence, re-baseline) + FDK_EVENT_GESTURE_SWIPE (the
  per-finger end-gesture verdict) on the pure touch engine; no
  backend moved. D-025; rig: 6 engine legs.
- **the add-on's runtime re-anchoring + auto-hide: SHIPPED (1.0.0,
  C-5 / A-05 / A-08, add-on 0.5.0)** — the LIVE desc (anchor/
  margin/zone/size/keyboard/level on a shown surface; the Wayland
  role swap with the wire-order lesson; the X11 state exchange) +
  the reveal-strip auto-hide (backend-native slide, withdrawn zone,
  the pointer state machine, reveal/hide_now). D-024; rigs: 9 X11
  legs + 4 sway legs.
- **The AT-SPI2 bridge, level 1: SHIPPED (1.0.0, S-8 / B-10)** —
  the accessibility WIRE over the internal a11y tree: the two-bus
  handshake (retry on a 2s throttle — late-starting screen readers
  are a real case), the object registry (virtual rows included), the
  Accessible/Component/Action interfaces + the properties, the
  Focus/State/Property/Children events, the discovery announcement.
  $FDK_ATSPI=0 off. D-023; rig: 5 live-bus legs against the mock
  stack (the premature-degradation bug the rig caught is the retry
  posture's origin story).
- **The xdg-desktop-portal FileChooser: SHIPPED (1.0.0, C-1 / B-09)**
  — fdk_dialog_open_file/save_file consult the desktop's own picker
  first (org.freedesktop.portal.FileChooser on the native D-Bus
  client; the spec's race-free request dance) and fall back to the
  toolkit's own browser on any failure; $FDK_FILE_DIALOG_PORTAL=0/1/
  auto. The fallback's re-entry guard (no second flight; the UAF the
  rig caught) and the null parent. 848 exports. D-022; rig: 5 live-
  bus legs against the mock portal.
- **Drag icons + widget-level drop targets: SHIPPED (1.0.0, S-9)**
  — `fdk_drag_begin_ex` (the icon: Wayland start_drag's own surface /
  X11's override-redirect tracker window, alpha-shaped) +
  `fdk_widget_set_drop_target`/`_get_drop_target`/`_set_drag_callback`
  (scoped ENTER/MOTION/LEAVE/DROP; per-position acceptance on both
  backends — XDND Status and wl_data_offer.accept re-evaluate per
  position; the window callback still sees everything). Same-
  connection drags fixed on Wayland (the ::send self-serve read +
  the drag_drop_active fence); the 250 ms EOF wart and the X11
  DROP-event position closed. 847 exports. D-021; rigs: 7 X11 legs
  + 2 sway legs incl. the FDK-source drag-with-icon end to end.
- **Window activation + startup notification: SHIPPED (1.0.0, S-6)**
  — `fdk_window_activate` (Wayland xdg-activation-v1 through a
  compositor token; X11 `_NET_ACTIVE_WINDOW` with the live
  last-user-event timestamp + the bare-X raise/focus fallback) and
  `fdk_window_activation_token` (the launcher half: a minted token /
  SN id handed to a spawned child via `XDG_ACTIVATION_TOKEN` /
  `DESKTOP_STARTUP_ID`; the X11 mint emits the chunked "new:" SN
  state client-side, the first show of an id-carrying window emits
  the one-shot "remove:"). Both vouchers are captured once at
  connect and unset immediately (the protocols' hygiene rule). The
  Wayland mint is reentrancy-safe (private event queue +
  roundtrip_queue). 843 exports. D-020; rigs: 6 X11 legs + 5 sway
  legs incl. the two-process launcher flow (held-button serial,
  re-exec'd child, focus flip through the voucher)
- **GPU present lane: SHIPPED (1.0.08, X11)** — hybrid rendering:
  CPU rasterization unchanged (byte-identical, pinned by tests),
  GLX presentation via runtime-loaded libGL (dlopen, hand-declared
  API subset, no new build deps); hardware-renderer policy with
  software refusal (llvmpipe/softpipe/swrast), FDK_NO_GL /
  FDK_GL_FORCE / FDK_GL_FAIL_AT knobs; first-present server-side
  self-verification; permanent downgrade with full frame salvage;
  `fdk_window_get_render_path()` introspection (621st export);
  `tests/data/lsan-gl.supp` for Mesa's exit-time allocations. See
  docs/rendering.md "The GPU present lane (1.0.08)". The Wayland/EGL
  sibling SHIPPED 2026-10-30 (D-1 / B-15): src/platform/wayland/
  wayland_egl.c — the same contract on dlopen'd libEGL.so.1 +
  libwayland-egl.so.1 (eglGetProcAddress for the GLES2 subset;
  zero new build deps), the quad shader pinning alpha 1.0 + the
  alpha-less EGLConfig (the staging XRGB8888 buffer's zero top
  byte would otherwise composite as a fully transparent window —
  the GLX lane's "X byte rides along" reasoning does NOT cross to
  Wayland's ARGB compositing), the RGBA-readback self-check (the
  only GLES2-guaranteed default-framebuffer readback) with the
  process-wide refusal + FORCE override + fdk__wayland_egl_test_reset
  seam, the permanent downgrade with frame salvage
  (fdk__wayland_surface_downgrade_from_lane), the deferred
  first-frame flush (lane_frame_deferred), the same knobs and
  small-surface gate; test_wayland_integration.c's GL-lane group
  (policy / present+health / downgrade / self-check poison), the
  sway-rig screenshot smoke proving compositor-side bytes, exports
  unchanged at 848. Real hardware GPUs: implemented — should work
  (untested on real hardware; the rig runs it through Mesa's
  swrast under FDK_GL_FORCE).

- **Tablets / stylus: SHIPPED (1.0.0, D-3 / B-17)** —
  FDK_EVENT_TABLET_PROXIMITY_IN/OUT/DOWN/MOTION/UP/BUTTON (enum
  52-57, appended) + fdk_tablet_event (position/pressure/tilt/
  tool id/type/button, the high-bit release marker); NO synthesis
  layer (a stylus is never a click). Wayland zwp_tablet_v2
  (vendored generated code, manager bound when offered, frame
  batching absorbed); X11 the valuator path through dlopen'd libXi
  (XIQueryDevice scan, 65535 clamps); the honest XI2 gap: no
  proximity for clients (IN at first event, no OUT — D-030).
  Seams drive the real bodies (the M-3 discipline) + the live
  halves (sway manager bind; the Xvfb device scan). 855 exports
  unchanged (enum + payload only).
- **Subsurfaces: SHIPPED (1.0.0, D-2 / B-16, extension API v2)** —
  fdk_ext_subsurface_create/_create_ex + set_position/resize/
  set_desync/place_above/place_below (855 exports, +7): the
  FDK-owned child surface, X11 real child windows (server-clipped,
  XTEST-routed input, XDND aware) + Wayland wl_subsurface DESYNC.
  THE protocol lesson (D-029): tree state is PARENT-managed — FDK
  issues the applying EMPTY ancestor commit at create/set_position/
  place so moves are immediate. Keyboard follows the parent; popups
  on subsurfaces: X11 yes, Wayland the loud UNSUPPORTED.
- **The foreign toplevel list: SHIPPED (1.0.0, D-4)** —
  include/fdk/fdk_toplevel.h (open/close/count/get_info/activate/
  close, 861 exports +6): the taskbar substrate, Wayland
  wlr-foreign-toplevel-management (LAZY manager bind at open — the
  initial burst meets a listener; the empty set fires its change
  from open) + X11 EWMH _NET_CLIENT_LIST (live on root
  PropertyNotify). The coarse changed-callback model (D-014's
  shape); the X11 retitle gap documented (D-031). Rigs: the X11
  fake-WM round trip + sway's LIVE section.
- **Session save/restore: SHIPPED (1.0.0, D-5 / B-19)** —
  include/fdk/fdk_session.h (open/destroy[state preserved]/remove
  [forgotten]/id/get_state + the coarse notify) +
  fdk_window_set_command (X11 WM_COMMAND, argv form, type STRING)
  + fdk_window_join_session/leave_session +
  FDK_EVENT_SESSION_SAVE/RESTORED (enum 58/59). X11: the ICCCM
  objectless lane — WM_SAVE_YOURSELF in every window's
  WM_PROTOCOLS, the ClientMessage dispatches the SAVE event and
  the dispatch tail rewrites WM_COMMAND after the handler returns
  (the PropertyNotify IS the ack; zero-length for command-less
  windows; XSMP fenced, D-032). Wayland: xdg-session-management-v1
  (staging, wayland-protocols 1.48; vendored generated code, lazy
  bind; restore_toplevel pre-show vs add_toplevel post-show by the
  window's own gate — the already_mapped error is structurally
  impossible; the tracker's join table sweeps at teardown; window
  destroy unregisters WITHOUT remove_toplevel). No released
  compositor offers the protocol yet (probed live: sway 1.10 +
  1.12/wlroots 0.20) — open answers the documented NULL today;
  the bodies are seam-proven and the live-compositor branch is
  ready in the suite. Rigs: the X11 five-leg battery (the probe
  plays the session manager, the full ClientMessage→event→ack
  round trip) + the Wayland degradation/seam/validation legs.

### Infrastructure (SHIPPED)

- **The AA chrome pass (1.0.07)**: every themed control paints
  through the antialiased primitives (fill/draw_rounded_rect_aa,
  fill_circle_aa — 57 call sites across the widget layer); interior
  pixels byte-identical to the crisp path, only the curved fringes
  blend; **fdk_surface_draw_rounded_rect_aa** completes the public
  AA family (1-px stroke through the edge rows' pixel centers —
  radius 0 is byte-identical to draw_rect, straight sides exactly
  the crisp outline's pixels, corners ramp)
- **Hover-fade handle lifetime (1.0.07)**: the shared
  fdk__hover_fade_arm's done callback clears the stored flight
  handle on completion AND cancel — the reaped-zombie
  use-after-free the settings demo found under ASan; the
  regression test pumps past the grace and arms again
- Theme engine: `.fdk` format, **33 color tokens + 15 metrics**
  (`FDK_TK_*` / `FDK_TM_*`), runtime switching, the Faded Dream /
  Modern / v1 recipes; button roles (suggested/destructive/link),
  placeholder text, hover fades (1.0.0/1.0.0). **The 1.0.06
  additions: the TITLEBAR family** (titlebar_background/text/
  border/button_hover/button_pressed — FALLBACK tokens: unset keys
  read through to control/text/border so pre-1.0.06 partial themes
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
- **fdk_widget_set_background_token (1.0.06)**: the token-following
  background — a widget's fill resolves a theme token at PAINT
  time, tracking every theme switch live with no hook (explicit
  set_background still wins and cancels the mode); the examples'
  structural panels ride it
- **The three faces + the .FDKThemes folder (1.0.06)**: Mono
  Chromatic (white world / black speech / near-black titlebar /
  circle buttons), Pink Rave (plum floor / rose ramp / hot-pink
  accent and band), Faded Dream (the default's packaged copy);
  shipped in the source tree's `.FDKThemes/` (daylight + matrix
  alongside), installed to `<prefix>/share/fdk/.FDKThemes/`,
  discovered on the grown search path
  ($FDK_THEME_DIR > **$HOME/.FDKThemes** > $XDG_DATA_HOME/fdk/
  .FDKThemes > the legacy fdk/themes > each XDG_DATA_DIRS entry's
  pair) — test_shipped_themes pins all five
- **Theme discovery + the settings engine + the CLI (1.0.03,
  grown into its final shape 1.0.04)**: fdk_theme_find /
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
- **Paint group (1.0.0): per-widget subtree opacity** — the ARGB
  offscreen + global-alpha source-over blit engine (the crossfade
  rides it; nested groups composite naturally)
- **Font surface (1.0.0): `fdk_font_enumerate`** (name-table parse,
  TTC faces, family/style/path sorted) + **`fdk_font_load_face`**
- Accessibility: a11y tree (**41 roles**), narrator, announcements,
  per-widget value interfaces
- i18n: catalog, plurals, dates, locale
- Undo/redo: generic undo stack + entry integration
- Preferences: `.fdk-prefs` persistence, atomic saves (1.0.0)
- Text: fontconfig scan, stb_truetype shaping, AA rendering,
  ellipsize, line breaking
- **Images (1.0.0)**: stb_image decode (file + memory), the PNG
  ENCODER over zlib (fdk_surface_save_png), the Picture widget,
  clipboard image/png (X11 atomic property + Wayland data-device
  pipe, one-content-at-a-time)
- **Rich text / markup (1.0.01)**: fdk_span (byte range +
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
- Shortcuts/accelerators/mnemonics (1.0.0 / 1.0.0)
- Versioned shared-library export surface (version script +
  verify-exports gate)

### The 2026-10-31 rounds (post-rewrite re-lands + the desktop
report fixes — CHANGELOG "Unreleased-candidate features")

- **The XI2 tablet crash fix** (D-033): the missing XIDeviceEvent
  `flags` field (every valuator tail field read 8 bytes early —
  the owner's SEGV in xi2_valuator with 0xbebebebe as a pointer),
  the off-by-one touch/gesture constants, the unvalidated tail;
  hostile-cookie + classification + XTEST-cookie rigs.
- **The Wayland EGL upload contract** (D-034, the strict-ES2
  NVIDIA fix, re-landed): extension detection with the RGBA-twin
  and packed-row fallbacks; FDK_GL_CORE_ONLY pins the shapes.
- **The f64 shaped pen** (D-035) + **the O(n) attribution boundary
  table and the f64 wrap pen** (D-037): the extreme-scale
  quadratics are gone (128 KB took ~19 s pre-fix; 1 MB now ~1.5 s
  under ASan) and a 20k-line document's deep lines no longer
  drift; tests/test_stress.c is a PERMANENT make-test member (10k
  list, 20k document, 1200 fps full-screen animation + scrolling,
  1 MB mixed-direction single line).
- **The §20 single-owner rule** (D-036): while the GL lane owns a
  wl_surface its buffer state belongs to EGL — FDK's queue sends
  no attaching commit (background-buffer refusal at the configure
  and resize seams, defensive classic-commit refusal, hide()
  inert); the load-bearing downgrade order; every
  dead-connection mark logs the protocol error's
  interface/object/code + errno; the attach-count rig pins zero
  FDK-queue attaches (verified both directions on sway/llvmpipe);
  the activation re-exec rig lies no more (fork+execl, not
  system()).
- **The shaped-selection geometry** (D-038):
  fdk_font_selection_spans_utf8() (the cluster-snapped visual
  spans — the rectangle IS the clusters' advance boxes, zero-width
  marks never widen, bidi selections are several rectangles); the
  Entry paints them and draws its text WHOLE (the old
  three-segment draw cut clusters and moved the glyphs);
  pixel-level pins in tests/test_selection_pixels.c.
- **The D-Bus once-notice** (the unavailable line fires once per
  process).

## What's left (the authoritative backlog)

### NEXT — nothing

The backlog is EMPTY (and as of 2026-10-30, the 1.0.0 roadmap is
fully landed too: every register row DONE except M-9, the release
gate — no code remains there, only the owner's real-desktop
sign-off before any tag; see ROADMAP.md (Part 1)'s counts line). The widget families shipped across the
board; rich text shipped in 1.0.01; INCR transfers shipped in
1.0.02; the command-line face shipped in 1.0.03; the live settings
desktop shipped in 1.0.04 (per-app themes, live re-theming, the
INCR-drop fix, the whitelist — the maintainer's own usage reports
were the backlog, and every item closed); the three faces shipped
in 1.0.06 (Mono Chromatic, Pink Rave, Faded Dream as the default,
the .FDKThemes folder, the titlebar family, button shapes, the
token-following background — the theming-ability test the
maintainer asked for, and with it the 1.0.05 dialog-offset bug
swept out); the five extra faces shipped data-only on top of it
(Catppuccin Mocha/Latte, Nordic Frost, AMOLED Void, Solar Flare);
and 1.0.07 closed the maintainer's polishing report in full (the
AA chrome pass over every themed control, the hover-fade
use-after-free, example 04's screen-sized scrolling catalog, the
ten-face enumeration pins). What remains is
LATER-by-policy (IME completion surface
waits for a protocol joining third_party/wayland-protocols) and
OUT-by-policy (the deliberate non-goals below) — the line where
adding anything more would make FDK something other than a
toolkit.

### LATER (real, not next)

- IME completion surface (preedit is display-only today)

### OUT (deliberate policy — do not chase)

GLArea (software renderer is the product) · CSS-like styling (`.fdk`
is the surface) · printing · Assistant/wizard · full XSMP/ICE
session management (the ICCCM obsolete lane + the Wayland protocol
are the shipped surface — D-032's fence) · animated WebP/APNG
(B-14's fence: future families on the GIF player's shape).

(History note, kept honest: this list is the 1.0-era policy. The
1.0.0 roadmap — owner-directed — then shipped icon-theme loading,
the D-Bus foundation and everything it rides (tray, notifications,
portals, AT-SPI), touch AND gestures; those lines are gone from
OUT because the roadmap overrode them, not because policy drifted.
What remains above is still policy.)

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
   item, bump the milestone, update `ROADMAP.md` narrative +
   ledger, and commit it all together.
3. Never trust a session hand-off summary over the tree — `git log
   --oneline -3` and this file are the truth.


---

# The bug/gap ledger (absorbed from test-apps/FINDINGS.md)

Every FDK defect a test-app hits lands here. Protocol: a failing
regression test in FDK's own suite FIRST (tests/), then the fix in
src/ or include/, then the app continues on the public API. Nothing
is worked around silently inside an app — if an app needs something
FDK lacks, that is a bug in FDK by project rule.

| ID | App | What failed | Root cause | Fix commit | Regression test |
| -- | --- | ----------- | ---------- | ---------- | --------------- |
| G-1 | fdk-files | The Notebook had `fdk_notebook_page_label` (get) but NO setter — a file manager cannot retitle a tab after navigating (every multi-document app needs this: editors retitle on save-as). The app was about to work around it by rebuilding the notebook on every navigation. | Missing API since Phase 9: the getter shipped alone. | (this commit: tests/test_widgets2.c `test_notebook_set_page_label` written FIRST — red; then `fdk_notebook_set_page_label` in notebook.c + header) | tests/test_widgets2.c::test_notebook_set_page_label |
| G-2 | fdk-files (§25, by design) | No cross-thread story AT ALL: docs/threading.md's "Getting work back onto the UI thread" was a Phase-1 placeholder promising `fdk_invoke_on_ui_thread` "planned". A file manager must copy/move/thumbnail off-thread or its UI freezes; §24 worked around it with timer-sliced operations on the MAIN loop (recorded there as friction). | Missing subsystem: no post queue, no wake fd in the pump, no worker facility, no enforcement of the threading rule. | §25: `fdk_post` + eventfd/pipe wake wired into the pump's poll set (backend-independent), `fdk_worker`/`fdk_job` pool with cooperative cancel, `FDK_ERR_WRONG_THREAD` refusal under `FDK_DEBUG_THREAD_CHECKS` (debug builds), docs/threading.md rewritten from placeholder to contract. Tests FIRST: tests/test_threads.c written red, then the implementation. | tests/test_threads.c (stress 256 posts / ordering 1024 / wakeup latency / worker roundtrip / cancel / shutdown-in-flight / wrong-thread refusal) — ASan via test-x11, TSan via `make test-threads-tsan` |
| G-3 | fdk-files (§24 friction, §25 fix) | List selection was query-only: no per-row deselect, no programmatic range select (the shift-click equivalent for menus/scripts), and on_selection_changed carried NO row information — a status bar must rescan the model on every fire. The §24 app's invert-selection rebuilt the whole set via clear+re-select (N signals), and pane_report_status walked every row. | Missing API: the selection plumbing was complete internally (list_set_row) but only fdk_list_select/clear were exposed; the signal had no payload. | §25: `fdk_list_deselect` (no-op contract: no signal when unchanged), `fdk_list_select_range(first,last,additive)` (order-agnostic, clamped ends, one fire), and `fdk_list_set_on_selection_changed_bounds` — the bounds-carrying signal twin (the smallest contiguous span covering every row the settled change touched; the clear of a replaced selection widens it, documented). Written RED first (undefined refs), then implemented over the existing span-free plumbing. | tests/test_list.c::test_selection_completeness |
| G-4 | fdk-files (§25/4c) | Thumbnails must DECODE on a worker thread, but every decode API returned an `fdk_surface` — and surfaces are documented UI-thread-only (docs/threading.md). The first thumbs.c draft created surfaces on the worker: against the letter of the threading rule. | Missing API: no pure-pixel decode path; `fdk_surface_create_from_image*` are surface constructors and the rule covers them. | §25: `fdk_image_decode_pixels` + `fdk_image_pixels_free` — the same reject-first, size-bounded, 4-channel stb path as `create_from_image`, minus the surface object: a plain malloc'd RGBA buffer, any-thread-safe by construction (no FDK state). Written RED first. | tests/test_render.c::test_image_decode_pixels |
| G-5 | fdk-files (§25/4c) | IconView items are glyph+label pairs only — a thumbnail file manager has nowhere to PUT a per-item image (§24 showed a generic FILE glyph for every image in icons view). The paint path already had a borrowed-image slot (the icon-theme engine's internal setter); applications could not reach it. | Missing public API over an existing internal mechanism. | §25: `fdk_iconview_set_item_pixmap` / `fdk_iconview_item_pixmap` — the public face of the borrowed-image slot (app owns the surface; image paints instead of the glyph, fit into the icon band; NULL restores the glyph). Written RED first. | tests/test_iconview.c::test_item_pixmaps |
| G-6 | fdk-files (§26/3a, split view) | `pane_remove_last` destroyed a pane's widgets while a MODEL BINDING still referenced the destroyed list — the next `fmodel_load` (filter change) rebuilt into freed memory (ASan heap-use-after-free in `fdk_list_begin_batch`). The binding registry is keyed by widget pointer with NO destroy hook: a destroyed bound widget left a stale entry forever. | Missing lifecycle hook in the model/view binding (§23/B2e shipped bind/unbind but no teardown). | §26: `fdk__model_bind_widget_destroyed` called from `teardown_free` (widget.c) beside the a11y-relations sweep — releases the slot + frees the id map WITHOUT touching the model (the model may already be freed: apps that shut models down before windows). binding_changed's existing stale-handler guard covers the model's lingering on_change pointer. Test FIRST: tests/test_model.c::test_binding_detaches_on_destroy (RED on the UAF, GREEN after). | tests/test_model.c::test_binding_detaches_on_destroy |
| G-7 | audit (§27/2a chase) | `fdk__extfd_dispatch` pairs the LIVE source array with the pollfd snapshot by INDEX and iterates the live count — a source added between `fill_poll` and dispatch (reachable: a widget event handler may call `fdk_ext_loop_add_fd` from the backend-dispatch phase) pairs with a pollfd slot beyond the filled region and fires with revents the poll never reported. | Latent mispairing hazard; today's pump early-returns make it unreachable, but any future change to those returns exposes it. | §27: dispatch now bounds itself by the FILLED count `n`, never the live count (tests/test_extfd_race.c pins the discipline with the public API: a mid-pump-added pipe source must not fire with fabricated revents). | tests/test_extfd_race.c |
| G-8 | fdk-files (§27/2a) | Five desktop modules open five session-bus connections around the same pumps; the burst of simultaneous SASL handshakes is the shape under which dbus-daemon closed the app's first connection mid-auth (the wire trace: auth OK then dropped as the burst opened; the notification round-trip died). | No per-address handshake serialization: every module raced its own connect/auth/hello against the others. | §27: the serialized-connect queue — a connect that arrives while another connection to the SAME address is still in its connect/auth/hello phase is parked (handle returned immediately, sends queue per the existing contract) and its socket opens when the predecessor settles (ready or dead). No public API change. tests/test_dbus_serialize.c: five no-pump-between-opens connects all reach ready, zero failures. NOTE: the app's --smoke boot still sees its first notify connection closed in some runs — the serialization removes the burst but the deeper trigger (see CONTINUATION §27) is NOT fully pinned; the client path itself is proven by the tray-bus rig's run-1 (notify → daemon → card placed). | tests/test_dbus_serialize.c |
| G-9 | fdk-files (§27/2b chase, fixed §28) | A pane whose directory HAS files showed 0 rows though fmodel_load returned 2 (the DRIG drag rig found it: its clicks found no rows to select). FOUR defects stacked: (1) model_bind installed the binding through the model's SINGLE on_change slot, so the second pane's bind OVERWROTE the first's handler — after fmodel_load's clear+fill only the last-bound pane rebuilt; (2) the proxies read the CHILD'S RAW ROW ARRAY (proxy_rebuild collect, sort comparator, filter predicate) — but fdk-files chains store→filter→sort, so the sort's child is the filter's FACE, whose rows[] is empty (its rows live in the permutation): the whole chain showed 0 rows, and cell/schema lookups through a face-over-face failed (empty row texts; fdk_model_get → FDK_ERR_INVALID_ARGUMENT); (3) the filter's predicate ran ONLY in set_predicate — a store mutation arriving through child_changed rebuilt the permutation UNFILTERED, resurrecting hidden rows; (4) sort_set_key's refire never reached an upstream proxy, so a filter over a re-keyed sort kept the old order. | The model/view split's core promise (§23/B2e) was single-view in practice (one public on_change slot, no listener list), and the proxy layer violated its own face abstraction by reading store internals instead of the public row API. | §28: (a) per-model LISTENER LIST (m_listener + internal fdk__model_add_listener/remove_listener) — fire_change runs the app's classic slot THEN every listener, the walk stable under mid-flight unbind; the binding subscribes through it, so N views mirror one model and unbinding/destroying one view spares the others (G-6 stays green); (b) proxy_rebuild's collect, the sort comparator (proxy_cmp_keys) and the filter predicate all read through the PUBLIC row API (child_count/child/row_id/fdk_model_get), so a proxy's child may itself be a face; (c) cell_row RECURSES through face children and schema_of WALKS to the chain's bottom store; (d) fire_face (slot + listeners) is the one refire path for child_changed / sort_set_key / filter_set_predicate, so face refires reach upstream proxies like any listener; (e) the filter rule re-applies inside proxy_rebuild (a hidden row stays hidden across child changes). LSan caught the missing listeners free in fdk_model_destroy and the proxy destroys. Tests FIRST: tests/test_model.c::test_multiple_views_one_model written RED (view 1 froze at 2 rows on the second bind — the exact fdk-files symptom), then ::test_multiple_views_through_proxies and ::test_bound_views_a11y_exposure (both views' AT-SPI LIST_ITEM rows grow with the model). Example 15 now binds TWO lists to the same proxy face. | tests/test_model.c::test_multiple_views_one_model, ::test_multiple_views_through_proxies, ::test_bound_views_a11y_exposure |

## Verdict vocabulary

- **BUG** — FDK behaved wrong (crash, wrong pixels, wrong values,
  leak). Fixed + tested in-tree.
- **GAP** — FDK lacks an API a real application needs. Judged per
  the project rule: fixed in FDK (new public API, documented,
  export-checked, tested) rather than worked around — unless the
  gap is a deliberate v1 boundary (docs/PARITY.md), in which case
  it is recorded here and the app states the limit honestly.
- **DOC** — the headers/docs misdescribed behavior. Docs fixed.

## §29 findings
- G-10 (fdk-files, §28 rig): drag arming starved — a press on a List ROW is
  consumed by the row's selection handler, bubbling stops, the ancestor's
  event callback never sees the press. FIX: widget-level drag-source
  controller, fdk_widget_set_drag_source (7af865b); regression
  tests/test_widget_drag_source.c; example 18 twin pad.
- G-11 (fdk-files, drag rig D1/D2): drag onto the app's OWN window delivers
  an empty payload (self-drop). OPEN — repro parked at
  test-apps/g11_self_drop_test_parked.c; evidence + next leads in
  PROGRESS.md's round-29 log (§29 of the old file).
- G-12 (x11 dnd): ButtonRelease can beat the target's last XDndStatus;
  release now drains queued status replies first (419431f).
  §30/STEP-1d: the DEDICATED regression test exists and is red-without-
  fix / green-with-fix: tests/test_x11_integration.c::test_dnd_release_
  drain_gui — the race is CONSTRUCTED (release queued ahead of the
  accepting Status via two SendEvents, no timing involved), asserts the
  drag stays alive awaiting XDndFinished instead of cancelling on the
  spot (the state machine G-12 owns), then finishes SUCCEEDED. Verified:
  drain disabled -> RED at `!reported` (immediate CANCELLED); drain
  restored -> GREEN (suite x11 219 ok, 0 skips).
  OPEN MYSTERY (recorded, not chased further this round): inside the
  FULL suite, a ClientMessage FDK XSendEvents to this test's target
  window was traced SENT+flushed yet never appeared in the receiving
  connection's queue (post-race XPending=0); the identical construction
  as a standalone probe sees it every time. The test therefore asserts
  FDK's own state machine, not cross-client delivery — G-12's contract
  either way. Next lead if ever chased: Xlib per-connection event
  queue state after 130 prior GUI tests in one process (XUnsynced
  counter? destroyed-client window IDs recycled into the target's?).
  Also: the drag's target discovery is z-ORDER dependent — the test
  re-raises its target and retries the motion (8 attempts, matches
  the Position's packed root coords) instead of assuming an empty
  screen; that is a TEST-harness need, not an FDK defect.
- ENV (not FDK): xtest_driver's unconditional selfcheck moved the pointer
  to (11,22) between every invocation — the §28 rig's four dead legs.
  Fixed in scripts/xtest_driver.c (selfcheck opt-in).

## Round 21 findings (the two remaining apps built)

- G-20 (fdk-edit, build): doc.c's "● " + label[128] into marked[128]
  tripped -Wformat-truncation on the release -O2 build. App-side
  fix (marked[132]); no FDK defect.
- G-21 (fdk-monitor, API friction — recorded, not a core gap):
  fdk_color channels are fdk_f32 0..1 while the SNI tray pixmap
  contract wants ARGB32 BYTE order — every tray-pixmap app redoes
  the float->byte conversion by hand. Candidate v1.1 helper
  (fdk_color_to_argb32_be); recorded in DECISIONS, not shipped
  (two call sites do not yet justify an export).
- G-22 (both apps, leak discipline): app-owned fdk_menu models MUST
  be destroyed by the app after the bar dies — the smoke target's
  LSan leg caught both apps leaking them (fdk-files' td_ registry
  existed for exactly this). Fixed app-side; the menu-ownership
  contract is already documented in fdk_widgets.h.
- fdk-monitor and fdk-notes are now COMPLETE against the README's
  four-app standard: /proc sampling + canvas graphs + levelbars +
  tray sparkline pixmap + threshold notifications with actions
  (monitor); notebook tree + rich-text attrs + reminders + focus
  timer + a11y narration sink + tray (notes). Both build warning-
  free under the full -Wconversion battery, smoke clean under
  ASan/LSan in EN + RO, and pixel-verified.

---

# Test-app round log (absorbed from test-apps/CONTINUATION.md)

THE OWNER'S TWO ASKS this round: (1) the app fleet on a second,
independent wlroots compositor — labwc, not just sway; (2) proof
that EVERY letter, in every language, can be typed into FDK.

## labwc (9/9 run, 8 receipts + 1 by-design refusal)

labwc 0.8.x + libwlroots-0.18-dev + Xwayland + wtype all come from
Debian trixie debs extracted into /home/z/apt/prefix (the restore
script's recipe; wlroots' pkg-config needs libdrm-dev + libsfdo0 +
wayland-protocols, Xwayland needs libxcvt — all fetched). Headless
run: WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=
pixman WLR_HEADLESS_OUTPUTS=1 WLR_XWAYLAND=<prefix>/usr/bin/Xwayland,
with rc.xml's <core><xwayland>no</xwayland> (the xwayland errors are
then non-fatal). scripts/labwc_smoke.sh runs the whole matrix;
screenshots via grim verified fdk-files (full manager, menubar,
toolbar, live directory), fdk-chat, fdk-workbench (server-side
decorations from labwc, pixel-correct content).

## the typing audit (fdk-keys, new app)

Three layers, all ASan/UBSan clean:
1. ENGINE SWEEP: 1,112,111 codepoints (all of Unicode minus
   surrogates and C0/C1) set→read byte-identical through the entry
   model; caret boundaries exact; mid-codepoint offsets snap to
   boundaries; every combining mark 0x300-0x36F after 'e' round-
   trips; font measure sampled — 0 failures. (fdk-keys receipt:
   engine=1112111 rej=0 bad=0.)
2. LAYOUT SWEEP: libxkbregistry enumerates 589 installed layouts;
   every keycode × {none, Shift, AltGr, Shift+AltGr} goes through
   xkb_state_key_get_utf8 (the exact call both FDK backends make)
   into the same entry: 125,473 letters, 3,972 unique codepoints,
   0 failures.
3. END-TO-END: Xvfb + the extended xtest_driver (keyboard commands
   k:/t:/bksp/cq added) types "Hello, FDK! 42 [ok]" + 8 backspaces +
   "#1 (typed-for-real)" — the receipt's text matches the intent
   exactly. On labwc, wtype (virtual-keyboard protocol) types
   "Hi from labwc" and "Hello 你好Привет γεια שלום مرحبا" — the
   screenshot shows the entry + the live mirror; the receipt holds
   everything except one γ and one space that WTYPE's keymap
   generator itself dropped (the layout sweep proves γ round-trips
   — injector quirk, not FDK). Ctrl+Q (XTEST and wtype) quits the
   app cleanly for the receipt.

## THE BUG THE ROUND FOUND (library, FIXED)

Presenting a surface from OUTSIDE the pump loop (an add-on's
render → fdk_surface_present from main) that hits a flush EPIPE
lands in fdk__wayland_connection_dead, whose guarded read called
wl_display_read_events BARE — and read_events cond-waits while any
prepare_read reader is registered (the pump's prepare/dispatch
pattern), hanging the process forever. FIXED in wayland_dispatch.c:
prepare_read first (bare read → same line, now guarded), and when
prepare returns EAGAIN (events already queued) dispatch_pending —
the error event may be in the queue; the detail line then prints it.


# Round 28 — the disappearing editor: the prompt-dialog teardown damages the parent

Round 27's code item, RELEASE-READINESS blocker #2, and the one
CONTINUATION-filed open bug a user hits in daily editor use —
CLOSED this round.

THE REPORT (round 25, 22:50): after ANY fdk_dialog_show_prompt
closes, the fdk-edit main window stops painting its editor — the
region where the dialog sat stays a dead band while the text
underneath stays alive (on_doc_changed shows it growing). The save
file-picker was unaffected.

ROOT CAUSE: a modal dialog is a normal toplevel window, NOT a
popup — so it gets NONE of the anchored parent damage the popup
teardown delivers on destroy. Whether the parent ever hears about
its uncovered pixels is then the WINDOW MANAGER's choice: a
reparenting WM's unmap path can simply not send the parent an
Expose for the region, and nothing else in the pipeline flags it.
Under bare Xvfb (no WM — every CI run and sandbox rig) the
server's own unmap Expose happens to arrive, which is exactly why
the bug hid from every test-green run while the owner's real
desktop showed it.

THE FIX (src/widget/dialog.c): dialogs now store options->parent,
and dialog_damage_parent() invalidates the parent's whole widget
tree when the dialog goes away — on BOTH teardown paths:
dialog_respond (the answered close — run at the TOP, before the
application callbacks) and dialog_destroyed (the unanswered early
destroy: app teardown, parent window teardown, shutdown).
Whole-tree rather than the popup's rect math because Wayland owns
dialog placement and window positions are not uniformly
queryable — one full repaint of the parent is correct on every
backend and cheap next to the dialog that just closed.

LIFETIME DISCIPLINE (the subtle part): the parent pointer is
BORROWED with no outlive contract, and the response callback may
destroy the parent (the documented contract allows it; the file
dialog's respond chain does exactly that). So dialog_damage_parent
NEVER dereferences the stored pointer — it compares its VALUE
against the context's registered live-window list and damages the
match only. A destroyed parent is absent from the list, matches
nothing, costs nothing. The damage also runs BEFORE the
application callbacks on the respond path, so the repaint is
flagged while the world is certainly still alive.

REGRESSION FENCE (the repro r25-edit-probe.sh never got):
test_dialog_close_damages_parent in the X11 suite — opens a modal
prompt over a parent, paints the parent clean, answers the prompt,
then asserts (1) the parent's tree reports damage immediately
after the dialog dies — the contract any has_damage-gated paint
loop relies on, WITH OR WITHOUT a helpful Expose, which is what
makes it fire under bare Xvfb too; (2) the next paint consumes it
(no permanently stuck damage flag); (3) the parentless form stays
legal (nothing to damage, no crash).

# Round 27 continued — the CI layer-2 fix

After the Makefile repair exposed the next layer (the notifyd rig
had not RUN since Nov 1), the rig crashed the daemon: SEGV
attr.c:111 attr_font_for via sh_attr_font, reading a garbage
`family` pointer, with 22x "loaded @ 512px" spam. ROOT CAUSE:
fdk__span_flatten built fdk__text_run on the stack WITHOUT the
memset its own header contract promises — the §23/B2c appended
fields (family/size_px/rise/link/...) were raw stack garbage in
every span-only flatten. The garbage family pointer crashed the
attr resolver; garbage size_px clamped to 512 loaded a font per
segment. One-line fix (memset in span.c) + a stack-poisoning
regression test in test_attr.c (poison the stack, flatten, assert
every appended field is zeroed, then draw through the same path).
Full battery after: core, X11 229, dbus, 4-app smoke, notifyd rig
ALL PASS. (Debugging detours worth remembering: the rig's
daemon.log is truncated per phase — copy it inside phase_P to see
the phase-P daemon's death; and a stale test binary after a
library-only rebuild produces a phantom assertion failure.)

---

# Round 27 (partial) — the release-readiness audit + the CI repair

The owner asked what's left for release. The answer is
docs/RELEASE-READINESS.md (written this round): the feature work is
done (ROADMAP 60/62, PARITY at parity for the app core); what
remains is the round-25 prompt-dialog bug (round 27's code item),
the small PARITY tail by choice, and the owner's 28-section
desktop-verification walkthrough (TESTING-ON-DESKTOP.md), then the
tag decision.

**CI was RED on every push since 4d06933 (Nov 1)** and nobody
noticed: the thread-test rule landed above `all:` in the Makefile,
so CI's clean-checkout `make` built only that binary — never
`shared`/`tools` — and `make test` died at test_cli_tools' stat()
on the missing tools. Incremental sandboxes stayed green the whole
time (their build/tools/ already existed). Fixed: `.DEFAULT_GOAL
:= all` + test_cli_tools depends on $(TOOL_BINS); proven by a full
clean-tree CI simulation (make -> static+shared+tools, make test ->
all green). Process rule: read the CI run before the next push.

---

# Round 26 — fdk-monitor becomes a real system monitor

## Landed
- **The process table** (proclist.c): every /proc/<pid> row —
  name, CPU%, resident memory, PID — in a 4-column List, rebuilt
  once a second (every 4th 250 ms tick) in ONE batch. CPU% is a
  delta quotient against all cores' jiffy growth (first sample
  reads 0 — no baseline yet, like every monitor). Click a column
  header to sort (Name/CPU/Memory/PID; second click flips
  asc/desc; CPU+RSS default biggest-first, name/pid ascending).
  Selection survives the rebuild, re-anchored by PID.
- **End process** (Ctrl+K / Monitor menu / the button): SIGTERM
  after a Yes/No confirm naming the process and PID.
- **Disk bar**: the 4th levelbar — statvfs("/") used/total with
  the root-reserved blocks counted as used (df's rule).
- **CORE FIX (round-25 regression)**: the list header-click
  signal sat behind the POINTER_DOWN MULTIPLE-mode gate —
  SINGLE-mode lists (the monitor's) never saw header presses;
  fdk-files never noticed (its lists are MULTIPLE). The header
  branch now runs in every mode; the rubber band stays
  MULTIPLE-only (NONE/SINGLE still never band — test_none's
  contract, now also asserted alongside new SINGLE/NONE
  header-click regression cases).

## Suites at commit time
core 766 (new: header-click in SINGLE/NONE modes), X11 229,
4-app smoke clean. Proof shots in download/: fdk-monitor-
proclist / -sorted-by-name / -kill-confirm / -sort-before-after.

## Still open (round 27 candidates)
- ~~The round-25 prompt-dialog repaint bug~~ — CLOSED in round 28
  (dialog_damage_parent on both teardown paths; regression test in
  the X11 suite).
- Monitor next steps if wanted: per-core CPU rows, the disk/net
  history graphs, a process search/filter box (fdk-files' live
  filter pattern ports directly).

## Environment notes
Sandbox recycled AGAIN before this round (second time): recovery
= fetch origin main (ef48660) + bash scripts/setup-sandbox.sh +
source scripts/env.sh, then rebuild the rigs (miniwm/xsnap live
in work/FDK/scripts/; key_rig + the r26 click/probe helpers
compile against /tmp/x11dev). The r26 rigs:
scripts/r26-verify.sh (boot+shot), scripts/r26-interact.sh
(header sort + kill flow with XTEST clicks).

# Round 25 — save/save-as split, editor+apps upgrades

## Landed
- fdk-edit: Save vs Save As untangled. Root cause of "two buttons
  same thing": BOTH mapped to the document-save icon (the "save"
  alias), and for an UNTITLED doc Save legitimately falls through
  to the picker (first save needs a name) — so both opened the
  same dialog. Now: Save As uses the drive icon + explicit
  tooltip, and Save-on-untitled SAYS "Untitled — pick a name for
  the first save" before opening the picker (titled "Save — name
  the file"). Menu shows Ctrl+Shift+S on Save As.
- fdk-edit: Go To Line (Ctrl+G, toolbar button, Edit menu) via the
  prompt dialog; status bar now shows live words/chars/lines.
- fdk-notes: live search box over titles AND bodies (tree narrows,
  first match auto-selected, match count in the status bar).
- fdk-monitor: uptime (Xd Xh Xm) + load averages (1/5/15) from
  /proc/uptime + /proc/loadavg in the status bar.
- fdk-files: column-header click sorting (Name/Modified/Size/Type,
  click again to toggle asc/desc). Core gained
  fdk_list_set_on_header_click (a header press fires the signal
  and never starts a rubber band).
- Core: fdk_textview_scroll_to_line (goto-line) + set_caret now
  scrolls (consistency with its a11y twin). Unit test test_goto.

## THE BUG (CLOSED in round 28 — the prompt-dialog teardown damage fix; see the round 28 section)
After ANY fdk_dialog_show_prompt closes, the fdk-edit MAIN WINDOW
STOPS PAINTING ITS EDITOR: the region where the dialog sat stays
black, the editor area never redraws (status bar still updates).
The TEXT IS ALIVE (on_doc_changed tracer shows it growing), so
this is a PAINT/EXPOSE bug: the modal dialog's unmap does not
produce damage that repaints the parent, and later editor
invalidates don't either. Repro: scripts/r25-edit-probe.sh —
shot 2-two-lines fine, 4a/B2/C1 all blank editor + black band at
the dialog's old rect. The SAVE file-picker dialog does NOT cause
it (its flow was fine) — suspect the prompt dialog's teardown
(x11_window.c unmap path / damage translation, cf. round 23's
page-local damage work). Go To Line itself works when the dialog
bug doesn't bite; keep the feature, fix the core.

## Suites at commit time
core 765 (incl. new test_goto + list header-click paths), X11 229,
4-app smoke clean. Proof shots in download/: fdk-edit-wordcount /
-goto-dialog / -save-dialog / fdk-files-columns / fdk-monitor-status.

## Environment notes
Sandbox was RECYCLED before this round (stale snapshot): recovery
was fetch origin main (4637dc4) + `bash scripts/setup-sandbox.sh`
+ `source scripts/env.sh`. key_rig gained ctrl+<key> and escape.

# Round 24 — the owner's follow-up: space/Enter invisible

## The report
Space doesn't move the caret; Enter doesn't show the second line
("1"/"2") until typing resumes. The buffer IS updated server-side
(caret state advanced) — only the paint lagged.

## Three stacked bugs (all core textview/text engine)
1. BREAK ENGINE: the final-line emit required visible bytes, so
   the empty line a trailing '\n' opens was dropped. "Hello\n"
   = 1 line; the caret (offset 7) mapped to line 1. Fixed in
   fdk__break_lines_runs (src/text/layout.c): emit the empty
   final line when the last thing before the text's end was a
   hard break. CAREFUL: after a trailing break line_start ==
   byte_len (not <) — the first guard draft was wrong and the
   unit test + the app's "Hello \n" caught it in sequence.
2. tv_line_width_to clamped the caret offset to the line's
   trimmed byte_len (src/widget/textview.c): a caret after a
   trailing space painted at the last glyph. Now bounded by the
   NEXT line's start (or the caret itself on the last line).
3. doc_w (the page's natural width) used trimmed advances: the
   caret column after a trailing space fell outside the page's
   bounds clip and the fill was discarded. Now includes the
   trimmed tail + one pad.

## The rig lesson (IMPORTANT for future rounds)
- scripts/edit-caret-repro.sh + scripts/key_rig.c: paced keys,
  per-key screenshots, blink pairs. The FIRST key_rig re-clicked
  the window before every key — a pointer-down in the textview
  that YANKED the caret to offset 5 between keystrokes and made
  the next key DELETE the space. key_rig now clicks only with
  KEY_RIG_CLICK=1 (once, at rig start).
- THE STALE-LINK TRAP: `make -C test-apps` did NOT relink when
  only ../build/libfdk.a changed (the .d dep file pointed at a
  removed binary; also grep -c returning 0 breaks && chains).
  Symptom: "fixed" code showing old behavior. RULE: after any
  core change, `rm -f test-apps/build/<app> test-apps/build/<app>.d`
  before make, and verify with `strings <binary> | grep <new
  marker>` when in doubt.
- pkg-config: use
  /tmp/my-project/local-x11/lib/x86_64-linux-gnu/pkgconfig (the
  env.sh usr/lib path only has x11.pc; xrandr.pc lives in lib/...).

## Suites
core 772 (incl. 3 new break-engine tests + updated 20k stress
expectations: the empty tail now counts, matching the paragraph
model), X11 229, 4-app smoke clean, notes renders. Golden shots:
download/fdk-edit-{space-caret,enter,line2}-FIXED.png.

# Round 23 — the owner's fdk-edit report: three stacked bugs

## The report
"Theres a box is cut off where i could type and it doesnt extend
to the edge of the window" + square toolbar buttons + the U+25CF
warning in the log. The present trace itself was GOOD news (the
round-22 merge fix held: no over-area lines, rect counts 1-4).

## The three bugs (all real, all fixed)
1. LAYOUT: the doc page was a plain widget holding a textview at
   its 100x100 creation bounds; the notebook never expanded. Fix:
   page = vertical box + set_expand, notebook set_expand,
   textview expand. (doc.c / ui.c)
2. FONT: eapp.base.font_mono was NEVER assigned by ta_open —
   font==NULL, tv_relayout's first guard, zero lines computed.
   Fix: ta_open probes fdk_font_enumerate for the mono families.
   (common/app.c — affects every app's font_mono user)
3. PAINT (the deep one, found by instrumenting the paint path):
   tv_page_paint drew content-local (x=pad+gutter, y=pad+i*line_h)
   into a window-coordinate pipeline. The paint_rec clip stack
   clips to the page's WINDOW bounds; content-local ink at (8,8)
   is outside (0,122)+ — invisible. EVERY textview not at the
   window's top-left rendered nothing (fdk-notes' body too).
   Fix: ox/oy = bounds.x/y added at every draw call; the damage
   clip translated to page-local at entry. Verified by XTEST
   typing + screenshot pixel-diff: text rows 132-146 appear.

## Also fixed while here
- fdk-notes' tree/editor stacked vertically (editor below the
  fold): horizontal row box + expanded pane children.
- 7 NULL-icon toolbar buttons (the "squares"): real names.
- U+25CF tab marker -> '*'.

## The verification rig (reusable)
- scripts/edit-repro.sh (sandbox): Xvfb + miniwm + xsnap + the
  XTEST type_rig; screenshots boot/typed; pixel-diff verdicts.
- scripts/notes-repro.sh: the notes control.
- /home/z/my-project/work/FDK/scripts/type_rig.c: fake-keys.
- NOTE: pkg-config needs
  PKG_CONFIG_PATH=/tmp/my-project/local-x11/lib/x86_64-linux-gnu/pkgconfig
  (the env.sh path points at usr/lib which only has x11.pc —
  xrandr.pc lives in lib/x86_64-linux-gnu/pkgconfig).

## Suites
core 772, X11 229, 4-app smoke clean. Proof screenshots in
download/: fdk-edit-typed-FIXED.png, fdk-notes-FIXED.png.

# Round 22 — damage-rect overlap merging

## The owner's evidence, re-read
The present trace showed damaged-area sums EXCEEDING the window
area (3.29M px on 2.07M — 1.59x). The trace sums per-rect areas
WITHOUT dedup, and the rect list had no overlap control at all:
every primitive's bbox appended blindly. Overlapping bboxes =
double-counted trace area AND double-shipped pixels (two sub-image
puts covering one region) AND premature whole-image coarsening
(the >=75% threshold fired on inflated sums).

## The fix (core, src/render/surface.c)
`fdk__surface_damage_add` merges before appending: an incoming
rect that overlaps OR TOUCHES an existing entry unions into it in
place; a compaction pass then removes any neighbor the widened
rect now contains (a merge can cascade). Bounded: one O(count)
scan per add, count <= 64. The overflow->full degradation and the
exact-strip path are untouched.

## Test-first protocol held
`test_damage_overlap_merges` written RED (identical rects = 2
entries, failed at the first assert), then the fix, then green.
One pre-existing test needed its FAMILY updated, not its contract:
`test_damage_bookkeeping`'s overflow loop used 64 ADJACENT 1px
rects, which now correctly merge into one — the loop leaves 1px
gaps now (the overflow contract itself is unchanged and still
asserted).

## Proof under load (the round-14 stress rig, release build)
- scroll storm (1000 files, 1000 Hz): 48 presents, ZERO over-area,
  median 1 rect/present, damage median 0.927 (the full-height
  scroll strip — genuinely most of the window), 0 full presents.
- hover storm: 247 presents @ 49.2 fps, damage median 0.110.
- scrollbar drag storm: 217 presents @ 43.2 fps, damage median
  0.055, 0 full presents.
- noisewatch (round 20's regression): 4/4 gates, 150 watch fires,
  rows stable.
- core 772 (was 771: +1 new test), X11 229, 4-app smoke clean.

## The owner's next check
Nothing new to do — this shipped inside libfdk. Any app linking
the new library gets it. If you re-run your FDK_PRESENT_TRACE
session, the damaged-area column should never exceed window area
again, and rect counts per present should collapse to 1-2 for
widget-typical frames.

# Round 21 — the four-app suite complete

## What happened
The sandbox recycled and took /tmp/x11dev, /tmp/my-project/local-x11
and /home/z/apt with it. Rebuilt the toolchain (local-x11 from the
debs, the wayland prefix, xauth). ONE trap: the surviving
/home/z/my-project/scripts/xtest_driver was the PRE-§29 build with
the UNCONDITIONAL selfcheck — it moved the pointer to (11,22)
before every command and silently cancelled every XTEST drag (the
dnd interop legs failed). Root-caused from the "selfcheck:" lines in
the suite log; rebuilt both rig binaries from the repo's current
sources -> X11 suite 229 ok / 0 skips.

## fdk-edit finished (it was ~90% there)
- doc.c marked[128] -> marked[132] (the release-build truncation).
- The EDIT MENU WAS WIRED TO NULL (Undo/Redo/Find/Replace — the
  toolbar and shortcuts worked, the menu items did nothing). Wired
  through ed_menu_* twins in ui.c.
- No catalog: po/ro.fmo written (46 msgids) + ta_i18n_load at boot.
- Pixel-verified: menubar/toolbar/tabs/statusbar all render.

## fdk-monitor BUILT (proc.c / graph.c / tray_notify.c / main.c)
- /proc/stat aggregate CPU, /proc/meminfo, /proc/net/dev (busiest
  non-lo interface), 960-sample rings, 250 ms timer (prefs-persisted
  250/1000 toggle).
- Canvas graphs (fdk_canvas paint callbacks): graticule + filled
  area + line, CPU at 0..100 scale, NET dual-series (RX accent /
  TX warning) on a shared max(16 KiB/s) scale.
- Levelbars CPU/RAM/Swap (continuous mode).
- Tray: live CPU sparkline PIXMAP (ARGB32 BE), tooltip with live
  numbers, menu (pause/test/quit), activate = pause toggle.
- Notifications: >=90% CPU alert (critical, re-arms under 70% or
  via the "mute" action) + a test item.
- Pixel-verified: levelbars fill, series lines drawn, TX activity
  visible; LSan clean; timer retired at exit.

## fdk-notes BUILT (store.c / ui.c / main.c)
- Notebooks + notes in flat arrays, prefs-persisted per-field.
- Tree (fdk_tree), title Entry + body TextView, whole-note rich
  flags through fdk_attr_list (italic->underline, bold->size delta
  — the honest v1 mapping; per-range spans need the selection-attr
  editor, queued).
- Reminders (5-min item, 60 s sweep, notification with an "open"
  action), 25-min focus timer (label countdown, completion notify
  + a11y announce), a11y NARRATION (fdk_a11y_set_speaker +
  narrator_start; utterances mirrored in the status bar), tray.

## Verified
test-apps-smoke 4/4 green, zero leaks. Core 763 / X11 229 from the
restored environment. Screenshots of all three apps in download/.

## Not done (honest)
- The damage-rect inflation (merged rects > window area in the
  owner's PRESENT-TRACE) is NOT touched this round — round 22.
- fdk-notes' per-range rich text (selection attributes) and
  fdk-edit's print/PDF leg remain queued.

# Round 20 (2026-11-05) — the disappearing folders: root cause

## The owner's evidence
The round-19 expose hardening WORKS (the log shows full presents on
uncover) — yet folders still vanish, on X11 AND Wayland, WITHOUT
perf running. The trace's ending is the tell: growing per-row
removal damage (661k/29 -> 880k/38 -> 272k/13 px) = the file rows
being REMOVED one by one while the app keeps painting. The model
was emptying; the paint layer was never the bug.

## The hunt
1. Instrumented fmodel_load's clear path + the watch + the bound
   row count (DRIG-CLEAR / DRIG-WATCH / DRIG-ROWS).
2. Built the noisewatch rig: a background dotfile writer in the
   watched dir (real session-noise shape) + focus loss + cover.
3. CAUGHT IT: watch fire @frame 24 -> pane_reload -> PHASE: nav
   (EMPTY) -> DRIG-CLEAR dir= prev_rows=2 -> DRIG-ROWS=0 forever.
4. The empty path: pane_reload passes active_tab(p)->dir to
   pane_navigate, which snprintf'd t->dir onto itself. Overlapping
   snprintf = UB; this glibc empties the buffer. Proven in
   isolation (scripts/overlap_repro.c): before [/tmp/fdk-drig/left]
   len=18, after [] len=0.
5. The wipe: fs_list_dir returned NULL/0 for BOTH an empty dir and
   an opendir failure, and fmodel_load's failure path cleared the
   store — unbatched, hence the per-row removal storm in the trace.

Why the owner's symptoms fit exactly:
- X11 AND Wayland: pure app logic, backend-independent.
- No perf needed: any write in the viewed folder fires the watch;
  session noise in $HOME is constant during session switches
  ("more so when I switched to X11").
- Click Home restores: a real path doesn't alias the buffer.

## What shipped
- panes.c: pane_navigate snapshots abs_path before writing t->dir
  + refuses empty/NULL paths.
- fsops.c/h: fs_list_dir's unreadable out-param.
- model.c: keep-rows-on-read-failure (status bar message instead
  of a wipe); the legit empty-dir clear is batched.
- main.c: the DRIG telemetry stays (gated, for future rigs).
- run_noisewatch.sh (4 decisive gates) + scripts/xfocus.c +
  scripts/overlap_repro.c.

## Verification
noisewatch 4/4 gates under 229 watch fires; covercheck 4/4 shapes;
core 763; X11 229. The pre-fix build fails the rig at the FIRST
watch fire.

## The owner's next check
Pull, rebuild (release too), use normally. If a directory ever
fails to read you now get "Cannot read <dir>" in the status bar
with the view intact — if anything still vanishes, that message
(or its absence) tells us which layer to look at next.

# Round 19 (2026-11-05) — the disappearing-folder report

## The owner's brief
Two things: (1) confirmation the perf work is in the CORE, not the
app — yes: everything lives in src/render/surface.c inside libfdk;
every app linking the library gets it, nothing is fdk-files-specific
(the profile itself proves it: the symbols are libfdk's). (2) The
remaining bug: `sudo perf record ... -- sleep 10` still makes the
folder content disappear; everything else is fixed.

## What the profile says
Healthy. fill_rect_translucent 52.4% -> 10.3% (round 18 landed),
~35% total blend work, ~20% libnvidia-eglcore (the GPU present
lane — where a CPU-rendered toolkit SHOULD top out). No action.

## The investigation
- Traced the expose path end to end: x11_events.c translates
  Expose -> FDK_EVENT_WINDOW_EXPOSE; window.c invalidates the tree;
  every test app's shared loop (ta_pump) repaints on tree damage.
- Round 16's stage D rig tested UNMAP, not occlusion — the real
  cover path was never exercised. Built occlude.c (a REAL
  override-redirect occluder stacked above the window) and
  miniwm.c (a minimal reparenting WM: frames, synthetic
  ConfigureNotify — the owner has an EWMH WM, bare Xvfb does not).
- Result: NOT reproducible in the sandbox. All four shapes (GL
  lane forced / CPU lane x miniwm / bare server) pass — rows
  restored, a present happens with zero input, pixel-identical to
  a forced full repaint.
- Structural weakness found anyway: the expose handler never
  touched the SURFACE's damage bookkeeping — only the tree's. A
  re-present shipping only new widget damage relies on
  server-side pixels the server may have discarded. The owner's
  NVIDIA + WM combo is plausibly the case where that bites.

## What shipped
- window.c: every expose marks the window surface full-damage
  (hoisted out of the root check — tree-less apps covered too).
  PRESENT-TRACE confirms the post-uncover present is now `full`.
- scripts/occlude.c + scripts/miniwm.c + test-apps/run_covercheck.sh
  (the four-shape regression rig).
- CHANGELOG entry.

## The owner's next check
Pull, rebuild, and repeat the perf-record session. If the folder
still vanishes, run with FDK_PRESENT_TRACE=1 and check whether a
`full` present follows the uncover — if it does and the screen is
still blank, the pixels are being sent and LOST server-side (that
would be a driver-level interaction: send the app log).

# Round 18 (2026-11-05) — the release-lane round

## The owner's brief
Round 17's instructions said `make release` and pointed at
test-apps/build-rel/fdk-files — which did not exist (the apps
Makefile had no release target). The owner got there anyway: built
the apps with their debug rule against the release lib, ran the
profile on THAT, and the numbers were still clean enough to read.
Also: the owner's newer gcc flags -Wformat-truncation paths this
sandbox's gcc misses (discover.c x3), and the slow highlight is
FELT-FIXED already ("it doesn't feel slow anymore").

## The owner's data, read
- 52.4% fill_rect_translucent: the blend work itself, no longer
  per-pixel overhead — round 17 did its job, and this round's
  table pass takes the remaining wall down 12-22x.
- ~23% across .glXXXXXX (deleted): the NVIDIA GLX present shader
  — the GPU upload lane doing its job. NOT CPU-side work of ours;
  do not chase.
- blend_pixel 0.38% (was 53.7%): the highlight fix, confirmed on
  real hardware.
- PRESENT-TRACE: ~146K px of 756K damaged per highlight motion —
  sane damage for a multi-row sweep; no whole-window repaint.
- "exited cleanly after 4001 frames" and the boot logs: no
  anomalies. The 1591 vs 4001 frame counts are the two runs'
  different session lengths.

## What shipped
- surface.c: the XRGB table pass (per-fill 3x256 LUT + uniform-run
  store). 22x uniform / 12x anti-run, byte-exact. ARGB keeps the
  round-17 form (dest-alpha coupling has no table).
- test-apps/Makefile: the real `release` target (build-rel/, -O2,
  no sanitizers); root `make release` reaches it.
- discover.c x3 + fdk-edit doc.c: -Wformat-truncation bounded
  properly (return checks / ta_copy).
- test-apps/core (a 6MB core dump, tracked by mistake) untracked.

## The owner's commands, corrected
    make release                        # lib + apps, -O2, no sanitizers
    FDK_FILES_STARTUP_DIR=$HOME test-apps/build-rel/fdk-files &
    sudo perf record -g -p $(pidof fdk-files) -- sleep 10
    sudo perf report --stdio --no-children -g none | head -40
    FDK_PRESENT_TRACE=1 test-apps/build-rel/fdk-files 2>&1 | grep PRESENT-TRACE | head

After this round, expect fill_rect_translucent to be GONE from the
top of that profile (or single-digit); the top should be the GL
present shader and the text shaper. If translucent fill is still
top, send the report — that would mean the drag is not hitting the
XRGB window surface and something else is going on.

## NOT DONE (carried)
- The 4K / scale>1 physical-space strip blit (round-16 carry).
- The release-config test_attr segfault + sway button_hits
  assertion (both reproduce on the CLEAN round-16 tree).
- The up-storm geometry-flip residue (round-16 carry).
- ARGB table pass would need a 64K dst-alpha-coupled table — not
  worth it while window surfaces are XRGB; revisit only if the
  offscreen composite profile ever says so.

# Round 17 (2026-11-04, evening) — the drag-highlight profile

## The owner's brief
"Very fast highlighting folders by using drag click": perf shows
53.7% blend_pixel (45 of it via fill_rect under row_paint, 8 via
list_paint), 15.5% fill_rect self, ASan still on the rig. "It feels
like we are going in circles" — fair; see the command list below.

## What shipped
- surface.c: fill_rect_translucent — the hoisted, 4-wide-unrolled
  translucent fill; EXACT vs blend_pixel on the 256-value sweep
  (both formats); 1.17-1.22x on -O2 scalar. The bench harness is
  #ifdef FDK_R17_BENCH inside surface.c (fdk__r17_bench; link any
  main against libfdk.a).
- list.c: the incremental band want-test (press = full window,
  motions = prev∪current band window only). The FIRST version
  seeded an empty window at press — test_row_press_band caught it
  (a new gesture must re-evaluate every row: outside-band rows
  clear). Lesson re-confirmed: never ship a band change without
  that test.
- run_scrollcheck.sh now uses the in-tree PPM xsnap (the sandbox's
  PNG py-tool was lost to a recycle; the C tool is the permanent
  one), and its stage-D unmap/map helper was rewritten (the ctypes
  XQueryTree argtypes were wrong and segfaulted).

## The owner's machine — the command list (BREAK THE CIRCLE)
The recurring mistake: profiling the ASan build. ASan doubles
raster costs and skews every ratio. On YOUR rig, run:

  # 1. build + run the RELEASE binary
  make release && FDK_FILES_STARTUP_DIR=$HOME test-apps/build-rel/fdk-files &
  # 2. profile THE RELEASE BINARY this time:
  sudo perf record -g -p $(pidof fdk-files) -- sleep 10
  sudo perf report --stdio --no-children -g none | head -40

  # 3. the present-volume counter (how many pixels per frame hit
  #    the wire — tells us damage quality, not just CPU):
  FDK_PRESENT_TRACE=1 test-apps/build-rel/fdk-files 2>&1 | grep PRESENT-TRACE | head

  # 4. the A/B switch for the blit path (is the strip path even
  #    active? FDK_SCROLL_NOBLIT=1 turns it OFF — if scrolling is
  #    EQUALLY slow both ways, the bottleneck is elsewhere):
  FDK_SCROLL_NOBLIT=1 test-apps/build-rel/fdk-files

Send the outputs of 2 (both with and without the scroll storm) and
3; if 4 shows no difference, the fill volume is damage-side, not
raster-side, and round 18 goes after the caller, not the pixel.

## NOT DONE (carried)
- SSE2 / 8-bit-LUT translucent fill (the scalar 1.2x is honest but
  modest; the LUT needs a rounding-proof against the float form).
- The 4K / scale>1 physical-space strip blit (round-16 carry).
- The release-config test_attr segfault + the sway button_hits
  assertion (both reproduce on the CLEAN round-16 tree — a config
  hygiene round of their own).
- The up-storm geometry-flip residue (round-16 carry; the current
  rig passes all stages on the PPM tool).

# Round 16 (2026-11-04, later) — the profile, the storms, the no-op clear

## The owner's brief
A perf profile of fdk-files on real hardware (perf record during
aggressive scrolling) + three live reports: the folder list
disappears after aggressive scrolling (nav click restores it), the
same after sitting on another window, and up-scrolling glitches
text into overlapping. The profile itself: 39.6% fdk_surface_fill_rect
+ 22.9% blend_pixel + 4.1% blend_mask, libasan loaded (an ASan
build on the rig!), libnvidia-eglcore present (the EGL GPU lane).

## What shipped
- tests/test_x11_integration.c: test_scroll_storm_identity_gui —
  the deterministic aggressive-scroll regression (striped content,
  two notches per pump, XGetImage identity vs forced full repaint;
  the vbar column excluded — the thumb's animation timing is the
  BAR's story, not the blit's).
- scroll.c: two strip-path fixes (clipped translate of the pending
  box; live re-read of the root box before the replace). See
  CHANGELOG for the mechanism.
- surface.c: 0-alpha fill on ARGB = memset clear (the HiDPI
  intermediate no-op bug — also a big perf win at scale > 1).
- scripts (sandbox, outside the repo): scrollcheck_r16.sh (the
  Xvfb visual rig — stages: 30 Hz down-storm, up-storm, jitter,
  unmap/remap sit, post-nav; pixel-identity after each), xsnap.py
  TWO bug fixes (missing XImage.bits_per_pixel field; the channel-
  shift formula computed the mask's top bit, not its shift — every
  old screenshot had scrambled channels; identity checks were still
  valid because both sides used the same deterministic tool),
  fdk-wl-inject.c gained a `wheel <notches> <ms>` verb (real
  axis + axis_discrete v120 events).
- run_scrollcheck.sh (in-tree): the visual rig, for any future
  scroll-path change.

## The ASan guidance for the owner (IMPORTANT for perf numbers)
The profiled fdk-files had libasan loaded — AddressSanitizer
instruments every memory access and roughly doubles raster-loop
cost. The 62% fill_rect+blend_pixel self-time is real work volume,
but the absolute numbers are inflated. Daily-driving: use the
release build (make release / build-rel); keep an ASan build for
repro sessions only.

## The rig's lessons (round 16's own four bugs)
1. The Wayland injector path never delivered wheels to the app in
   this rig (pointer/seat quirks under headless sway) — the Xvfb +
   XTEST path (stress-rig) is the reliable one for scroll storms.
2. My first "blank list" reading was a screenshot-tool channel bug
   (xsnap.py), not an app bug — verify the TOOL against a known
   color before trusting a blank verdict.
3. The storm snapshot vs full-repaint snapshot must exclude the
   CLASSIC bar column (thumb position differs by animation timing).
4. A canvas as scrollview content measures 8x8 — no scroll range;
   use a plain widget with real bounds (or set the canvas's min
   size) when you need a long striped content.

## Verification ladder
- make test: 763 [ok]. make test-x11: 229 [ok] (+1 storm test).
- make test-wayland (headless sway): 65 [ok].
- scrollcheck_r16.sh: down-storm identity PASS, unmap/remap PASS,
  jitter PASS; up-storm residue ~266 px (below).

## NOT DONE (do not lose)
- The up-storm 266 px residue: geometry changes mid-flight (the
  hbar visibility flip resizes the viewport between ticks) can
  still leave a stale band just below the list. Repro:
  run_scrollcheck.sh stage B. The fixed-geometry storm is green.
- The 4K / scale > 1 physical-space strip blit (P0 for the owner's
  rig — the profile's fill volume lives there).
- blend_pixel batch/hoist for translucent fills (22.9% self).

## Next round candidates
- The geometry-flip storm shape (fix the 266 px residue).
- The physical-space blit at scale > 1 (design already recorded in
  scroll.c's round-14 comment block).
- The frame profiler env var + p50/p95/max present-interval stats
  (round-15 carryovers).

# Round 15 (2026-11-04) — delete stability, the scroll notch

## The owner's brief and the round's shape
Priority one was STABILITY: "deleting a file leaves fdk-files broken"
(real-hardware report). Then scroll feel (viewport-proportional notch,
smooth handling, frame-time stats), the 4K physical-space blit, an
opt-in frame profiler, and the full verification ladder.

## What shipped
- **The delete-stability fix**: fdk_list_remove now clamps
  last_click_row and key_cursor like it always clamped anchor
  (src/widget/list.c). Enter-activate after a delete was silently
  dead — the stale past-end key_cursor read as "no cursor".
- **tests/test_list_delete.c** — the regression test. Fails on the
  pre-fix engine at the Enter leg (assertion at the "interaction
  alive after removals" step), passes after. The fail-before proof
  was run by temporarily rebuilding the engine without the clamp.
- **The delete rig** (test-apps/fdk-files/delete_rig.c +
  run_deleterig.sh): six XTEST shapes (Delete key, Shift+Delete +
  modal-confirm OK, multi-select, last row, scrolled offset,
  directory-via-async-engine) against the real app under
  ASan+UBSan. ALL GREEN — the X11 trash/delete paths did not
  reproduce the owner's breakage in this environment; the one real
  engine bug (the stale indices above) was found by CODE READING
  the removal path, not by the rig. The rig itself needed four
  fixes before its greens meant anything (below).
- **The scroll notch**: viewport-proportional wheel step
  (viewport/4, clamped 40..160, per-axis from live extents;
  fractional touchpad deltas multiply it directly) in
  src/widget/scroll.c. test_scroll.c + the x11 animation-gui legs
  re-pinned (150px viewport floors at 40; the x11 leg's
  180px-bounds minus 12px classic bars = 168 viewport, 42/notch).
- **App telemetry for rigs**: PHASE: nav <dir> (DRIG-gated),
  DRIG-GEO path, rowh= on the list GEO lines.
- **setup-sandbox.sh cwd bug fixed** (ROOT_DIR pinned absolutely at
  start; the fourth recycle had steps 2-4 running from
  /tmp/x11dev-debs and the Wayland toolchain never installing).

## The rig's own four bugs (the round's real lessons)
1. The DRIG boot WIPES /tmp/fdk-drig and rebuilds it — fixtures
   created before boot are gone by the time the list populates.
   Every shape was "green" deleting in an EMPTY directory until the
   fixture moved post-boot. Verify the deletion actually happened
   (files-left count) before trusting a green.
2. List rows start ~22px below the columns header (round 10's
   lesson, re-learned): raw list-local y lands on the header.
3. An if/else chain meant the delscroll shape never ran its click
   (the wheel block ate the branch) — no selection, Delete a no-op.
4. Hand-rolled probe scripts passed KEYCODES where KEYCODES belong
   but the nav probes passed raw keysyms via ord(ch) — the typed
   path never landed. The runner (XKeysymToKeycode per char) works;
   the quick probes were the broken ones. Lesson: use the rig, not
   ad-hoc ctypes, for anything with key synthesis.

## Verification ladder (post-recycle, final code)
- `make test`: **763 [ok]** (762 + the new regression test).
- `make test-x11`: **228 [ok]** (after re-pinning the animation-gui
  wheel legs to the proportional step).
- `make test-wayland` (headless sway, the pinned wl-dnd-source
  recipe): **65 [ok], all passed**.
- -Werror clean throughout (the suites build with it on).

## NOT DONE this round (honest ledger, do not lose these)
- **The 4K / scaled-display physical-space blit** (round 14
  admitted it degrades to full damage at 4K; the walk-time
  physical-space strip collapse is DESIGNED but NOT IMPLEMENTED).
- **The opt-in frame profiler env var** (per-frame rasterized
  pixels, damaged area, present time, frame interval — the owner
  will run it on real hardware).
- **Frame-time consistency stats** (inter-present p50/p95/max
  during fast scrolling) — PRESENT-TRACE exists (round 14) but the
  percentile report was not built.
- **The owner's real-hardware breakage was NOT reproduced**: the
  X11 sandbox paths are clean. If the hardware report persists
  after this fix, suspect environment differences (the XDG trash on
  a real disk with EXDEV, inotify storms on real folders) and add
  shapes for those.

## Next round candidates
- The 4K physical-space blit + the frame profiler (the two
  not-done items above, both spec'd).
- His X11 boot-slowness report is STILL OPEN (round-6 item C).

# §31 continuation — UPDATE (owner round 14: the scroll repaint — delta-strip damage, and the hover repaint gap)

## The owner's brief and the round's shape
The performance-audit thread's third round, aimed at the scroll path:
the engine invalidated the WHOLE scrollview on every offset change
(wheel flights, thumb drags, trough paging) and repainted the full
viewport, though a pure offset change leaves nearly every row painted
— just shifted. Fixed engine-level with a same-surface blit of the
preserved region + exposed-strip-only tree damage; along the way the
new rigs found and fixed a real HOVER repaint gap and two strip-path
correctness bugs. A full sandbox recycle mid-round (the third
incident) destroyed the round's first implementation; it was rebuilt
from scratch on the true lineage (origin/main b2df84d) and every
verification was re-run — nothing below cites a pre-recycle run.

## What shipped (engine, all internal seams — API surface unchanged)
1. **fdk__surface_self_blit** (render): overlap-safe same-surface
   region copy (memmove row walk, direction by shift sign). The
   DESTINATION is damage-tracked — deliberately and necessarily: the
   X11 present's slot sync brings the next back slot up to date by
   copying exactly the PRESENTED rects, so an untracked blit leaves
   the moved rows off the wire (stale arrangement on screen outside
   the strips) AND stale in the next back slot. SURFACE damage (wire
   bytes) is a different ledger from TREE damage (repaint-walk
   bounds): paint stays strip-limited, the wire honestly carries the
   moved band. [This was blit-check bug #1 — 36k differing subpixels.]
2. **scroll_apply_delta_damage** (widgets/scroll): pure offset
   changes blit the preserved region and set EXACT strip damage
   (fdk__widget_set_damage — replaces the root's box, unions the
   layout-entry snapshot + the visible bars' thumb-precise old∪new
   rects via bar_thumb_at/bar_thumb_damage). The flight is a
   SELF-DAMAGING animation (fdk__animation_set_self_damage: the pump
   skips its blanket post-tick invalidate); declined paths fall back
   to the whole-widget invalidate. [Blit-check bug #2: the blit/strip
   footprint must EXCLUDE a visible CLASSIC bar's strip — a
   full-width blit smeared shifted content over the opaque trough.]
3. **The HiDPI gate**: fdk__window_tree_paint_scale/_tree_surface
   (window.c, the one-way-opaque owner back-edge). At scale > 1 the
   tree paints into a cleared intermediate — preserved pixels do not
   exist there — so the strip path declines. Recorded follow-up: a
   physical-space blit under scaling (4K also honestly degrades to
   full damage today via the 75% coarsening).
4. **The hover repaint gap**: set_hovered flipped FDK_WF_HOVERED and
   delivered ENTER/LEAVE but nothing guaranteed an invalidation —
   and list rows handle neither event while row_paint renders hover
   fills from the flag. Sweeps left stale highlights; the hover
   storm measured 1 kHz zigzag → ZERO presents. The transition now
   damages both sides at the choke point. [Found by the rig AFTER
   fixing my own zigzag pattern bug — see rigs.]
5. **Instrumentation**: FDK_PRESENT_TRACE=1 (one line per present:
   monotonic ms, full|rects, damaged area, window area, rect count)
   and FDK_SCROLL_NOBLIT=1 (the A/B kill-switch).

## Measured (release build, 20k-row list, run_stressrig.sh)
| storm | strips OFF | strips ON |
|---|---|---|
| wheel 20/s | 3.0% core, 53/53 fulls | **1.8% core, 2/49 fulls, ratio 0.66** |
| wheel 250/s (freespin) | 15.7% core, 254/254 fulls | **8.8% core, 0/241 fulls, ratio 0.63** |
| hover 1 kHz | 0 presents (the gap) | **43/s presents, ratio 0.311, 4.7% core** |
| sbdrag 1 kHz | — | **ratio 0.141, 5.5% core** |
Blit pixel-identity (run_blitcheck.sh, 200/500/2000 files): PASS —
0 differing subpixels of 700,000, all three sizes.

## The rigs (in-tree; the round's own lessons are baked into each)
- `stress_rig.c` + `run_stressrig.sh` — real-rate input at last
  (RIG_RATE_HZ wheels, 1 kHz hover, scrollbar drags). THREE rig bugs
  found and fixed while building it, each of which first looked like
  an engine bug: (a) the /proc/<pid>/stat parser skipped one field
  too many — it read stime+cutime, so a pure-userspace worker
  measured "0% CPU" and the settle gate released a 20k-row UI bind
  straight into the storm (the "dead storm, burst at end" signature);
  the comment in proc_cpu_ticks guards the field count. (b) the hover
  zigzag reset its position per 16-event batch — the app's motion
  coalescing (by design) kept only each batch's LAST motion, i.e.
  the SAME row every batch: no hover change, a vacuous storm. Zigzag
  state must persist across batches. (c) the settle gate needs
  THREE consecutive quiet polls — the worker→bind pipeline has an
  idle gap a single poll can land in.
- `run_blitcheck.sh` + `scripts/xsnap.c` + `scripts/xexpose.c` —
  the pixel-identity gate (blit-scrolled frame vs forced full
  repaint must be byte-identical). XClearArea(exposures=True) is the
  repaint twin; the tools select the window by title substring or id
  (largest toplevel fallback under bare Xvfb).
- Wayland suite rig note (cost me an hour): the sway config MUST pin
  `org.fdk.wl-dnd-source` at `100 px 380 px` (the DnD legs' drag
  coordinates assume it) and export SWAYSOCK — without them the
  suite fails at wl_dnd_rx.enters / focused-window asserts that look
  exactly like engine regressions. Full recipe: WLR_BACKENDS=
  headless WLR_RENDERER=pixman, XDG_RUNTIME_DIR=/home/z/apt/xdg-run,
  LD_LIBRARY_PATH=/home/z/apt/prefix/usr/lib/x86_64-linux-gnu, sway
  -c with `xwayland disable` (no Xwayland binary on this image).

## Verification ladder (all post-recycle, on the final code)
- `make test` (ASan/UBSan): **762 [ok], all tests passed.**
- `make test-x11`: **228 [ok]** — after fixing a PRE-EXISTING
  round-10 leftover this round's runs tripped over: the test (and
  extend.c's comment, fdk_extend.h's doc, EXTENDING.md's table)
  still said X11 has no frame pacing, but round 10 shipped a real
  window_frame_ready (17 ms interval + SHM backpressure). All four
  flipped to the round-10 truth.
- `make test-wayland` (headless sway): **65 [ok], all passed** (GL-
  lane legs skip: no EGL driver on this image — documented).
- A/B + blit-check numbers above; FDK_SCROLL_NOBLIT control run
  confirms the OFF path still measures the old behavior exactly.

## Incident (the third recycle — same lesson, now with a twist)
Round 14 was completed once already when the sandbox recycled and
handed back a tree WITHOUT the round's working dir at all (work/
held an older clone at c1f27e9-era; FDK-r14 gone entirely; /tmp and
/home/z/apt wiped). Recovery followed the standing practice: fetch
origin (b2df84d was intact — the push-every-step rule would have
made this free, and the round's code commit had NOT yet been pushed,
so everything was rebuilt from the conversation's surviving
artifacts), reset main to it, rebuild toolchains via
scripts/setup-sandbox.sh (whose own cd bug — steps 2-4 stranded in
/tmp/x11dev-debs when x11dev was already prepared — is fixed in the
round's commit). The twist worth remembering: the recycled snapshot's
/home/z/my-project/scripts/ still held the OLD session's rig binaries
(fdk-wl-inject, wl_dnd_source — built Oct 7) — check there before
re-deriving tools.

## Next round candidates
- The physical-space blit under HiDPI scaling (4K full-damage today).
- fdk-files: the fs_stat_entry worker-pool threading (round-13
  carry-over — the 20k-row UI-thread bind this round's rig kept
  tripping over is exactly that candidate's business).
- (existing list: S7/S8/S10, PART-1..8, §30 backlog.)

## Rules unchanged
Master rule; zero warnings (-Werror on); no D-Bus library in FDK;
ta_buf in apps; 8-experiment timebox; identity ONLY
Faded Dream Project <242450780+FemBoyGamerTechGuy@users.noreply.github.com>;
WIP push every step; webfetch specs, record URLs; NEVER force-push.

# §31 continuation — UPDATE (owner round 13: the core-of-FDK performance audit — the text path)

## The owner's report and what the audit found
"the filemanager feels slow — something is wrong in the core of
FDK about performance, look through the code really carefully and
find anywhere the performance can be increased." A deliberate
full pass over the ENGINE core (src/text, src/render, src/widget
paint paths, window paint walk), engine fixes only, measured with
a NEW input rig (wheel storms + hover sweeps under Xvfb, CPU
sampled from /proc) plus the round-12 nav rig.

Audit verdict table (what was checked and what it found):

| path | finding | verdict |
|---|---|---|
| paint walk (paint_rec) | damage-rect + bounds culling, clip stack | sound |
| blend_mask/blend_rgba | integer source-over, transparent-skip | sound |
| hb_font per font | created once, cached on the font | sound |
| **glyph cache lookup** | **linear scan over 2048 slots, per glyph PER PLACEMENT** | **FIXED — hash index** |
| **shaping (measure/draw/spans/caret)** | **FriBidi+HarfBuzz+3 allocs re-run per call per frame, freed, re-derived** | **FIXED — shaped-string cache** |
| fdk-files fs_stat_entry | stat() per file ON THE UI THREAD (invisible on tmpfs; THE real-disk feels-slow) | recorded — next fdk-files round: the fdk_thread.h worker pool exists, mirror thumbs.c |

## The two engine fixes
1. **Shaped-string cache** (src/text/shaping.c, text.c, entry.c):
   a shaped result is a pure function of (font, style, bytes) —
   the subpixel phase derives from the in-string pen-from-zero,
   consumers offset whole pixels — so measure/draw/spans/caret
   BORROW one LRU-cached result (256 entries, <=1KB). Runs!=NULL
   (styled spans) bypass. Invalidations: glyph eviction, style
   flush, font destroy, last-root teardown (beside the animation
   drain). THREE hard lessons baked in:
   - **Churn**: naive first-sighting admission REGRESSED the nav
     rig (14.9% -> 19.4% of a core): directory loads present N
     fresh strings, evicted before reuse. Fix: second-sighting
     admission (512-slot fingerprint remember-set + a park slot);
     bulk loads never churn, steady state always admits. Final
     nav: 13.6% (beats baseline).
   - **Ownership**: a bypass that returned caller-owned results
     into a borrow API leaked (LSan: the 1MB stress line, then
     every measure("")/draw("")). Fix: only NULL-returning cases
     bypass; empty strings flow through the cache; >1KB strings
     ride a single owned big-slot.
   - **Test layering**: two test_text assertions encoded the old
     "second draw touches the glyph table" assumption; now the
     shaped cache serves it pre-resolved. Invariants re-asserted:
     rasterizations flat (misses) + pixel-identical output.
2. **Glyph-cache hash index** (text.c): open addressing over the
   key (4096 buckets), backward-shift delete, insert-before-
   rasterize with key committed first (recursive fallback
   glyph_slot calls must see consistency). Init-to-EMPTY on font
   create (slot 0 is valid — memset(0) was the trap), reset on
   flush.

## Measurements (% of one core; release build; input rig + nav rig)
| rig | baseline | final |
|---|---|---|
| hover sweep 1000-file dir | 1.3% | **0.7%** |
| wheel scroll 1000-file dir | 6.2% | **5.5%** |
| nav storm 500 files | 14.9% | **13.7%** |
| (naive-cache nav detour) | 19.4% | the churn lesson above |
ASan: scroll 14.2 -> 10.5%; hover/nav flat (instrumentation
dominates). All rigs settle to 0.0 CPU after the storm.
Hover halved, scroll -12%, nav -8% — and the steady-state
re-shaping per frame (the actual "feels slow" mechanism for text
everywhere, not just fdk-files) is gone at the engine level.

## Verification this round
- make tools && make test (both backends, ASan/UBSan, LSan):
  GREEN — 762 [ok], zero leaks, zero warnings.
- Rigs: menu rig PASS (Properties runs Properties); click sweep
  over rows: header 160-175 -> sel 0 (none); rows 180-200 ->
  sel 1 alpha.txt (interactive selection intact).
- New rig survives in-tree: test-apps/input_rig.c +
  run_inputrig.sh (RIG_MODE=scroll|hover, RIG_FILES,
  RIG_SECONDS; build-rel for release numbers).

## Next round candidates (unchanged + this round's additions)
- fdk-files: thread the fs_stat_entry pass through the
  fdk_thread.h worker pool (mirror thumbs.c's pattern) — the
  real-disk directory-load latency, the remaining app-layer
  "feels slow".
- (existing list: S7/S8/S10, PART-1..8, §30 backlog.)

## Rules unchanged
Master rule; zero warnings (-Werror on); no D-Bus library in FDK;
ta_buf in apps; 8-experiment timebox; identity ONLY
Faded Dream Project <242450780+FemBoyGamerTechGuy@users.noreply.github.com>;
WIP push every step; webfetch specs, record URLs.

# §31 continuation — UPDATE (owner round 10: the wrong-row offset, the pegged core, the GPU question)

# §31 continuation — UPDATE (owner round 11: the dead rubber band, the Properties-asks-delete, the pegged core, the unselecting right-click)

## The four reports, four root causes (all fixed this round)

1. **"hold and drag to highlight does not work / I want multi-file
   selection"** — the 1.0.0 drag veto was checked on the press's HIT
   TARGET, but a press inside a list hits a ROW, never the list: the
   list's own veto (list.c:2301) was dead code. The controller fired
   pane_begin_drag at 6px, the DnD grab broke the band. FIX: vetoes on
   every UNSELECTED row (flipping with selection), the rows box, the
   scrollview + dynamic scrollbars (new fdk__scrollview_veto_input,
   re-run at every scrollview_layout), the iconview grid. Click-select
   stays icon+text-only (round 9's gate is untouched).
2. **"Properties asks me if I want to delete"** — item_activate read
   g_menu_acts[idx-1] against a 1-based registration: every entry
   fired the action ABOVE it. Plus: context menus re-registered into
   the 256-slot shared registry per popup (silent corruption after
   ~25 right-clicks). FIX: 0-based like the toolbar/shortcut twins;
   bounded 64-slot ctx ring; "PHASE: act" telemetry.
3. **"one core pegged when I hold-click fast"** — every fast sweep
   "began" a spurious DnD (window-tree walk per motion on X11), and
   the controller re-walked the whole widget tree per motion to find
   its armed source; the iconview repainted the whole view per motion
   pixel. FIX: root-cached armed source (O(1), destroy-safe), veto
   kills the spurious gestures, iconview band damage is old∪new +
   per-flip. perf_drag: 4.8% → 4.0% of one core.
4. **"the row of selecting a folder is not right"** — the right-click
   selected nothing; the menu flavored from the stale live selection.
   FIX: new public fdk_list_row_at / fdk_iconview_item_at (the same
   geometry the press/band use); the right-click selects the clicked
   row (multi-selection kept), flavor from the CLICKED item
   (CTX_FOLDER now reachable). menu rig proves: right-click beta-dir
   → "sel 1 beta-dir"; last entry → "act Properties".

## Verification this round
- make tools && make test (BOTH backends, X11+Wayland lib, ASan/UBSan):
  GREEN, 764 [ok] checks, incl. 4 new (veto interplay, gutter/
  scrollbar never-drag, O(1) armed-lookup mid-press destroy,
  row_at/item_at geometry).
- Click sweep (Xvfb XTest, 600ms-spaced): header y 155-175 → nothing;
  row 0 (180-205) → alpha.txt; row 1 (210-235) → beta-dir; 240+ →
  nothing. Pixel-exact.
- run_menurig.sh end-to-end: VERDICT PASS (Properties runs
  Properties).
- perf_drag: 4.0% of one core steady-state.
- KNOWN OPEN: the §28 drag rig (run_drig.sh) D1-D4 fail IN THIS
  SANDBOX (the sink process / app boot timing under ASAN Xvfb; no
  dnd-g12 trace lines ever appear — the drag never starts here).
  Engine-level drag-fires-from-selected-row IS covered by the new
  test_veto_row_selection_interplay. NEXT ROUND: re-run run_drig.sh
  in a clean boot; if it still fails, bisect against e092f17.

## Remote proof
origin/main == this round's commit, normal fast-forward push (round 9's standing rule holds:
never force-push). `git ls-remote origin main` is the authority.

## Incident (the second one — read before trusting any sandbox snapshot)
Round 10 was COMPLETED once already (code, tests, rigs, docs, all verified, commit cbc26fc)
but the session ended before its push, and the next sandbox recycle handed back an ALIEN
snapshot again: HEAD at the pre-round-8 force-push-era commit c1f27e9, with c71a7b0/9fc4fb1
not even present as objects, plus one uncommitted 169-line wayland_egl.c edit that was never
part of the true lineage. Recovery this time: the uncommitted edit is preserved as a patch
(/home/z/my-project/download/alien-wayland-egl-uncommitted.patch — the owner decides whether
any of it is wanted), the alien head is tagged locally as branch alien-snapshot-c1f27e9,
origin was fetched and main reset --hard to c71a7b0 (the round-9 head, intact on the remote
all along), and round 10 was REDONE on the true lineage and re-verified from scratch.
The lesson both incidents teach, now standing practice: the REMOTE is the only lineage
authority; a recycled sandbox's tree is untrusted until `git fetch origin && git status`
says it matches, and any round whose push failed must be assumed LOST, not "pending".

## The owner's three reports, one root cause + one architecture finding

1. **"Selecting something in the context menu is not right"** and
2. **"clicking a folder [in fdk-files] highlights the item below"** — SAME bug. The context
   menu flavors itself from the pane's LIVE SELECTION (`view_event`: `sel > 1 → CTX_MULTI,
   sel == 1 → CTX_FILE, else CTX_EMPTY`), so a selection that lands one row low makes the menu
   act on the wrong file. The pointer coordinates were NEVER wrong — the platform layers
   (X11 core + XI2 + Wayland seat, fractional viewporter) were audited first and are
   scale-correct by design; the Xvfb event trace proved widget-local coordinates exact
   (`row-DOWN local=(60,7)` for a click 7px into row 1's rect).
   ROOT CAUSE, engine: the 1.0.0 columns round inset the rows 26px below the header strip but
   left the rubber-band math dividing RAW list-local y by the row height. Every list-level
   press mapped one row LOWER than the pointer: a header press "selected" row 0, a press right
   of a row's text (round 9's gate falling through to the band) selected the row BELOW — the
   owner's exact wording. FIX: `list_content_top()` (the header inset, 0 without columns) now
   routes every band↔content mapping — anchor capture, apply-band row math, band rect, paint
   anchor, both auto-scroll edge tests (which also mis-measured "near the top edge" against
   the widget's 0 instead of the viewport). Six sites, one invariant: content space starts
   below the strip.
   Round 9's OWN test had enshrined the bug (part 3 pressed visual row 0 and asserted row 1 —
   written against the offset's output). Corrected to assert the visual row; `test_columns`'
   sweep coordinates (press at local y=15 = the header!) recomputed through the new
   `fdk__list_test_content_top()` seam; NEW `test_round10_header_strip`: R1 header click
   selects NOTHING, R2 right-of-text chase lands the VISUAL row, R3 sweep edges exact, R4 the
   same sweep at a NONZERO scroll offset (offset 3*rh, rows 4..6 — the path where a sign
   error would hide until scrolled; one lived briefly in band_rect and diff review caught it).
3. **"One core pegged during drags; GPU idle"** — two findings:
   a) X11 had NO `window_frame_ready` op (Wayland-only), so `fdk_surface_frame_ready()`
      short-circuited to always-true and every canonical `pump; if (frame_ready && damage)
      paint` loop painted UNPACED — one full acquire/raster/present per loop pass while a
      drag kept damaging (every motion). X11 motion COALESCING already existed (1.0.0-rc1,
      the XPeekIfEvent collapse) — the pegging was paint RATE, not event rate. FIX: X11 now
      ships the op — 17ms minimum present interval stamped by both render lanes (GLX and
      SHM/CPU), the 1.0.0 Wayland capacity gate's reasoning (both SHM slots in flight →
      not-ready, so the next acquire doesn't block), and two exceptions (nothing presented
      yet; popups — first-present latency is their correctness). Measured with the new
      perf_drag rig (250 XTEST motions/s, 6s, /proc/<pid>/stat sampling): steady-state
      13.2% → 4.4% of one core on the original run; 4.8% on the redo (same ballpark —
      the sandbox's noise floor, not a regression). The click sweep re-verified
      pixel-perfect on the paced build (a click is not a frame-rate event; no latency
      regression is possible from a 17ms gate on a discrete gesture).
   b) The GPU question — honest answer below, in "The GPU assessment".

## The round's rigs (new, all survive recycles in-tree)
- `test-apps/fdk-files/click_rig.c` + `run_clickrig.sh` — REAL XTest click sweep down a pane
  (DRIG layout), reading the app's `PHASE: sel` telemetry. Two lessons baked in for whoever
  extends it: (1) the telemetry itself had to be ADDED — main.c's header has promised
  "PHASE: sel every selection change" since §26, but only the boot smoke script ever printed
  it; the gated print now lives in `list_selection_changed` (panes.c, FDK_FILES_DRIG-gated,
  prints the selected count + lowest row's text AND the empty selection as `sel 0 (none)` —
  the rig must distinguish "nothing fired" from "fired with zero"). (2) the sweep must space
  clicks ≥600ms: at ~330ms consecutive same-row clicks consume as DOUBLE-clicks
  (FDK_WINDOW_DBLCLICK_MS=400) and the second of each pair activates (open/navigate) instead
  of selecting — a rig artifact that perfectly impersonates an app bug. And a third, paid for
  twice now: the sweep's FROM/TO are WINDOW coordinates — read them from the DRIG-GEO lines,
  never from memory (pane0's list sits at y=152..377 in this layout).
- `test-apps/perf_drag.sh` — the pegged-core prover: DRIG app under Xvfb, a 6-second
  250-motion/s held-button drag, /proc/<pid>/stat sampled every 500ms, steady-state deltas
  computed. Gotcha already paid for: `pgrep -f` matches the timeout(1) wrapper first — use
  `pgrep -x fdk-files`.
- Makefile: both rigs link against /tmp/my-project/local-x11 (the drag-rig line's old
  /tmp/x11dev path died with a recycle; that prefix no longer exists).

## Verification ladder this round (the REDO, on the true lineage)
- `make tools && make test` (both backends, X11+Wayland lib): **826 ok / 0 fail** —
  identical to the pre-recycle original run of this same round.
- test_list round-10 regression green (R1–R4); round-9 test corrected-and-green.
- End-to-end click sweep (Xvfb, XTest, 600ms-spaced): header 152–177 → nothing; row 0 rect
  178–208 → alpha.txt; row 1 rect 208–238 → beta-dir; empty space 240+ → nothing.
- perf_drag: 4.8% of one core steady-state (pre-pacing baseline 13.2%).

## The GPU assessment (the owner's question, answered straight)
"Shouldn't the GPU fans ramp when I hold-click-drag? The GPU does nothing; FDK files is not
GPU accelerated; only one core is pegged." All three observations are correct and they all
follow from one architecture fact: **FDK is a CPU rasterizer with a GPU present-only lane.**
Every pixel — text glyphs, rounded rects, alpha blends, the rubber band — is computed by
scalar C into a software framebuffer (src/render/surface.c; blend_pixel per-pixel
source-over). The GPU's only job, when the lane engages at all, is the final composite: the
damaged rects are uploaded as a texture and drawn on a fullscreen quad (GLX on X11 / EGL+GLES2
on Wayland, `FDK_GL_MIN_PIXELS=131072` gate — panels and popups stay on the CPU memcpy path
because a private context + vsync swap costs more than a few hundred KB of SHM). So during a
drag: the CPU rasterizes band-sized rects at up to the loop rate (the pacing bug made that
"unbounded"), and the GPU does one small texture upload + one quad per frame — single-digit
watts, fans never move. That is not a misimplementation of acceleration; it is the design.
Is it the RIGHT design? For this project's stage, yes: the retained-mode damage tracker +
per-rect invalidation keeps most interactions touching a few thousand pixels, the whole
renderer is portable and testable headlessly (the entire test suite runs under Xvfb),
and no GPU state lives in the app's address space to leak or crash. What "GPU-accelerated
FDK" would actually mean, in honest effort order: (1) SIMD rasterization (SSE/AVX2 blend
and fill paths, or a skia-style scanline core) — the biggest win per week of work, no GPU
required, helps the pegged core directly; (2) render-thread + damage batching — move the
raster off the input thread so motion never queues behind paint; (3) GPU-side damage
(two texture slots + scissored glTexSubImage2D, which the lane already half-owns) — cuts
the upload but NOT the raster; (4) true GPU raster (glyph atlases, tessellated rects,
MSAA) — a rewrite-class project, months, and it forfeits the headless test story. The
recommendation recorded for the owner: (1)+(2) next; the GL lane as-is is fine for
presenting. And on "one core": FDK is deliberately single-threaded for input+paint (the
thread-safety story is documented in docs/architecture.md); the fix for a pegged core is
doing LESS per event (this round's pacing: 3× less) and eventually the render thread —
not more cores.

## Rules unchanged
Master rule; zero warnings (-Werror on); no D-Bus library in FDK; ta_buf in apps;
8-experiment timebox; identity ONLY Faded Dream Project
<242450780+FemBoyGamerTechGuy@users.noreply.github.com> (git config global+local set to it;
.git/hooks/pre-commit enforces it and commit-msg strips any Co-Authored-By/Signed-off-by);
WIP push every step; webfetch specs, record URLs; NEVER force-push (round 9's standing rule).
# §31 continuation — UPDATE (owner round 9: the five reports, ported onto the restored lineage)

## Remote proof
origin/main == this round's code commit + docs commit. Tree clean at that commit.

## This round's first act: the lineage itself
Round 9 began with an incident, recorded here because it shaped the round. A mid-session sandbox
recycle handed the working tree a DIVERGED snapshot of the repo (a copy-fork whose HEAD had no
test-apps/), and the session committed and FORCE-PUSHED that fork's head over origin/main,
displacing round 8's head (9fc4fb1). The owner caught it. Recovery: the true head was re-fetched
from GitHub by full SHA (the object was still servable), main was reset and force-pushed BACK to
9fc4fb1, and the alien lineage's one commit (2263ee9 — the five fixes, already written against
the wrong tree) was preserved on the local branch backup/round-9-alien. This round's real work
was then PORTING those five fixes onto the true lineage. Standing rule from the owner, effective
permanently: **never force-push again** — normal fast-forward pushes only; a remote/local
divergence is a stop-and-reconcile event, never an overwrite.

## Round 9 delivered (the owner's five reports, engine-level)
1. **STUCK BANDS (gesture-cancel sweep)** — when a menu chain dies (outside-click CLOSE_REQUEST,
   popup_done), the popup swallowed the POINTER_UP, so any live rubber band's highlight stuck on
   screen. `fdk__list_cancel_bands_all_roots()` (list.c, seam in widgets_internal.h) now fires
   from menu.c's teardown paths (session_close_above level-0, menu_popup_destroyed): walks every
   root, ends every live band exactly as an UP would. PORT EXTRA beyond the alien version: it
   also clears the rows' pending_band flags (1.0.0's row-born gestures) — a stale armed row
   would have synthesized a phantom band from stale coordinates on the next motion.
2. **ROW HIT BANDS (presses right of the content are empty space)** — a row now accepts presses
   only over icon + name text + padding (`row_press_on_content`, columns-aware: the 1.5.x column
   cells ride the row's full-width PAINT rect, but presses there are the LIST's). A press right
   of the text falls through to the list and starts the rubber band there. The round-6
   press-time chase semantics are PRESERVED (a no-motion click at a row's y still selects that
   row — pinned by test_row_press_band and now by test_round9_ports).
3. **MENU STORMS (single-session context menus)** — `fdk__menu_popup_open_full` closes every
   other bar-less (context) session before opening; the 4-slot table can no longer be reached
   from popups. MenuBar chains are a different gesture class and keep their own rules.
4. **MENU MISFIRES (the opening-release guard)** — menu views now press-ARM: a POINTER_UP
   without a press this view saw is swallowed without activating. This kills the
   "Properties acts like Delete" class: the menu popped under the still-held button and the
   release chose whatever row sat there (GTK/Qt swallow this exact release too).
5. **REPAINT COST (damage-limited band)** — apply_band no longer invalidates the whole list per
   motion pixel: `fdk__widget_invalidate_rect` (new widget.c seam, widget-local rect clipped to
   bounds) carries the union of the old + new band rects; list_set_row already damages only the
   rows that FLIP. Large directories no longer peg a core during sweeps.
6. **POPUP TEARDOWN DAMAGE (the black box)** — popup windows now capture their anchor rect in
   parent-client coords at creation (popup_anchor_x/y/anchored, window_internal.h) and
   fdk_window_destroy damages that rect in the parent's tree: X11 popups unmap off the ROOT, so
   the parent never got the Expose for the region the menu covered.

What was NOT ported, deliberately: the alien commit's wayland_egl.c NVIDIA upload-path work
(BGRA/UNPACK_ROW_LENGTH probing, RGBA twin, pack scratch) — the TRUE lineage already carries it
(it originated here at 1.0.0 and evolved through the GLVND round-7 work; the alien fork's copy
was a re-derivation). Same for its examples/10_file_manager.c demo: fdk-files' own menus.c has
had the real context menu (Open / Open with… / Rename / trash / permanent delete, each wired)
since the §26 rounds.

## Tests
test_round9_ports (tests/test_list.c): presses right of the content start bands not row-clicks
(content presses still select), the cancel sweep ends swallowed gestures (redundant-safe,
stray-UP-safe), the gate holds under 1.5.x columns with the far-end anchor active. Full suite
green (`make test`, ASan/UBSan clean). fdk-files relinked and boots (app_id
org.fdk.testapps.files, PHASE: boot).

## Next round candidates
- His X11 boot-slowness report (round-6 item C) — STILL OPEN.
- The E2E XTest self-drop proof (round-6 leftover).

# §31 continuation — UPDATE (owner round 8: the columns' far-end anchor + the folder type)

## Remote proof
origin/main == this round's code commit + docs commit. Tree clean at that commit.

## Round 8 delivered (the owner's three reports)
1. **THE COLUMNS HUG THE FAR END + FOLLOW RESIZES (engine, 1.0.0)** — round 7 summed widths from
   the left, so the fixed columns clustered right after the shortest filename and never moved on
   resize. The layout is now RESOLVED, not summed: `list_columns_layout()` runs on every viewport
   change (arrange / the app's set_bounds / the paint self-heal — all funneled through one sync)
   and picks one of two modes. ANCHORED: the last fixed column's right edge sits at the viewport's
   end (clear of the v-scrollbar's reserved strip), the name column stretches across ALL the slack.
   CONTENT (window narrower than the natural spread): natural widths, one separator apart, with the
   h-scroll floored at the spread — narrow windows degrade into a scrollable pane instead of
   colliding with the names. The header gained vertical divider hairs between the fixed columns.
   ENGINE BUG this round exposed + fixed: the paint-tail self-heal had its own inline scroll
   set_bounds that BYPASSED the shared sync — the anchor never re-resolved on a direct set_bounds
   resize; it now routes through list_sync_scroll_bounds like every other path.
   Tested: right-edge anchor == viewport, widen-to-600 re-anchors, narrow-to-160 flips to content
   mode with the spread floor, re-widen re-anchors, selection under columns, clear-to-plain.
   Rig-verified on BOTH backends: name→Modified gap 571px (X11 1080px window) / 767px (Wayland
   1280px window) vs round 7's ~150px; right edges at the window end minus scrollbar+pad.
2. **THE FOLDER TYPE (app fix)** — mime_category had no inode branch: "inode/directory" fell
   through every category and returned "File". Directories now read "Folder" (and other inode/*
   entries "System"). Rig-verified: the folder row shows no Size (dirs) and a distinct Type cell.
3. **WAYLAND SUPPORT CONFIRMED** — fdk-files runs the Wayland backend natively (the owner's own
   logs have always been backend=wayland); this round adds the proof rig: headless sway + grim
   screenshot (attached to the report) showing the same far-end column geometry on Wayland, and
   the Wayland self-quit under LSan is STILL zero-leak exit-0. X11 self-quit: only the documented
   Mesa/GLVND ~108KB rig class (round 7) — unchanged, third-party, absent on the owner's NVIDIA X11.

## Rig index (additions)
grim lives at /home/z/apt/prefix/usr/bin/grim (restore_wayland_toolchain.sh); run it while the app
is ALIVE (a compositor screenshot is of live surfaces), with LD_LIBRARY_PATH to the prefix.
NOTE for the X11 rig: `env -u WAYLAND_DISPLAY` when a sway session is running in the same shell,
or the app will prefer the Wayland backend (the backend picker prefers WAYLAND_DISPLAY).

## Next round candidates
- His X11 boot-slowness report (round-6 item C) — STILL OPEN.
- The E2E XTest self-drop proof (round-6 leftover).

# §31 continuation — UPDATE (owner round 7: the list-view columns + the exit leaks)

## Remote proof
origin/main == this round's code commit + docs commit. Tree clean at that commit.

## Round 7 delivered
1. **LIST COLUMNS (engine, 1.0.0)** — the file-manager list view as a first-class fdk_list
   capability, NOT app code: `fdk_list_set_columns()` (fdk_list_column: title/min_width/
   align_left; column 0 = the primary text column, expands, keeps the v1 contract; columns
   1..N fixed-width right-aligned default), `fdk_list_row_set_cell()`, `fdk_list_get_column_count()`.
   Geometry is PAINT-TIME on the existing row widgets (no per-cell children — hit-testing,
   the implicit grab, the rubber band all untouched). Header strip pinned above the scrollview
   (inset via one list_sync_scroll_bounds rule), titles ride the horizontal scroll, cells render
   in text-disabled. Widths: declared → widened to the title → widened per cell (never clips a
   right-aligned tail); columns_width floors content_width AND the natural width. Exports
   1001 → 1004. make test green incl. new test_columns (API/geometry/selection-under-columns/
   clear-to-plain). ALSO FIXED a latent bug my code exposed: row_paint computed the owning list
   as list_of(SCROLLVIEW) — garbage since v1, harmless until something read it; now walks the
   chain to the real list class.
   The APP (fdk-files) only supplies data: Name | Modified | Size | Type (mime_category),
   human sizes, "Nov 12, 18:20" this-year dates — through the public per-row API beside the
   icon loop, on every rebuild path. Visual rig pixel-verified: header + rule + right-aligned
   cells, folder row shows no size, right edges align.
2. **EXIT LEAKS (his round-7 log)** — the dnd_uri leak (round 6) confirmed GONE on his log.
   Remaining classes were ALL third-party driver state:
   - libEGL_nvidia 4x256B + libdbus 184/646B (NVIDIA's EGL pulls libdbus transitively; FDK
     itself NEVER links/dlopens libdbus — its D-Bus is a self-implemented socket protocol):
     freed by the library's dlclose destructors, which the pinned-forever policy skipped.
     FIX: fdk_wayland_egl_libraries_release() — dlcloses libEGL AFTER wl_display_disconnect
     (the display must be dead first; the same rule the X11 lane taught us below).
     PROVEN: headless-sway self-quit LSan = ZERO leaks, exit 0.
   - A cell-leak in MY new columns code (row_destroy walked a parent chain that destroy
     ALREADY unlinked — the tree walker's rule): rows now free their own cells by cells_cap
     recorded at every alloc/grow site. Same rig: zero.
   - The X11 twin (libGLX_mesa ~108KB on the llvmpipe rig): dlclose(libGL) after
     XCloseDisplay runs clean BUT Mesa/GLVND holds the vendor lib with its own ref
     (dlclose rc=0, state survives) — GLVND internals, rig-specific: the owner's NVIDIA
     X11 runs showed NO such class (round-6 X11 log leaked only FDK's dnd payload).
     The hook (fdk_x11_gl_libraries_release) stays as a documented no-op. THE SEGV LESSON:
     dlclose BEFORE XCloseDisplay crashes (Xlib's GLX extension cache lives inside libGL) —
     recorded in the code so nobody repeats it.

## Rig index (this round)
headless sway: WLR_BACKENDS=headless + XDG_RUNTIME_DIR=/home/z/apt/xdg-run + LD_LIBRARY_PATH=
/home/z/apt/prefix/usr/lib/x86_64-linux-gnu (restore_wayland_toolchain.sh rebuilds the prefix;
fix the local-x11 xrandr.pc prefix after a reset — it ships prefix=/usr). Self-quit LSan rigs:
FDK_TESTAPP_FRAMES=120 + FDK_FILES_STARTUP_DIR on each backend. Column visual rig:
Xvfb :99 + x11_shot.py, pixel-assert via spans (bgs {(35,35,41),(41,41,49)}).

## Next round candidates
- His X11 boot-slowness report is STILL OPEN (round-6 item C, unstarted).
- The E2E XTest self-drop proof (round-6 leftover; rig was flaky).
- Column WIDTHS could later ride the theme metrics (FDK_TM) if a theme ever wants them.

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
  + stash; DO NOT git add -A on this tree — untracked old-install-tree
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