# FINDINGS — the FDK bug/gap ledger (the §24 protocol)

Every FDK defect a test-app hits lands here. Protocol: a failing
regression test in FDK's own suite FIRST (tests/), then the fix in
src/ or include/, then the app continues on the public API. Nothing
is worked around silently inside an app — if an app needs something
FDK lacks, that is a bug in FDK by project rule.

| ID | App | What failed | Root cause | Fix commit | Regression test |
| -- | --- | ----------- | ---------- | ---------- | --------------- |
| G-1 | fdk-files | The Notebook had `fdk_notebook_page_label` (get) but NO setter — a file manager cannot retitle a tab after navigating (every multi-document app needs this: editors retitle on save-as). The app was about to work around it by rebuilding the notebook on every navigation. | Missing API since Phase 9: the getter shipped alone. | (this commit: tests/test_widgets2.c `test_notebook_set_page_label` written FIRST — red; then `fdk_notebook_set_page_label` in notebook.c + header) | tests/test_widgets2.c::test_notebook_set_page_label |
| G-2 | fdk-files (§25, by design) | No cross-thread story AT ALL: docs/threading.md's "Getting work back onto the UI thread" was a Phase-1 placeholder promising `fdk_invoke_on_ui_thread` "planned". A file manager must copy/move/thumbnail off-thread or its UI freezes; §24 worked around it with timer-sliced operations on the MAIN loop (recorded there as friction). | Missing subsystem: no post queue, no wake fd in the pump, no worker facility, no enforcement of the threading rule. | §25: `fdk_post` + eventfd/pipe wake wired into the pump's poll set (backend-independent), `fdk_worker`/`fdk_job` pool with cooperative cancel, `FDK_ERR_WRONG_THREAD` refusal under `FDK_DEBUG_THREAD_CHECKS` (debug builds), docs/threading.md rewritten from placeholder to contract. Tests FIRST: tests/test_threads.c written red, then the implementation. | tests/test_threads.c (stress 256 posts / ordering 1024 / wakeup latency / worker roundtrip / cancel / shutdown-in-flight / wrong-thread refusal) — ASan via test-x11, TSan via `make test-threads-tsan` |

## Verdict vocabulary

- **BUG** — FDK behaved wrong (crash, wrong pixels, wrong values,
  leak). Fixed + tested in-tree.
- **GAP** — FDK lacks an API a real application needs. Judged per
  the project rule: fixed in FDK (new public API, documented,
  export-checked, tested) rather than worked around — unless the
  gap is a deliberate v1 boundary (docs/PARITY.md), in which case
  it is recorded here and the app states the limit honestly.
- **DOC** — the headers/docs misdescribed behavior. Docs fixed.
