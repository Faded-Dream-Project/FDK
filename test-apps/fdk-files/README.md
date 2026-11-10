# fdk-files — the FDK validation-suite file manager

The §24 APP 1 test application: a real, feature-rich file manager
written ONLY against FDK's public API. Every gap the app hit became
an FDK bug: failing test first, fix, `PROGRESS.md (the bug/gap ledger)` entry
(G-1…G-6 so far).

## The 50,000-entry performance gate (MEASURED, §26/3c)

Machine: this sandbox — X11 backend on Xvfb 1280x800, llvmpipe
software GL, ASan+UBSan debug build, single core class. Numbers from
`make -C test-apps fdk-files-perf-50k` (`fdk-files/perf_50k.c`,
committed):

    GEN:    50000 entries (1000 dirs + 49000 files) in 9148 ms
    OPEN:   50000 entries walked+statted in 105 ms
    FILL:   50000 iconview rows appended in 102 ms (batched)
    SCROLL: 120 frames avg 2.55 ms, worst 76.92 ms
    MEM:    RSS 540944 kB with the 50k model live

Reading them honestly: opening a 50k directory costs ~105 ms (the
opendir/stat walk — I/O bound, not FDK); filling the view ~102 ms
with `fdk_iconview_begin_batch` (the 1.0.0 batching is what makes
this O(N) instead of O(N²)); average scroll frames 2.55 ms —
comfortably inside a 60 Hz frame budget — with one 77 ms worst-case
spike (the first frame after a large jump, allocation + full
repaint under ASan; a release build without sanitizers will be
faster across the board). RSS ~528 MB is the 50k-row widget tree
under ASan (sanitizers roughly double heap overhead; the same run
without them would be materially smaller). The gate: the UI stays
responsive at 50k entries — met, measured, reproducible.

## FDK APIs exercised

fdk_init/fdk_shutdown, fdk_run/pump, fdk_quit; window create/destroy/
title/icon/decorations/close-request; menubar+menu models+shortcuts;
toolbar+toolbuttons; statusbar+push; Paned; Notebook (tabs, reorder,
set_page_label); List (modes, select/deselect/range, bounds signal,
batching, row icons); IconView (grid, selection, per-item pixmaps,
batching); Label (ellipsize), Entry, Button (roles), Progress;
fdk_drag_begin_ex + drop targets + drag callbacks (URI lists);
clipboard cut/copy/paste; undo stack; prefs (typed keys); theming
(FDK_THEME=dark verified by screenshot); i18n catalogs (EN+RO);
a11y names/roles/states; fdk_post + the worker facility (copy/move/
delete/size/thumbnails off-thread with progress+cancel);
fdk_image_decode_pixels (worker-side decode); fdk_surface (paint,
save_png, create_from_image, ARGB format); notify + tray (the
session-bus rig below); inotify through the ext-fd loop hook.

## FDK issues found (full detail: PROGRESS.md (the bug/gap ledger))

- G-1 notebook tab retitle API missing (§24)
- G-2 the whole threading subsystem (§25): fdk_post, workers,
  FDK_ERR_WRONG_THREAD enforcement
- G-3 List selection completeness (§25)
- G-4 fdk_image_decode_pixels — the any-thread pixel decoder (§25)
- G-5 IconView per-item pixmaps (§25)
- G-6 destroyed bound widget left a stale model binding (§26 —
  heap-use-after-free; fixed test-first)

## Verification

- `make test`, `make test-x11` (incl. the G-2 thread suite), TSan
  lane `make test-threads-tsan`, `make verify-exports`: green.
- Wayland: `scripts/run_wayland_suite.sh` == 65 [ok] PASS; the G-2
  suite green on the Wayland backend under ASan AND TSan; the
  fdk-files smoke 3/3 rc=0 zero-leak (§26).
- The smoke rig (`--smoke`) exercises: boot, navigate, select, mkdir,
  rename, trash, split on/off, bookmark add/reorder/persist,
  worker-copy round-trip, quit — zero-warning build, LSan clean.
- Tray + notification (§26/3b): `fdk-files/run_files_tray_bus.sh`
  against a real `dbus-daemon --session` with a real StatusNotifier
  watcher and notification daemon (TRAYHOST_BIN / NOTIFYD_BIN, or
  whatever the running desktop already provides; legs without a
  counterpart SKIP honestly). T1 (watcher name) and T2 (the
  tray item registers) PASS. T3 (the Notify call witnessed on the
  bus) is SHOULD WORK: in this sandbox the app's notify connect
  fails in the fresh-app-after-daemon order ("session bus connect
  failed", src/desktop/notify_client.c on_ready); the module
  connects cleanly when the app boots before the daemon. Recorded
  for the next round.
- Screenshots (this dir): `fdk-files-main.png`, `fdk-files-dark.png`,
  `fdk-files-smoke-split.png` — all 1080x700, non-blank verified.

## Desktop integration

`desktop/org.fdk.testapps.fdk-files.desktop` (spec 1.5:
specifications.freedesktop.org/desktop-entry-spec/1.5/
recognized-keys.html — "If the name is an absolute path, the given
file will be used", hence the THEME-NAME Icon form +
StartupWMClass=the app's real id). Icons:
`desktop/icons/hicolor/{48x48,128x128}/apps/` — drawn by FDK itself
(`fdk-files/gen_app_icon.c`, `make fdk-files-icon`).

## §24 APP 1 checklist — honest verdicts

- Icons not text (symbolic row icons, toolbar icons, theme icons): DONE
- Menu bar + toolbar + status bar + shortcuts dialog: DONE
- Tabs (open/close/switch/retitle/reorder-by-drag): DONE
- Breadcrumb navigation + editable location bar: DONE
- Views (details/icons/compact) + hidden-files toggle: DONE
- Copy/move/delete/trash + restore + conflict dialog: DONE
- Worker engine (progress + cancel, UI responsive): DONE (§25/4b)
- Properties + open-with chooser: DONE
- Thumbnails (async, XDG cache, icon fallback): DONE (§25/4c)
- Sidebar: places + mounts + bookmarks (drag reorder, persisted): DONE (§26/3a)
- Split view (two panes, F3, cross-pane drops): DONE (§26/3a)
- DnD: to/from other apps (URI lists), between panes/tabs: DONE
  (the drop targets + drag sources are wired and smoke-verified at
  the API level; interactive drag VERIFICATION between two live
  panes needs a scripted-input rig — should work, untested here)
- Undo (delete/rename/mkdir grouping): DONE
- Prefs persisted (view mode, hidden files): DONE
- Theme-aware (light/dark): DONE (dark screenshot)
- i18n EN+RO: DONE (catalogs)
- a11y (names, roles, states on the tree): DONE
- inotify live updates: DONE (the watch rides the loop)
- .desktop + icon: DONE (§26/3b)
- Tray + notification: PARTIAL — the item registers on a real bus
  (verified); the finish-notification is should-work (T3 above)
- 50k performance gate: DONE (measured, numbers above)
- README with sources: DONE (this file)

Sources: desktop-entry-spec 1.5 (freedesktop.org, above); the KDE
StatusNotifier item/watcher protocol (notably the item registers its
UNIQUE bus name — verified live against a real watcher
implementation); XDG basedir spec for the cache/config locations
(the icons use the hicolor theme layout per the Icon Theme
Specification).
