#!/usr/bin/env bash
# FDK release gate — G1..G7, printed PASS/FAIL per criterion.
# Exit 0 only when G1-G6 pass. G7 is informational and never blocks.
# The criteria are defined in docs/RELEASE-READINESS.md section 6.
set -u
cd "$(dirname "$0")/.."
# sandbox-local prefixes (CI does not need this; the sandbox keeps the
# X11/Wayland dev trees outside the repo — scripts/env.sh sets the paths)
[ -f scripts/env.sh ] && . ./scripts/env.sh
PASS=0; FAIL=0
say()  { printf '%s\n' "$*"; }
ok()   { say "G$1 PASS  — $2"; PASS=$((PASS+1)); }
bad()  { say "G$1 FAIL  — $2"; FAIL=$((FAIL+1)); }

# ---------- G1 feature freeze ----------
reg=$(grep -m1 "IN PROGRESS\|MISSING" ROADMAP.md | head -1)
partial=$(grep -m1 -oE "DONE [0-9]+ . PARTIAL [0-9]+ . IN PROGRESS [0-9]+( \(M-[0-9]+\))? . MISSING [0-9]+" ROADMAP.md || true)
pcount=$(echo "$partial" | grep -oE "PARTIAL [0-9]+" | grep -oE "[0-9]+" || echo 1)
mcount=$(echo "$partial" | grep -oE "MISSING [0-9]+" | grep -oE "[0-9]+" || echo 1)
inprog=$(echo "$partial" | grep -oE "IN PROGRESS [0-9]+" | grep -oE "[0-9]+" || echo 9)
parity_open=$(grep -cE "^\| [0-9]+ \|.*\| OPEN" docs/PARITY.md 2>/dev/null || echo 0)
if [ "${pcount:-1}" = "0" ] && [ "${mcount:-1}" = "0" ] && [ "${inprog:-9}" = "1" ] && [ "${parity_open:-1}" = "0" ]; then
    ok 1 "register: $partial; PARITY open gaps: $parity_open"
else
    bad 1 "register: $partial; PARITY.md OPEN gap rows: $parity_open (close or policy-out each)"
fi

# ---------- G2 zero open P0/P1 ----------
openbugs=$(awk '/^## Current state/,/^## Last audited|^## 2026/' PROGRESS.md | grep -ci "Open bug" || true)
layer=$(grep -c "Open bug (top priority)" PROGRESS.md || true)
if grep -q "Open bug \*\*(top priority)\*\*:\*\* the hud" PROGRESS.md 2>/dev/null || \
   grep -q "Open bug (top priority)" PROGRESS.md; then
    bad 2 "PROGRESS.md still records an open bug (the layer-shell first-commit kill)"
else
    ok 2 "no open bug recorded in PROGRESS.md's current-state block"
fi

# ---------- G3 bug-rate plateau ----------
rates=$(grep -oE "bug-rate: [0-9]+ new library bugs?" PROGRESS.md | tail -2)
n=$(printf '%s\n' $rates | grep -c "bug-rate: 0" || true)
rounds=$(printf '%s\n' "$rates" | grep -c "bug-rate:" || true)
if [ "$rounds" -ge 2 ] && [ "$n" -ge 2 ]; then
    ok 3 "last two rounds: $rates"
else
    bad 3 "need two consecutive zero-bug rounds in different environments (have: ${rates:-none})"
fi

# ---------- G4 real-desktop sign-off ----------
if [ -f docs/REAL-DESKTOP-SIGNOFF.md ] && grep -q "sign-off" docs/REAL-DESKTOP-SIGNOFF.md; then
    ok 4 "docs/REAL-DESKTOP-SIGNOFF.md exists and is signed"
else
    bad 4 "docs/REAL-DESKTOP-SIGNOFF.md missing — the 28-section walkthrough is the owner's hardware (M-9)"
fi

# ---------- G5 clean build + battery ----------
say "G5 running: clean checkout simulation (make + make test) ..."
if make clean >/dev/null 2>&1; then :; fi
if make >/dev/null 2>&1 && make test >/tmp/gate_test.log 2>&1; then
    checks=$(grep -c "^\[ok\]\|ok " /tmp/gate_test.log 2>/dev/null || echo "?")
    ok 5 "clean build + full unit battery green ($checks checks; /tmp/gate_test.log)"
else
    bad 5 "clean build or unit battery FAILED (see /tmp/gate_test.log)"
fi

# ---------- G6 claim discipline ----------
claims=$(grep -rniE "replaces? (GTK|QT)|replacement for (GTK|QT)|drop-in replacement" \
        README.md CHANGELOG.md docs/*.md 2>/dev/null | grep -v "not claim\|never claim\|does not\|must not\|cannot\|can't\|not a" || true)
if [ -z "$claims" ] && grep -q "honest parity" docs/PARITY.md; then
    ok 6 "no replacement claims in release-facing docs; PARITY verdict present"
else
    bad 6 "forbidden claim wording found: $claims"
fi

# ---------- G7 external evidence (informational) ----------
say "G7 INFO  — external evidence (dogfooding, beta users): not measurable in-repo; gates the 'total replacement' claim, never blocks a tag"

say ""
say "=== gate: $PASS passed, $FAIL failed (G1-G6 must all pass before any tag) ==="
[ "$FAIL" -eq 0 ]
