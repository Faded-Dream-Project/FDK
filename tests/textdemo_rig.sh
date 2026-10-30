#!/bin/sh
# textdemo_rig.sh — TESTING-ON-DESKTOP §10's hand-equivalent, headless.
#
# What the owner verifies by eye on the real desktop (complex scripts,
# bidi, fallback, caret) has a mechanical core this rig proves under
# Xvfb — making §10's own promise true ("the sandbox rig verifies ink
# presence + a contiguous joined Arabic run"). Runs at
# FDK_UI_FONT_SIZE=24 so the joined runs are big enough to see.
#
#   R1  text_demo renders each §10 string to a PNG (exit 0, the
#       "[screenshot] ... (WxH, N blocks)" line, real ink) in BOTH the
#       shaped and the legacy ($FDK_SHAPING=0) posture;
#   R2  THE DIFFERENTIAL: the same string, shaped vs legacy, must
#       render DIFFERENTLY (>= 300 differing pixels — both postures
#       draw identical chrome, so the delta is the string's own
#       pixels). A machine whose primary lacks the script (the owner's
#       Noto Sans) renders .notdef in BOTH postures when the fallback
#       channel is broken — delta ~0 — so this leg catches the §10
#       failure signature directly; when the channel works, the
#       fallback face's real glyphs (plus RTL reordering) delta hard
#       (measured ~1300 px);
#   R3  THE JOIN ANCHORS: shaped Devanagari contains a >= 35 px
#       connected run (the shirorekha joins नमस्ते into one component;
#       measured 43 px shaped vs 20 px legacy .notdef), and shaped Han
#       contains a denser near-square ideograph than anything the
#       legacy posture draws (measured 131 vs 106 ink px);
#   R4  THE NEGATIVE CONTROL: legacy Devanagari has NO >= 25 px
#       component — proving R3's detector sees the failure it guards.
#
# The glyph-level truth (fallback face resolved, no glyph 0, contextual
# joining forms chosen, conjunct clusters merged) is pinned by
# tests/test_shaping.c's six-script matrix; this rig pins the PIXELS.
#
# Requires: Xvfb, python3 with PIL. Fonts: whatever fontconfig
# resolves (the CI images install the per-script Noto packages —
# ci.yml / packaging/ci/*.sh). Skips HONESTLY (exit 0, disclosed)
# when a script's font is absent: fc-list decides.
#
# Usage: sh tests/textdemo_rig.sh   (from the repo root, after make)

set -u

DEMO="${DEMO:-./build/examples/text_demo}"
if [ ! -x "$DEMO" ]; then
    echo "TEXTDEMO-RIG SKIP: $DEMO not built (run: make examples)"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "TEXTDEMO-RIG SKIP: python3 not available"
    exit 0
fi
if ! python3 -c "import PIL" 2>/dev/null; then
    echo "TEXTDEMO-RIG SKIP: python3-PIL not available"
    exit 0
fi
if ! command -v Xvfb >/dev/null 2>&1; then
    echo "TEXTDEMO-RIG SKIP: Xvfb not available"
    exit 0
fi

XVFB_NUM=$((95 + ($$ % 300)))
XVFB_DISP=":$XVFB_NUM"
rm -f "/tmp/.X11-unix/X$XVFB_NUM" 2>/dev/null
Xvfb "$XVFB_DISP" -screen 0 1024x768x24 >/tmp/fdk-textdemo-xvfb.log 2>&1 &
XVFB_PID=$!
sleep 2
FAILURES=0

cleanup() {
    kill "$XVFB_PID" 2>/dev/null
    wait "$XVFB_PID" 2>/dev/null
    rm -f "/tmp/.X11-unix/X$XVFB_NUM" 2>/dev/null
    rm -rf "$TMP" 2>/dev/null
}
TMP=$(mktemp -d)
trap cleanup EXIT INT TERM

