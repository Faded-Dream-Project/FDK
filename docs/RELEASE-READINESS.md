# RELEASE-READINESS — what stands between this tree and 1.1.0

**Written:** 2026-11-05, after round 26 (`7f83a00`) and the CI repair
that follows it. This is the answer to the owner's question — "what's
left to build so FDK can build any app without limit?" — read out of
the tree, not out of memory: ROADMAP-1.1.0.md's 62-item register
(60 DONE, 1 open, 1 = this document's subject), PARITY.md's
category-by-category GTK4/Qt6 audit, TESTING-ON-DESKTOP.md's 28
owner-gate sections, the CI history, and the open bugs.

## The one-paragraph answer

The feature work is done. The 62-item gap register that this project
judged itself by (GTK4/Qt6 as the reference) is closed except for its
own release gate; PARITY.md's honest verdict is "at parity for the
application core — windows, input, 36+ widget classes, complex-script
text, gestures, model/view, AT-SPI, theming, D-Bus/tray/notifications/
session, i18n, undo, animation, shell surfaces, printing". What
removes "without limit" is not more building — it is, in order:
**fix the open bug, hold CI green, and spend a day on your real
desktops** walking the 28-section verification checklist. Then tag.

## 1. Code items (in priority order)

| # | item | size | status |
|---|------|------|--------|
| 1 | **CI green** — every push since `4d06933` (Nov 1, G-2) was red: the thread-test rule landed ABOVE `all:` in the Makefile, so a bare `make` (exactly what CI runs on a clean checkout) built only `build/tests/test_threads` + the archive — never `shared`, never `tools` — and `make test` died at test_cli_tools' `stat(build/tools/fdk-theme)`. Every incremental sandbox stayed green because its `build/tools/` already existed; only clean checkouts saw it. **Fixed this round**: `.DEFAULT_GOAL := all` (position-independent) + `test_cli_tools` now depends on `$(TOOL_BINS)` so `make test` is self-sufficient. Verified by a full clean-tree CI simulation (`make` → static+shared+tools; `make test` → all green). | S | **FIXED — watch the next CI run** |
| 2 | **The round-25 prompt-dialog repaint bug**: after a prompt dialog closes, the parent's editor stops painting (text intact; expose/damage bug in modal teardown; save picker unaffected). The one CONTINUATION-filed open bug a user will hit in daily editor use. **FIXED in round 28**: root cause — a modal dialog is a normal toplevel, not a popup, so teardown delivers no anchored parent damage and the parent's Expose is the WM's choice (bare Xvfb always sent one, hiding it from every test run). Fix — `dialog_damage_parent()` invalidates the parent's whole tree on both teardown paths (respond + early destroy), identity-compared against the live-window list (the borrowed parent is never dereferenced). Regression: `test_dialog_close_damages_parent` (X11 suite) pins the damage contract itself, so it fires without a WM too. | S–M | **FIXED — round 28** |
| 3 | The PARITY.md honest tail — each small, each a composition of shipped primitives, none blocking: inline color/font chooser *widgets* (the dialogs exist), text on extension surfaces (hosting widgets on `fdk_ext_surface`), the Pango attribute tail (gravity/features/stretch), the widget-catalog gaps GTK names (InfoBar, SearchBar, CenterBox, EditableLabel, DropDown, Video, ShortcutsWindow — all label+box+revealer compositions), a table-widget with per-column headers (List columns + the model cover it today). | S each | OPEN, by choice |

## 2. The verification gate (M-9 — the only roadmap item left)

No code remains. These need YOUR hardware because the sandbox
cannot produce them; TESTING-ON-DESKTOP.md §1–28 is the walkthrough:

- **Wayland desktop (labwc or sway)**: notifyd/trayhost vs the real
  incumbents (§1–7), the UI font-size following (§8), text_demo's
  complex scripts on your fonts (§10), a real input method — fcitx5
  typing CJK through text-input-v3 (§11), runtime re-anchoring +
  auto-hide (§17), the EGL present lane on real GPU hardware (§19),
  subsurfaces (§20), your drawing tablet (§21), foreign-toplevel
  (§22), session save/restore (§23), idle inhibition (§24),
  cursors (§25), touch (§26).
- **X11 desktop (Openbox + the GTX 1050 Ti)**: the GL lane under the
  proprietary driver (§27 — the strict-ES2 upload fix is landed and
  rig-proven on Mesa; your card is the sign-off), the NVIDIA
  Wayland round (§28), multi-monitor hotplug (§12), activation +
  startup notification (§13), drag icons (§14), portals (§15),
  Orca/AT-SPI (§16).

## 3. Release engineering (present; the checklist)

Already in the tree and verified: `make install/uninstall` with
SONAME discipline (`libfdk.so.1`), pkg-config file generated from
`fdk_version.h` (single source of truth), LICENSE + THIRD-PARTY +
licensing-policy docs, the five-family packaging matrix
(debian/rpm/arch/alpine/void) with clean-container install-tests in
CI, per-push CI (build + both suites + shaping postures + textdemo
rig + sway rig + verify-exports at 1010 symbols), 25 documents in
docs/ including abi-policy, versioning, packaging, security.

What actually remains:

1. **CI green on main** — done this round; keep it that way (the
   process rule this incident taught: never push without reading the
   run; a green local battery on an incremental tree proves nothing
   about a clean checkout).
2. **The tag decision** — the tree versions itself `1.1.0-rc2`
   (development posture, no tags exist, by the owner's recorded
   decision). When §2's checklist comes back green, the release act
   is: bump `fdk_version.h` to `1.1.0`, CHANGELOG final pass, tag
   `v1.1.0`, publish the five packages. That is the owner's call
   alone (docs/versioning.md, M-9).
3. Optional polish, not blocking: generated API reference (the
   headers are the documented API; a doxygen pass would be nice,
   the 25 docs already cover architecture and usage).

## 4. The honest "without limit" statement

What the owner asked for — "any apps without limit" — is achieved
within FDK's recorded scope, and the limits that remain are
documented decisions, not missing work: Linux desktop only (X11 +
Wayland; no Windows/macOS backend, by charter), plain-C explicit
API (no GObject/moc runtime), token themes (not full CSS),
immediate-mode rendering over a framebuffer with an optional GL
present lane, single UI thread (documented; `fdk_post` for workers).
PARITY.md §1–12 states each of these with its trade-off. Anything
outside those fences that an app needs is, by the project's own
rule, "a bug in FDK" — file it and it goes on this list.

## 5. Housekeeping done alongside this document

- `fdk-shell/` in the worktree is an old INSTALL tree (renamed to
  `fdk-shell-overlay/` long ago; sources live there) — gitignored so
  it can never be committed by accident.
- The uncommitted wayland_egl patch the sandbox rollback orphaned
  was verified already re-landed upstream (`3d989c8`, `5a67416`) —
  nothing was lost.
