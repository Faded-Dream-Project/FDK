# §30 continuation state (ONE document — written 2026-11-02, applies until superseded)

This file replaced the stacked per-round status blocks (§25–§29).
Their durable facts survive in the SHORT HISTORY at the bottom; the
detail lives in git history. Read this top to bottom to resume.

## Remote proof at write time
origin/main == `7dfc015` (scripts: the test rigs move INTO the repo).
`git ls-remote origin main` is the authority; if you lack commits,
fetch + rebase. Identity: Faded Dream Project
<242450780+FemBoyGamerTechGuy@users.noreply.github.com> for author,
committer, tagger; NO trailers ever (hooks in .git/hooks enforce,
reinstalled by scripts/setup-sandbox.sh).

## Environment (ONE step after any sandbox recycle)
```
bash scripts/setup-sandbox.sh     # x11dev debs, wayland prefix, rig
                                  # binaries (RPATH'd), identity hooks
source scripts/env.sh             # PKG_CONFIG_PATH + LD_LIBRARY_PATH
make -j4 && make tools
```
The script is idempotent. Gotchas it already encodes: deb .pc files
need `prefix=/tmp/x11dev/usr` rewritten; the non-PIC `.a` archives
must be deleted from both prefixes (they break the libfdk.so link);
the driver's selfcheck is opt-in via XTEST_SELFCHECK (it MOVES the
pointer — that bug was the §28 drag-rig mystery); the x11
integration suite spawns /home/z/my-project/scripts/xdnd_sink
(abs path) — setup-sandbox.sh builds it there.

## What is DONE and verified (per area)

**FDK core** — all suites green at §29 cut (make test, test-dbus,
verify-exports 985, test-x11 227, TSan, Wayland PASS, 4-app smoke):
- G-2 threads (fdk_post/worker pool, ASan+TSan) · G-3 list
  selection spans · G-4 image decode any-thread · G-5 iconview
  borrowed pixmaps · G-8 app notifications · G-9 N-views-one-model
  (per-model listener list; proxies read the public row API) ·
  G-10 widget drag-source controller
  (`fdk_widget_set_drag_source`; tests/test_widget_drag_source.c;
  example 18 twin pad) · G-12 release-time XDndStatus drain in the
  X11 drag source (419431f).
- The §28 drag-rig four dead legs were an ENVIRONMENT bug (the
  driver selfcheck), pinned and fixed in the driver — NOT an FDK
  bug. The dedicated G-12 regression test status: see FINDINGS.md
  (test-dnd_source_gui remains the live interop leg; a
  deterministic red-without-fix test may be impossible — the exact
  reasoning is recorded there).

**fdk-files (DONE through §29)** — dual panes with PER-PANE listing
models (a605cec; the singleton made pane B show pane A's directory —
the split view was fake), tabs, breadcrumbs, detail/icons/compact
views, sort/filter, search-as-you-type with typeahead, copy/move/
delete/trash on the worker engine with progress, thumbnails (LRU +
XDG cache, thread-legal pipeline), bookmarks, prefs, i18n EN+RO,
a11y, session restore (§28: app + both lanes), file watch, --smoke
LSan-clean, drag-out via the G-10 controller, DRIG-GEO export for
the rig.

**fdk-shell / overlay** — done in earlier rounds (taskbar, launcher,
overlays; see test-apps/fdk-shell/).

## What is OPEN (with evidence and the exact next action)