# font_present <lang>: does fontconfig have ANY face for the language?
font_present() {
    fc-list ":lang=$1" 2>/dev/null | grep -q . && return 0
    return 1
}

# demo_shot <out.png> <env-assignments...> -- <args...>
demo_shot() {
    out="$1"; shift
    envs=""
    while [ "$1" != "--" ]; do
        envs="$envs $1"
        shift
    done
    shift
    # env(1) applies the VAR=VALUE words (an unquoted $envs expansion is
    # not parsed as assignments by POSIX sh).
    # shellcheck disable=SC2086
    env DISPLAY="$XVFB_DISP" FDK_NO_GL=1 ASAN_OPTIONS=detect_leaks=0 \
        FDK_UI_FONT_SIZE=24 $envs \
        "$DEMO" "$@" --screenshot "$out" >/tmp/fdk-textdemo-run.log 2>&1
}

# analyze <png> -> "WIDE DENSE" : the widest connected ink component
# below chrome width, and the ink of the densest near-square component.
# Works on light AND dark themes (ink = luminance far from the
# dominant background).
analyze() {
    python3 - "$1" <<'PYAN'
import sys
from PIL import Image

img = Image.open(sys.argv[1]).convert("L")
w, h = img.size
px = img.load()
hist = [0] * 256
for y in range(h):
    for x in range(w):
        hist[px[x, y]] += 1
bg = max(range(256), key=lambda v: hist[v])
ink = {(x, y) for y in range(h) for x in range(w)
       if px[x, y] <= bg - 40 or px[x, y] >= bg + 40}

comps = []
seen = set()
for p in sorted(ink):
    if p in seen:
        continue
    stack = [p]
    seen.add(p)
    x0 = x1 = p[0]
    y0 = y1 = p[1]
    n = 0
    while stack:
        cx, cy = stack.pop()
        n += 1
        if cx < x0: x0 = cx
        if cx > x1: x1 = cx
        if cy < y0: y0 = cy
        if cy > y1: y1 = cy
        for q in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
            if q in ink and q not in seen:
                seen.add(q)
                stack.append(q)
    if x1 - x0 + 1 >= 200:   # window chrome: borders/rules/full-width bars
        continue
    comps.append((x1 - x0 + 1, y1 - y0 + 1, n))

wide = max((c[0] for c in comps), default=0)
dense = [c for c in comps if 12 <= c[0] <= 26 and 12 <= c[1] <= 26]
dn = max((c[2] for c in dense), default=0)
print("%d %d" % (wide, dn))
PYAN
}

# diff_px <png-a> <png-b>: pixels differing by >= 30 luminance.
diff_px() {
    python3 - "$1" "$2" <<'PYD'
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("L")
b = Image.open(sys.argv[2]).convert("L")
if a.size != b.size:
    print("999999")
    raise SystemExit
d = ImageChops.difference(a, b)
px = d.load()
n = sum(1 for y in range(d.size[1]) for x in range(d.size[0])
       if px[x, y] >= 30)
print(n)
PYD
}

wide_of() { echo "$1" | awk '{print $1}'; }
dense_of() { echo "$1" | awk '{print $2}'; }

