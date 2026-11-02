# §25 continuation state (written at round cut-off)

## Remote proof
origin/main == HEAD == 777492078d43664e8239b79320b35fbfb0fd66df (verified).
Round commits: 4a73ca3 (zero-warning restore) · 0780782 (ta_buf) ·
4d06933 (G-2 threads) · b68236 (G-3 selection) · 48d9780 (4a teardown) ·
7c7e42e (4b worker engine) · 7774920 (G-4+G-5+thumbs) · + the final
CONTINUATION commit below.

## DONE and VERIFIED (this round)
- STEP 1: env re-setup (see scripts/s25-env.sh; dev pkgs: xrandr/Xtst/Xi
  debs into /tmp/x11dev, wayland via scripts/restore_wayland_toolchain.sh).
  make test / test-x11 / verify-exports / test-apps smoke ALL GREEN.
- BUDGET RULE: test-apps/common/ta_buf.h (ta_copy/ta_join/ta_join_suf/
  ta_fmt + ta_truncated flag), verified under the full strict flag set.
- STEP 2 / G-2 (fdk_thread.h + src/core/thread.c): fdk_post (eventfd/pipe
  wake wired INTO the pump's poll), worker pool + jobs + cooperative
  cancel, FDK_ERR_WRONG_THREAD refusals under FDK_DEBUG_THREAD_CHECKS
  (fdk_window_create + fdk_timer_add guarded), docs/threading.md
  rewritten, tests/test_threads.c (stress/ordering/wakeup/roundtrip/
  cancel/shutdown/wrong-thread) green under ASan (in test-x11) AND TSan
  (make test-threads-tsan). TSan found one race — in the TEST (volatile
  cancel flag), fixed with atomics.
- STEP 3 / G-3: fdk_list_deselect, fdk_list_select_range (order-agnostic,
  clamped, additive), fdk_list_set_on_selection_changed_bounds (the span
  signal). tests/test_list.c::test_selection_completeness green.
- STEP 4a: teardown.[ch] registry; fdk-files --smoke LSan-clean rc=0.
- STEP 4b: ops.[ch] — copy/move/delete/recursive-size on the worker
  engine; progress dialog is an observer; rig round-trips
  (PHASE: workercopy 1). Found+fixed: done_msg needed calloc (garbage
  size_fn SEGV).
- STEP 4c: G-4 fdk_image_decode_pixels (any-thread RGBA decode) +
  G-5 fdk_iconview_set_item_pixmap (public face of the borrowed-image
  slot), both test-first; fdk-files thumbs.[ch] (LRU + XDG cache +
  thread-legal worker pipeline: pixels on worker, surface on UI);
  wired into the icons view.

## §26 progress (this round)
DONE+PUSHED: 1cefea0 Wayland verification (rig PASS 65; G-2 ASan+TSan
on wl 0.1-0.3ms wake; fdk-files 3/3 zero-leak; probe fix) · d995c7a
split view + bookmarks + G-6 (model-binding detach on widget destroy;
UAF found live, test-first) · 86b5965 .desktop + FDK-drawn icon +
tray-bus rig (T1/T2 PASS; T3 should-work — see below). The 50k gate
MEASURED (perf_50k.c: open 105ms, fill 102ms batched, scroll avg
2.55ms worst 76.92ms, RSS 541MB under ASan); screenshots in
test-apps/screenshots/ (main, dark, smoke-split); fdk-files README
with the §24 checklist verdicts. fdk-files PASSES its own checklist
per README → fdk-edit (STEP 4) may start next round.
KNOWN ISSUE to chase (fdk-files T3, recorded in run_files_tray_bus.sh
comments): the notify client's LAZY connect path fails in the
fresh-app-after-daemon order ("session bus connect failed",
src/desktop/notify_client.c on_ready) — app boots fine BEFORE the
daemon appears. Reproduce: dbus-daemon + notifyd first, then fdk-files
--smoke. Likely a first-send-vs-async-connect race in the client.

## §27 status (this round, pushed)
PUSHED: f57cd7c — STEP 2a: G-8 (serialized per-address D-Bus
handshakes; tests/test_dbus_serialize.c green: five no-pump connects
all reach ready) + G-7 (extfd dispatch bounded by the filled count;
tests/test_extfd_race.c). The notification CLIENT path is verified
end-to-end (tray-bus rig run-1: notify → daemon → card placed on
notifyd). STILL OPEN, with evidence: the fdk-files --smoke boot gets
its first notify connection closed by dbus-daemon in some runs even
serialized — wire trace via scripts/busproxy.py shows the connection
auths OK then dies as the module burst opens; the exact daemon-side
trigger is NOT pinned. Debugging aids built: the byte-proxy, the
DRIG deterministic layout (below). NEXT ROUND: the strongest untested
hypothesis is the model/list binding: with the DRIG gate, a pane
whose dir HAS files shows rows=0 in a freshly bound list (fmodel_load
returns 2; the bound list shows 0) — chase THAT in model_bind's
initial-mirror path with two bound lists on the singleton model (the
second bind's set_on_change overwrites the first's handler — the
single-slot design in src/widget/model_bind.c:183 is suspect).

STEP 2b (drag rig) BUILT but NOT GREEN — committed as WIP: the XTEST
driver (fdk-files/drag_rig.c + run_drig.sh + the FDK_FILES_DRIG=1
gate in main.c: split view, deterministic /tmp/fdk-drig layout,
1000x700 window; Xvfb must run -dpi 96 or FDK's X11 scale detection
divides the rig's coordinates). The blocker is the same rows=0
binding question above (the rig's clicks find no rows to select, so
no drag arms). NOT VERIFIED: D1 (drag-between-panes), D2
(drag-to-bookmark), drag-reorder. The Wayland leg of the drag rig is
NOT started (the repo's wl injector exists: scripts/fdk-wl-inject).

All suites green at cut-off: make test, test-dbus ALL PASS, test-x11
3/3, verify-exports OK, Wayland suite PASS, fdk-files smoke clean.

## §27 FINAL STATUS (round cut off at STEP 2b; read this first)
DONE + PUSHED:
- STEP 1 baseline: green on X11 AND Wayland at 56c3072 (start).
- STEP 2a → f57cd7c: G-8 serialized per-address D-Bus handshakes
  (tests/test_dbus_serialize.c: 5 no-pump connects all ready) +
  G-7 extfd dispatch hardening (tests/test_extfd_race.c). The notify
  CLIENT path verified end-to-end (tray-bus run-1: card placed on
  notifyd). OPEN: the --smoke boot's first notify connection still
  gets closed by dbus-daemon in some runs — evidence + the
  model_bind rows=0 lead in the §27 status block above.
- STEP 2b rig → 5331da2: drag_rig.c + run_drig.sh + FDK_FILES_DRIG
  gate committed WIP; D1/D2 NOT VERIFIED (blocked by the same
  rows=0 binding question — THE next chase: a freshly bound list on
  the shared singleton model shows 0 rows though fmodel_load
  returned 2; prime suspect model_bind.c:183's single on_change
  slot overwritten by the second pane's bind).

NOT DONE (budget ended): STEP 3 fdk-edit (§24 full spec), STEP 4
fdk-monitor, STEP 5 fdk-notes, STEP 6 PRE-1.0-CHECKLIST + GTK4/Qt6
re-audit, STEP 7 housekeeping (test-apps/README index). The four
apps' §24 specs live in the §24 round brief; fdk-edit/fdk-monitor/
fdk-notes exist as smoke-clean §25-era skeletons in test-apps/ —
they need the full feature work per spec.

All suites green at cut-off (5331da2): make test, test-dbus ALL
PASS, test-x11 3/3, verify-exports OK, Wayland suite PASS, all four
app smokes clean (§27 STEP 1 run).

## NOT done (next round, in order)
- STEP 4d: split view (two panes), sidebar bookmarks (add/remove/reorder
  by drag — fdk_list_bound_reorder_selected exists), DnD between
  panes/tabs (the drop-target registration exists in panes_build; the
  DRAG source arm logic exists — needs a two-pane wiring pass), trash
  restore verification, .desktop file + app icon, tray/notify are wired
  (verify on a real session only → "should work").
- STEP 4e: README.md with the 50,000-entry gate numbers (NOT measured
  yet — needs a generated 50k dir + a timed open/scroll run), APIs
  exercised, FDK issues (G-1..G-5 from FINDINGS.md), sources (trash
  spec, desktop-entry spec, shared-mime-info — URLs in the §24 report).
- STEP 4f: screenshots (main window, dark theme, a dialog) —
  FDK_TESTAPP_SCREENSHOT env var is already implemented in main.c's
  rig_screenshot(); run under Xvfb with FDK_THEME dark.
- STEP 5: fdk-edit — NOT STARTED (fixed order: only after fdk-files
  passes its own checklist).
- STEP 6: PRE-1.0-CHECKLIST.md — NOT STARTED (comes after all apps).

## Performance gates NOT yet measured
50k folder open/scroll, 250ms monitor refresh, 50MB editor, 2000-note
search — all pending their apps' verification rounds.

## §26 Wayland verification (DONE, verified in-sandbox)
The §25 "should work" items are now verified: sway 1.10 headless
(WLR_BACKENDS=headless, pixman renderer) via the repo's canonical
scripts/run_wayland_suite.sh rig (its floating/pinning sway config is
LOAD-BEARING — windows must float at (100,60) for focus/click tests).
Results: make test-wayland == WAYLAND SUITE: PASS (65 [ok]); the G-2
thread suite on the Wayland backend green under ASan AND TSan (blocked-
poll wake 0.1-0.3 ms — the eventfd path works against wlroots);
fdk-files --smoke 3/3 rc=0 zero-leak; fdk-monitor/fdk-notes/fdk-edit
smoke clean on Wayland too. One fix landed (test-side, not an FDK API
gap): the key-repeat probe assumed `test_key delivered` meant focus on
THE test's window; under sway headless the enter can sit on an earlier
section's window and the injector's second virtual keyboard churns
focus after the tap. The probe now counts downs on the window under
test, with a bounded tap-retry. (A first "hang" during bring-up was my
own dying compositor instance, not an FDK bug — reproducibly clean
once sway runs persistently via setsid.)

## Environment notes (re-setup each round)
source /home/z/my-project/scripts/s25-env.sh; dev debs: apt-get download
libxrandr-dev libxtst-dev libxi-dev (+libxrandr2/libxtst6/libxi6) into
/tmp/x11dev with dpkg -x; wayland: scripts/restore_wayland_toolchain.sh;
identity + hooks already in .git/hooks (pre-commit/commit-msg). No xauth
→ use the throwaway-Xvfb pattern. sway/weston binaries live in
/home/z/apt/prefix/usr/bin (WAYLAND suite was NOT run this round —
sandbox had no compositor session; run make test-wayland next round).

## §28 status (round cut; read this first)
DONE + PUSHED (164f70e): STEP 2a-2c — G-9, FOUR defects fixed
test-first (tests/test_model.c: test_multiple_views_one_model RED
first — view 1 froze at 2 rows on the second bind, the exact
fdk-files rows=0 symptom):
1. model_bind used the model's SINGLE on_change slot → second
   pane's bind overwrote the first's handler. Fix: per-model
   LISTENER LIST (m_listener + internal fdk__model_add/remove_
   listener in model.c, declared in widgets_internal.h); binding
   subscribes through it; fire_change runs the app's slot THEN
   every listener; unbind/destroy of one view spares the others.
2. Proxies read the CHILD'S RAW ROW ARRAY — but fdk-files chains
   store→filter→sort, so the sort's child is the filter's FACE
   (rows[] empty). Fix: proxy_rebuild collect, the sort comparator
   (proxy_cmp_keys), and the filter predicate all read through
   the PUBLIC row API; cell_row RECURSES through face children;
   schema_of WALKS to the chain's bottom store.
3. The filter predicate ran only in set_predicate — a store
   mutation rebuilt the permutation UNFILTERED. Fix: the rule
   re-applies inside proxy_rebuild.
4. sort_set_key's refire never reached an upstream proxy. Fix:
   fire_face (slot+listeners) is the one refire path in
   child_changed/set_key/set_predicate.
Plus: listeners freed in fdk_model_destroy + both proxy destroys
(LSan caught); proxy multi-view test + two-view a11y LIST_ITEM
exposure test; example 15 binds TWO lists to the same proxy face;
fdk_model.h docs state the N-views contract. ALL suites green
after (make test, test-dbus, test-x11 222, TSan, verify-exports
983, Wayland PASS, smoke 4/4).

STEP 2e (drag rig) — NOT GREEN, committed WIP (dcced0d): the app
now boots into the DETAIL list (the DRIG gate forces VIEW_DETAIL
— the app's real prefs said icons, and drag arming is list-only);
the calibrated click at (250,131) SELECTS (probe evidence:
fdk-files-drig-detail.png + pixel diff shows status bar + column
changes). But all four legs still FAIL on the DROP side: D1
between panes, D2 to-bookmark, D3 bookmark reorder, D4 drag-out
to scripts/xdnd_sink (started by run_drig.sh at (1020,100), log
/tmp/drig-sink.log never gets the payload).
NEXT LEAD (the strongest, untested): the click selects, so
POINTER_DOWN reaches the row, but the DRAG never starts — check
whether FDK_WIDGET_POINTER_MOTION events are DELIVERED to
view_event while the button is HELD (the arming path needs
motion-past-threshold: panes.c:413). One XTEST experiment with
FDK's x11_events.c motion delivery during a held button (does the
pointer grab swallow them? does the LIST widget consume them
before the event callback?) pins it. If FDK lacks motion
delivery during a press → that is a G-10 FDK bug, fix test-first.

STEP 3 (notify chase) — NOT STARTED this round (budget went to
the drag rig; the timebox was not consumed — 0 of 10 experiments
used). The §27 evidence stands (G-8 committed; see §27 block).

ROUND SCOPE NOTE: per the §28 brief, fdk-edit (STEP 4) was NOT
started — the round was limited to Steps 1-3 + the drag rig.

All suites green at cut-off (after dcced0d): make test, test-dbus,
test-x11 3/3, TSan, verify-exports, Wayland PASS, smoke 4/4.

## §29 status (round cut by budget — READ THIS FIRST)

### DONE + PUSHED this round
- env re-setup after a sandbox recycle (x11dev debs re-extracted,
  wayland prefix restored, xtest_driver + xdnd_sink rebuilt WITH
  RPATH; pc files' prefixes rewritten to /tmp/x11dev/usr).
- **THE §28 DRAG-RIG MYSTERY, PINNED**: the xtest_driver source's
  unconditional self-check MOVES THE POINTER to (11,22) on EVERY
  invocation — every multi-call drag sequence had its pointer reset
  between steps, so every leg left its target and cancelled. Fixed
  in scripts/xtest_driver.c (selfcheck now opt-in via
  XTEST_SELFCHECK env) — NOT in the FDK repo; rebuild the driver
  from that source when the sandbox resets.
- **G-12** (419431f): the release-time status drain in the X11 drag
  source (ButtonRelease may beat the target's last XDndStatus);
  interop sink relocated to 760,500 (was inside the dnd-receiver
  test's window footprint). test-x11 GREEN 227 after.
- **G-10** (7af865b): fdk_widget_set_drag_source / get — the
  widget-level drag-source controller (press on ANY descendant,
  incl. consumed-by-row presses; motion past slop fires on_begin
  once; click stays a click). tests/test_widget_drag_source.c GREEN,
  example 18 has the controller twin pad, exports 985, all suites
  green (make test, dbus, x11 227, TSan, Wayland PASS).

### COMMITTED IN THE FINAL BATCH (this commit, unverified-on-origin)
- fdk-files: PER-PANE listing models (model_pane_init/shutdown,
  fmodel_load(p,dir,hidden), model_pane_of/model_active) — the
  singleton made pane B show pane A's directory (the split view was
  fake); views bind their own pane's chain; panes.c/bookmarks.c use
  the G-10 controller for drag-out; DRIG-GEO geometry export in the
  DRIG gate (the rig no longer assumes pixels); drag_rig parses
  DRIG-GEO (usage: drag-rig <disp> <xid> <geo-log>); run_drig.sh
  env fixed + waits for GEO. make test GREEN, exports 985, apps
  build, fdk-files builds. **The rig legs D1-D4 still FAIL — see the
  G-11 lead below. THE RIG WAS NOT RE-RUN after the last rebuild.**

### THE G-11 LEAD (self-drop, PARKED — test parked in
### test-apps/g11_self_drop_test_parked.c, NOT in the suite)
A drag FDK starts and drops on THE SAME window (drag-between-panes)
delivers an EMPTY payload or no drop at all: g11 test failed at
`g11.dropped` (assertion at tests/test_x11_integration.c:9594 in the
parked copy). Root cause HYPOTHESIS (strong, not fully pinned): the
receiver's drop fetch calls fdk__x11_read_selection(requestor=the
drop-target window); the XdndSelection's owner is FDK's OWN
clip_helper — the SelectionRequest that must be served for the
SelectionNotify to arrive sits in FDK's own queue and the wait loop
(wait_selection_notify_on in src/platform/x11/x11_clipboard.c) never
dispatches it. I implemented the pump (drain queued
SelectionRequests via fdk_x11_dnd_serve_selection /
fdk_x11_clipboard_handle_event + helper ClientMessages via
fdk_x11_dnd_source_handle_helper_message, XPutBackEvent for
not-ours) — the test STAYED RED (same assertion), so the patch was
REVERTED before commit; the pump may still be right but incomplete
(next suspects: (a) the requestor is an APP window whose
atom_fdk_selection property the serving path writes correctly? (b)
the drop acceptance at fd fdk__core_drag_accepts_at re-enters and
deadlocks so no drop event is dispatched — instrument by printing
whether FDK_EVENT_DRAG_DROP is dispatched at all; (c) XCheckTypedEvent
steals the ButtonRelease that ends the drag). The parked test file
has the full repro; run it from tests/ by re-adding it to the suite.

### STEP 3 (notify --smoke boot race): NOT STARTED (0 experiments).
### STEPS 4-8 (fdk-edit/monitor/notes PASS A+B, final audit): NOT
### STARTED — the budget went to the env rebuild (sandbox recycled
### MID-ROUND, /tmp wiped, stale-clone reset) + the drag chase.

All suites green at cut-off: make test, test-dbus, verify-exports
985, test-x11 227, TSan, Wayland PASS, apps build, smoke 4/4
(earlier in the round; not re-run after the final app rebuild).
