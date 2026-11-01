# FINDINGS — the FDK bug/gap ledger (the §24 protocol)

Every FDK defect a test-app hits lands here. Protocol: a failing
regression test in FDK's own suite FIRST (tests/), then the fix in
src/ or include/, then the app continues on the public API. Nothing
is worked around silently inside an app — if an app needs something
FDK lacks, that is a bug in FDK by project rule.

| ID | App | What failed | Root cause | Fix commit | Regression test |
| -- | --- | ----------- | ---------- | ---------- | --------------- |
| — | — | *(the ledger opens empty: the skeleton round found nothing yet — the apps' real rounds fill it)* | | | |

## Verdict vocabulary

- **BUG** — FDK behaved wrong (crash, wrong pixels, wrong values,
  leak). Fixed + tested in-tree.
- **GAP** — FDK lacks an API a real application needs. Judged per
  the project rule: fixed in FDK (new public API, documented,
  export-checked, tested) rather than worked around — unless the
  gap is a deliberate v1 boundary (docs/PARITY.md), in which case
  it is recorded here and the app states the limit honestly.
- **DOC** — the headers/docs misdescribed behavior. Docs fixed.
