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
