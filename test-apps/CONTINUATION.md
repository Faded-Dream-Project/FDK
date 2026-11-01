# §24 continuation state (written at round cut-off)

## Pushed (verify with git log)
- 289a1ef skeleton (icons×70, common shell, Makefile+smoke)
- 56a083b FDK GAP #1: fdk_notebook_set_page_label (test-first, export-verified)
- 4a3e08d fdk-files complete draft (WIP; commits above pushed to origin/main)

## Where APP 1 (fdk-files) stands
DONE: compiles ZERO-WARNING (repo flag set), boots under Xvfb, navigates,
mkdir/rename/trash round-trips, PHASE rig prints, exits rc=0.
INCOMPLETE (next round, in this order):
1. LSan teardown pass — leaks: crumb payloads (pane_refresh_breadcrumb
   callocs without tracking), view_ctx (pane_build_view), fmodel.infos on
   search, context-menu models (menus_ctx_track never frees on activation),
   desktop_app arrays, chooser/progress/props/trash ctx structs.
   Fix pattern: app-owned registries freed in ta_close, like ta_icon_buttons_free.
2. Thumbnails (image files → fdk_picture in icons view; async via timer).
3. Split view (second pane in the Paned — structure exists, build fn needed).
4. Sidebar bookmarks (add/remove/reorder by drag — fdk_list_bound_reorder_selected).
5. README.md with the honest checklist + perf numbers (50k folder gate)
   + Sources section (trash spec, desktop-entry spec, shared-mime-info
   spec — URLs in the report).
6. .desktop file + app icon (icons/folder.svg → PNG via a tiny renderer or
   fdk surface save).

## Apps 2-4 (fdk-edit, fdk-monitor, fdk-notes)
NOT STARTED. The API survey + common shell are in place. Specs already
fetched this round: trash (complete), desktop-entry field codes (complete),
shared-mime-info globs2+magic (complete). Editor needs: textview+entry push,
50MB gate, syntax tokenizer over fdk_attr/fdk_span, POSIX regcomp find,
session restore, print via fdk_print_job. Monitor needs: /proc//sys parsers,
canvas graphs + fdk_animation, tray pixmaps (fdk_tray_item_set_icon_pixmap),
notify thresholds with actions, 250ms gate. Notes needs: rich text model,
PDF export (fdk_print_job_finish_to_file), reminders+notify actions,
fdk_a11y_narrator, 2000-note search gate.

## Environment (survives? NO — re-setup each round)
- dev pkgs: /tmp/x11dev + /tmp/wldev (see exported PKG_CONFIG_PATH etc. in
  the next round's first command; copy from this round's history or rebuild
  from the repo's own test-x11 recipe)
- no xauth (xvfb-run broken → use the throwaway-Xvfb pattern in
  test-apps/Makefile's test-apps-smoke)
- identity: git config global+local set; hooks pre-commit/commit-msg live;
  GIT_AUTHOR/COMMITTER env must be re-exported each shell
- token: see the task brief (never on disk)

## Full-suite gate (run before each push)
make test && make test-x11 && make test-dbus && make verify-exports
(+ the sway suite where the sandbox has it — it does NOT here; X11 only)
