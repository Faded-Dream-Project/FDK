# ACCESSIBILITY-AUDIT.md — the 1.0.0-dev accessibility audit (report only)

Date: 2026-11-20 · Scope: the whole shipped accessibility surface,
audited against what a real assistive-technology user needs. This
is a REPORT — nothing here changed code, per the task's terms.
Every finding cites the file and the fact.

## What FDK ships today

Three layers, each real and tested:

1. **The a11y tree is the widget tree** (`src/widget/a11y.c`):
   `fdk_a11y_describe()` walks any widget's registered
   `fdk_a11y_class` (41 roles, a 12-bit state set) without a
   parallel object system to keep in sync — the design GTK spent
   a decade correcting. Covered by 16 headless test groups in
   `tests/test_a11y.c` (describe, showing, notifications,
   actions, entry modes, role names, the virtual-view trio for
   menus/combos/notebooks, the Text interface ×2 — including
   UTF-8 —, relationships, the relations limit, caret
   notification, the extents seam).

2. **The AT-SPI bridge** (`src/desktop/atspi.c`, 2,136 lines): a
   native D-Bus AT-SPI2 implementation — Accessible (role, state,
   children, application, interfaces, attributes), the **Text**
   interface (GetText{At,After,Before}Offset, character extents,
   caret, selection), announcements, and focus events. Verified
   against a real bus by `make test-dbus` (the live rig asserts
   announcements and focus events arrive on the wire).

3. **The embedded narrator** (`src/widget/a11y_narrator.c`): an
   in-process screen reader core (no D-Bus, no bridge process)
   with a pluggable speaker and `.fmo` catalog support — the
   zero-dependency story `docs/architecture.md:324` documents.

## Findings, by severity

### High

**H1 — No Component, Value, Selection, or Collection interfaces
on the AT-SPI bridge.** `grep org.a11y.atspi.{Component,Value,
Selection,Collection} src/desktop/atspi.c` → 0 hits. Consequence:
a screen reader can READ the tree and the text, but Orca cannot
query a slider's value range, click a button by coordinates, or
enumerate a list's selection through the standard interfaces.
Workarounds exist (keyboard driving through focus + perform), but
this is the difference between "readable" and "fully drivable"
for AT users. The internal a11y tree HAS the data (actions,
values ride the describe/notification path); it is the wire
surface that is missing.

**H2 — No reduced-motion, high-contrast, or font-scaling
accessibility settings anywhere in the API or theme grammar.**
`grep reduced-motion|high-contrast` across `include/` and
`src/theme/` → 0. The animation engine has no way to say "tone it
down"; themes cannot declare a high-contrast variant; text size
follows the font the app picks, not a user preference. Modern
toolkits read these from the desktop settings (the same channels
the dark-mode code already speaks). This is a feature gap, not a
defect — but it is the gap AT users hit first.

### Medium

**M1 — Contrast is never checked.** No WCAG luminance/contrast
math exists anywhere in the tree (the only `4.5` hits are an
unrelated comment and a byte-mask). The theme grammar accepts any
fg/bg pair; the menu code even carries a hand-tuned workaround
for low-contrast themes (`src/widget/menu.c:321`). A
`fdk_theme_validate()` that flags fg/bg pairs below WCAG AA
(4.5:1 body, 3:1 large/UI) at LOAD time would fit the strict
fail-closed grammar's philosophy.

**M2 — The keyboard-only story is implicit.** Focus traversal
(`fdk_widget_tree_advance_focus`), mnemonics and accelerators
exist and are tested, but nothing asserts "every interactive
widget in the catalog is reachable and operable by keyboard
alone" — the canonical a11y guarantee. A keyboard-walk rig over
`04_widgets`'s catalog (the example that instantiates everything)
would pin it.

**M3 — No live-region semantics.** `fdk_a11y_announce()` maps to
the AT-SPI announcement, but there is no politeness level
(assertive vs polite) and no `live` property on widgets for
passive change narration (chat messages, progress, log tails).
Screen readers either miss updates or interrupt.

### Low

**L1 — 41 roles, but no `SEARCH`, `GRID`, `MATH`, or `HEADING`
roles.** The catalog covers the widget set it ships; the gap only
matters for exotic apps (a text editor's document area maps to
TEXT_VIEW, which exists).

**L2 — The narrator's catalog hook is global**
(`fdk_a11y_narrator_set_catalog` takes no domain), so a
multi-window app cannot localize announcements per-window. No
consumer has hit this.

**L3 — `fdk_a11y_info_free` is a no-op-style free (the struct is
flat, all strings inline-owned)** — correct today; the doc
comment should say so to prevent a double-free culture.

## What is verified good (worth keeping loudly)

- **The dual-bus design** (AT-SPI when a reader is on the bus,
  the in-process narrator when it isn't) degrades honestly — the
  same pattern as every other FDK desktop integration.
- **UTF-8 correctness in the Text interface** is tested
  (codepoint-index math, not byte-index — `test_text_interface_utf8`).
- **The relations API** (label-for, controlled-by, ...) ships
  with both a limit test and a lifecycle sweep on destroy
  (the G-6 fix).
- **Virtual views** exist for menus/combos/notebooks — data
  exposed to AT without instantiating widgets.

## Recommended order of attack (for the owner to decide)

1. **H1's Component + Value** (the two interfaces Orca uses
   hardest); Selection/Collection can follow.
2. **H2's reduced-motion + font scaling** (dark-mode's channel
   readers are 80% of the plumbing).
3. **M1's contrast validation** in the strict theme grammar.
4. M2's keyboard-walk rig; M3's live regions; the Lows as
   consumer demand appears.
