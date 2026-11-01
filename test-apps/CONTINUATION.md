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

## Environment notes (re-setup each round)
source /home/z/my-project/scripts/s25-env.sh; dev debs: apt-get download
libxrandr-dev libxtst-dev libxi-dev (+libxrandr2/libxtst6/libxi6) into
/tmp/x11dev with dpkg -x; wayland: scripts/restore_wayland_toolchain.sh;
identity + hooks already in .git/hooks (pre-commit/commit-msg). No xauth
→ use the throwaway-Xvfb pattern. sway/weston binaries live in
/home/z/apt/prefix/usr/bin (WAYLAND suite was NOT run this round —
sandbox had no compositor session; run make test-wayland next round).