# rig_leg <label> <lang> <string> [min_wide] [min_dense_delta]:
#   R1 both postures run; R2 the differential; R3 the anchors.
rig_leg() {
    label="$1"; lang="$2"; str="$3"
    min_wide="${4:-0}"
    min_dense_delta="${5:-0}"
    shaped_png="$TMP/shaped-$lang.png"
    legacy_png="$TMP/legacy-$lang.png"
    if ! demo_shot "$shaped_png" -- "$str"; then
        echo "TEXTDEMO-RIG FAIL: $label — text_demo (shaped) exited nonzero"
        FAILURES=$((FAILURES + 1))
        return
    fi
    if ! grep -q "screenshot" /tmp/fdk-textdemo-run.log; then
        echo "TEXTDEMO-RIG FAIL: $label — no [screenshot] line"
        FAILURES=$((FAILURES + 1))
        return
    fi
    if ! demo_shot "$legacy_png" FDK_SHAPING=0 -- "$str"; then
        echo "TEXTDEMO-RIG FAIL: $label — text_demo (legacy) exited nonzero"
        FAILURES=$((FAILURES + 1))
        return
    fi
    s=$(analyze "$shaped_png")
    l=$(analyze "$legacy_png")
    d=$(diff_px "$shaped_png" "$legacy_png")
    sw=$(wide_of "$s"); lw=$(wide_of "$l")
    sd=$(dense_of "$s"); ld=$(dense_of "$l")
    ok=0
    if [ "$d" -lt 300 ]; then
        echo "TEXTDEMO-RIG FAIL: $label — shaped and legacy differ in" \
             "only $d px: the shaping pipeline changed nothing" \
             "(broken fallback renders .notdef in both postures)"
        ok=1
    fi
    if [ "$min_wide" -gt 0 ] && [ "$sw" -lt "$min_wide" ]; then
        echo "TEXTDEMO-RIG FAIL: $label — widest connected run is" \
             "$sw px, want >= $min_wide (not a joined run)"
        ok=1
    fi
    if [ "$min_dense_delta" -gt 0 ] &&
       [ "$sd" -lt $((ld + min_dense_delta)) ]; then
        echo "TEXTDEMO-RIG FAIL: $label — densest near-square component" \
             "grew only to $sd px (legacy $ld px): no dense ideograph"
        ok=1
    fi
    if [ "$ok" -eq 0 ]; then
        echo "R+ $label: diff $d px, widest $lw -> $sw px," \
             "densest $ld -> $sd px"
    else
        FAILURES=$((FAILURES + 1))
    fi
}

echo "== text_demo rig (§10 hand-equivalent) under Xvfb $XVFB_DISP =="

# ---- Arabic: the differential (RTL + joining change the pixels) ---------------
if font_present ar; then
    rig_leg "R2 Arabic (مرحبا)" ar "مرحبا"
else
    echo "R2 SKIP: no Arabic font on this machine (disclosed)"
fi

# ---- Devanagari: the shirorekha joins the whole word --------------------------
if font_present hi; then
    rig_leg "R3 Devanagari (नमस्ते)" hi "नमस्ते" 35
else
    echo "R3 SKIP: no Devanagari font on this machine (disclosed)"
fi

# ---- Han: the dense full-width ideograph --------------------------------------
if font_present zh; then
    # dense-delta 8: measured margins are +11 (wqy-zenhei on the
    # ubuntu runner) to +25 (NotoSansSC here); a .notdef pair sits
    # at ~0-5 (the differential below carries the leg if a lighter
    # face ever shaves the margin).
    rig_leg "R4 Han (漢字)" zh "漢字" 0 8
else
    echo "R4 SKIP: no CJK font on this machine (disclosed)"
fi

# ---- R5: the negative control — legacy Devanagari cannot join -----------------
if font_present hi && [ -f "$TMP/legacy-hi.png" ]; then
    l=$(analyze "$TMP/legacy-hi.png")
    lw=$(wide_of "$l")
    if [ "$lw" -lt 25 ]; then
        echo "R5 negative control: legacy widest run is $lw px (< 25)" \
             "— the R3 detector sees the failure it guards"
    else
        echo "TEXTDEMO-RIG FAIL: R5 — legacy posture produced a" \
             "${lw}px wide run; the control is contaminated"
        FAILURES=$((FAILURES + 1))
    fi
else
    echo "R5 SKIP: the control needs R3's fonts (disclosed)"
fi

if [ "$FAILURES" -ne 0 ]; then
    echo "== TEXTDEMO RIG: $FAILURES FAILURE(S) =="
    exit 1
fi
echo "== TEXTDEMO RIG OK (§10 pixel core verified under Xvfb) =="
exit 0