**O-1 (chase, ≤8 experiments): G-11 self-drop.** A drag FDK starts
and drops on THE SAME window (drag-between-panes) delivers an empty
payload or no drop at all. Repro: test-apps/g11_self_drop_test_parked.c
(failed at `g11.dropped` — the drop event never arrives). Evidence
from §29's instrumented runs: the drag source SAW the motion and the
sink's XDndStatus (accept) arrived before the release; the release
drain works; the failure is on the RECEIVE side of a self-drag.
Leads, in order: (a) print whether FDK_EVENT_DRAG_DROP is dispatched
at all (the g11 test's callback); (b) the drop's selection fetch —
owner is FDK's own clip_helper, so the SelectionRequest must be
served by the SAME connection that is waiting; my §29 pump attempt
in wait_selection_notify_on stayed red and was REVERTED — before
retrying it, check (c) whether XCheckTypedEvent in the pump steals
the ButtonRelease that ends the drag (breaks the handshake), and (d)
whether the drop acceptance path re-enters dispatch (deadlock).
Master rule applies: fix in FDK test-first (unpark the test into
tests/ first, watch it fail, then fix).

**O-2 (chase): drag-rig legs D1–D4 verdicts.** The rig (test-apps/
fdk-files/run_drig.sh) now reads DRIG-GEO geometry the app exports.
D4 (drag OUT to the external sink) should pass since the driver fix;
D1/D2 are blocked by O-1 (the drop lands on the app's own window).
Re-run and record verdicts; if O-1 is fixed the legs should follow.

**O-3 (chase, 0/8 experiments used): notify --smoke boot race.**
The §27 evidence: G-8 notifications are done (the app API, the
fdk-notifyd helper, the desktop-file action buttons); the OPEN item
is a boot race when the daemon is cold — the app's notify --smoke
sometimes misses. Plan: strace -f -tt the daemon and the app; check
the daemon's limits in session.conf; the ordering of
Hello/RequestName/AddMatch on the first connection; whether the
closed connection sent anything before Hello completed.

**O-4: fdk-edit — NOT STARTED (code: nothing exists yet).** The §24
spec PASS A: tabs with modified indicator, open/save/save-as,
undo/redo, find/replace plain+regex, line-number gutter, syntax
highlighting (C/shell/Markdown/JSON/Makefile) via the
range-attribute engine with theme-aware colors, icon toolbar,
status bar (line:col, encoding, line endings, language), word wrap,
recent files, session restore, drag-file-to-open, UTF-8/BOM/UTF-16
+ CRLF/LF. PASS B (§24): 50 MB open/scroll/search MEASURED,
find-in-files, block/multi-selection, go-to-line, bracket matching,
auto-indent, comment toggle, grouped undo, RTL/mixed-script
(HarfBuzz/FriBidi), IME (text-input-v3 + X11), command palette,
preferences dialog, print, markdown preview, external-change
detection with reload prompt.

**O-5: fdk-monitor — NOT STARTED.** PASS A: reads /proc and /sys
ITSELF; Overview tab with live CPU/memory/network graphs on the FDK
canvas; Processes tab (model/view table, sort/filter, kill/terminate
with confirmation); tray icon with a live CPU pixmap, tooltip and
menu; refresh timer with pause; worker threads for anything that
can block. PASS B: Resources/Disks/Network tabs, per-core CPU,
disk IO, hwmon temps/fans, battery, process tree + column chooser +
renice, threshold notifications with action buttons, pinch/zoom/pan
on graphs, compact always-on-top widget mode, the fdk-shell-overlay
bar variant on the same model code, own-CPU-use at 250 ms refresh
MEASURED.

**O-6: fdk-notes — NOT STARTED.** PASS A: sidebar tree (model/view),
rich text (bold/italic/underline/strike, headings, lists,
checkboxes, links), live search, tags, crash-safe atomic storage
with autosave, clipboard rich+plain, trash. PASS B: inline images
(paste + drag-drop), code blocks, color/size attributes, pinned
notes, PDF export (paginated), plain-text and Markdown export, print
preview + dialog, reminders with notification snooze + tray badge,
zoom, distraction-free mode, focus timer with fdk_animation, full
a11y, RTL text, 2,000-note search MEASURED.

**O-7: STEP 4 audit — NOT STARTED.** test-apps/PRE-1.0-CHECKLIST.md
(every FDK API area: used-and-solid / used-had-bugs-fixed /
NOT exercised; remaining 1.0 blockers; ranked to-do), the GTK4/Qt6
widget-index re-audit via webfetch (repeat until nothing new),
test-apps/README.md (index of all four apps, build/run on a real
desktop, what to try, how to report).

## Rules of engagement (unchanged)
APPS FIRST, chases LAST. Master rule: when an app needs a feature,
API, widget, behavior or fix FDK lacks — add it to FDK, failing test
FIRST, then public API + docs + example + export check, recorded in
test-apps/FINDINGS.md. No in-app workarounds, no loosened warnings,
no weakened tests. All suites green at every push (make test,
test-x11, test-dbus, test-wayland, test-threads-tsan,
verify-exports). ta_buf for bounded strings. No D-Bus library
dependency in FDK. No version bumps, tags or releases. Push a WIP:
checkpoint after EVERY step that changes code, even red. Use
webfetch for specs and record URLs.

## SHORT HISTORY (durable facts from §25–§29)
- §25: ta_buf.h (ta_copy/ta_join/ta_fmt + truncated flag) is the
  bounded-string rule; teardown registry (4a) and the worker-engine
  ops (4b) are the patterns for app cleanup and blocking work.
- §26: Wayland lane verified (rig PASS 65; G-2 ASan+TSan on wl).
- §27: G-8 notifications (app API + fdk-notifyd + desktop-file
  action buttons); the notify boot race evidence is in the §27
  block of git history (O-3 above carries it forward).
- §28: G-9 (N views on one model — per-model LISTENER LIST;
  proxies must read the PUBLIC row API; face children recurse;
  schema_of walks to the bottom store) — fdk-files' rows=0 was
  this, FIXED, never revisit. The drag-rig dead legs were the
  driver selfcheck bug (see Environment above).
- §29: sandbox recycle mid-round; per-pane models in fdk-files;
  G-10 + G-12 pushed; G-11 parked (O-1); the apps-first order was
  adopted because the rounds before lost everything to drag chases.
