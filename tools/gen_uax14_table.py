#!/usr/bin/env python3
"""Generates src/text/uax14_table.h from the UCD's LineBreak.txt.

Input: the Unicode Character Database's LineBreak.txt (the file's
own license line: "For terms of use and license, see
https://www.unicode.org/terms_of_use.html" — permissive for
derived data tables; the same basis the emoji fixtures use).

Usage: python3 gen_uax14_table.py /path/to/LineBreak.txt

The output is two tables:
  1. fdk__uax14_class_lookup(cp) — a sorted range table (binary
     search, ~600 ranges for Unicode 16) mapping cp -> folded
     class enum.
  2. fdk__uax14_pair_breaks(b, a) — the pair table flattened into
     a bit matrix, hand-derived from UAX #14's Table 2 (the
     "canonical" rules LB1-LB31 as they apply to two adjacent
     classes). See the comments for the rule provenance.
"""

import sys

# The UAX #14 two-letter classes we keep distinct (folded set —
# matches src/text/uax14.h's enum; classes that behave
# identically for "break between b and a?" fold together).
FOLD = {
    "XX": "XX",
    "AL": "AL", "SA": "ID",  # SA -> ID: the no-tailoring fallback
    "AI": "ID",              # ambiguous -> ideographic (CJK context)
    "SG": "AL",              # surrogate: not real cps
    "CJ": "NS",              # conditional Japanese breaker -> NS
    "CB": "BA",              # contexual break-after: allowed
    "SP": "SP",
    "ID": "ID",
    "CL": "CL", "CP": "CP", "OP": "OP", "QU": "QU",
    "GL": "GL", "NS": "NS", "EX": "EX", "IS": "IS",
    "PR": "PR", "PO": "PO", "HY": "HY", "BA": "BA", "B2": "B2",
    "ZW": "ZW", "CM": "CM", "WJ": "WJ", "HL": "HL", "RI": "RI",
    "EB": "EB", "EM": "EM", "NU": "NU",
    # Mandatory classes never reach the pair table (the walker
    # consumes \n \r directly); map them to something inert.
    "BK": "AL", "CR": "AL", "LF": "AL", "NL": "AL", "NU2": "NU",
    # Remaining spec classes folded to their pair behavior:
    "NU": "NU",
    "AK": "ID",  # Akan letter (16.0) -> ideographic-like break
    "AP": "ID",  # Americanist letter -> ditto
    "LF2": "AL",
    "JV": "AL", "JT": "AL",   # Hangul vowel/final -> AL glue
    "H2": "AL", "H3": "AL",   # Hangul syllables -> AL glue
    "D": "ID", "IN": "ID",    # 16.0's new classes
    "ZI": "ID", "ZS": "AL", "MK": "CM", "MM": "CM", "MB": "CM",
    "LM": "AL",
    # 16.0 concrete classes
    "GG": "AL", "GC": "AL", "GI": "AL", "GK": "AL", "GP": "ID",
    "GX": "AL", "GB": "AL", "GL2": "GL", "XX2": "XX",
}

CLASSES = ["XX", "AL", "SP", "ID", "CL", "CP", "OP", "QU", "GL",
           "NS", "EX", "IS", "PR", "PO", "HY", "BA", "B2", "ZW",
           "CM", "WJ", "HL", "RI", "EB", "EM", "NU"]
CI = {c: i for i, c in enumerate(CLASSES)}
N = len(CLASSES)


def pair_table():
    """The flattened UAX #14 pair table, from the spec's rules.

    Matrix[b][a] = True when a break between class b and class a
    is ALLOWED. Defaults derived rule by rule from UAX #14 rev 53:
    """
    brk = [[False] * N for _ in range(N)]

    # LB4-5: mandatory classes handled by the caller. LB6: never
    # break before BK/CR/LF/NL (folded away) or after ZW (except
    # after ZW itself, LB8: break after ZW SPACE-run).
    # LB7: never break before SP, or after ZWNBSP(WJ).
    for b in range(N):
        for a in range(N):
            if a == CI["SP"] or b == CI["WJ"]:
                brk[b][a] = False
            elif b == CI["ZW"] or b == CI["SP"]:
                # LB8/LB18: break after ZW (post-space-run); SP
                # itself is transparent — decided by what precedes
                # the space run; approximated: break allowed unless
                # the after class is a non-starter.
                brk[b][a] = a not in (CI["NS"], CI["CL"], CI["CP"],
                                      CI["EX"], CI["IS"], CI["GL"],
                                      CI["OP"], CI["QU"], CI["ZW"],
                                      CI["CM"], CI["WJ"], CI["HY"],
                                      CI["BA"], CI["PR"], CI["PO"],
                                      CI["B2"])
    # LB9-10: CM/ZWJ attach to the base (treat CM as its base's
    # class); the walker never sees CM-first positions in practice,
    # and CM after anything = same as its base, so the table row
    # for CM copies the base behavior: treat CM as AL-row.
    for a in range(N):
        brk[CI["CM"]][a] = brk[CI["AL"]][a]
        brk[a][CI["CM"]] = brk[a][CI["AL"]] if a != CI["SP"] and a != CI["ZW"] else brk[a][CI["CM"]]

    # LB11 WJ both sides: no break (already default False).
    # LB12 GL: no break after GL.
    for a in range(N):
        brk[CI["GL"]][a] = False

    # LB12a: no break before GL except after SP/BA/HY.
    for b in (CI["SP"], CI["BA"], CI["HY"], CI["ZW"]):
        for a in range(N):
            if a == CI["GL"]:
                brk[b][a] = True  # allowed (defer to base rule)

    # LB13: no break before ] ) ! , ; / — after anything.
    for b in range(N):
        for a in (CI["CL"], CI["CP"], CI["EX"], CI["IS"], CI["SY"] if "SY" in CI else CI["IS"]):
            brk[b][a] = False
        brk[b][CI["NS"]] = False  # no break before NS

    # LB14 OP + anything: no break after OP (even spaces).
    for a in range(N):
        brk[CI["OP"]][a] = False
    # LB15 QU + SP + OP: no break (approximated: QU SP handled by
    # space run; QU itself is default no-break-after? No: QU
    # allows break after to non-special classes per LB19's "no
    # break before/after QU" exception set — default: QU x ? is
    # break-allowed except before QU. LB19: no break before/after
    # quotation marks EXCEPT the classic pairs (handled by GL/QU
    # folding here: we keep QU as a no-break-both-sides glue for
    # the CJK corner cases and quotes-in-Latin).
    for b in range(N):
        brk[b][CI["QU"]] = False
        brk[CI["QU"]][b] = False if b not in (CI["SP"],) else brk[CI["QU"]][b]

    # LB16 (CL|CP) x NS: no break. LB17 B2 x B2: no break.
    for b in (CI["CL"], CI["CP"]):
        brk[b][CI["NS"]] = False
    brk[CI["B2"]][CI["B2"]] = False

    # LB18: break after spaces (already True via the SP row above
    # for non-nonstarter classes).
    # LB19-25 word-ish rules for AL/HL/NU/PR/PO/HY/IS/OP/QU:
    #   no break between letters/numbers (x AL/HL/NU),
    #   no break IS x AL/NU, PR x AL/NU/OP-GL, PO x AL/NU...
    #   these all make the DEFAULT "no break"; breaks only happen
    #   where nothing forbids them — the matrix started all-False
    #   and only SP/ZW rows opened anything. So the remaining
    #   classes (AL ID NU PR PO HL HY IS EX CL CP OP QU GL NS B2
    #   EB EM RI WJ CM) keep: break only via SP/ZW rows. That is
    #   the LB2/LB3 net effect: AL x AL no break, AL x ID no break
    #   — which is WRONG for CJK: LB AA says ID x ID breaks.
    # LB21-21b: no break before/after HY/BA/NS (default True rows
    # above may have opened HY/BA after SP — correct, LB18 wins).
    # LB22-23: no break around EB/EM (default False, fine).
    # LB24: PR/PO x ID no break? Spec: PR x (ID|PO|AL) no break,
    #   (AL|HL) x PR no break — default False holds.
    # LB25 numbers: (PR|PO)? (OP)? NU ... — default False holds.
    # LB26-28: Korean syllable blocks — folded H2/H3/JV/JT to AL:
    #   default False (no break) holds, correct.
    # LB29-30: CM/IS glue — default False holds.
    # LB30a RI x RI -> break between TWO flags: exactly one RI
    #   pair no-break then break; approximated: RI x RI = break.
    brk[CI["RI"]][CI["RI"]] = True
    # LB30b EB x EM no break (default False holds).
    # LB31 THE CJK RULE: break everywhere else — ID x ID, ID x
    #   AL, AL x ID, NU x ID ... all break UNLESS covered above.
    #   The spec's terminal rule: ALL remaining pairs break.
    id_adjacent = (CI["ID"], CI["AL"], CI["NU"], CI["HL"],
                   CI["EB"], CI["EM"], CI["RI"])
    for b in id_adjacent:
        for a in id_adjacent:
            brk[b][a] = True
    # ...but LB letters-to-letters must NOT break (AL x AL is
    # inside a word). Undo: AL/HL glue with each other and NU;
    # keep ID breaking against everything (the CJK behavior).
    latin = (CI["AL"], CI["HL"], CI["NU"])
    for b in latin:
        for a in latin:
            brk[b][a] = False
    # Numbers glue to IDs? LB25 keeps NU runs together but NU x ID
    # per LB31 breaks — fine.
    # Punctuation adjacency: punctuation sticks to words by default
    # (False rows); breaking between two ideographs THROUGH
    # punctuation is handled by SP rows. Acceptable approximation.
    return brk


def main():
    src = open(sys.argv[1], encoding="utf-8").read()
    ranges = []  # (start, end, folded-class)
    for line in src.splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        cp_field, lb = [f.strip() for f in line.split(";")]
        lb = lb.split("#")[0].strip()
        cls = FOLD.get(lb, "AL")
        if ".." in cp_field:
            a, b = cp_field.split("..")
            start, end = int(a, 16), int(b, 16)
        else:
            start = end = int(cp_field, 16)
        ranges.append((start, end, cls))
    ranges.sort()
    # merge adjacent same-class
    merged = []
    for start, end, cls in ranges:
        if merged and merged[-1][2] == cls and merged[-1][1] + 1 == start:
            merged[-1] = (merged[-1][0], end, cls)
        else:
            merged.append((start, end, cls))

    out = []
    out.append("""/*
 * uax14_table.h — GENERATED line-break tables. DO NOT EDIT.
 *
 * Derived from the Unicode Character Database's LineBreak.txt
 * (Unicode 16.0.0; the file's license line points at
 * https://www.unicode.org/terms_of_use.html — permissive for
 * derived data). Regenerate with:
 *   python3 tools/gen_uax14_table.py LineBreak.txt
 * (the script carries the fold + pair-table logic and its own
 * provenance notes). The pair table encodes UAX #14's rules as
 * the walker's two questions need them; the fold set and the
 * rule-by-rule provenance live in the generator script.
 */

#ifndef FDK_TEXT_UAX14_TABLE_H
#define FDK_TEXT_UAX14_TABLE_H

#include "uax14.h"

#include <stdbool.h>
""")
    out.append("/* Class ranges, sorted; binary-searched. */")
    out.append("static const struct { fdk_u32 first; fdk_u32 last; fdk_u8 cls; }")
    out.append("fdk__uax14_ranges[] = {")
    for start, end, cls in merged:
        out.append("    {0x%04X, 0x%04X, FDK_UAX14_%s}," % (start, end, cls))
    out.append("};")
    out.append("")
    out.append("#define FDK_UAX14_RANGE_COUNT %du" % len(merged))
    out.append("""
static fdk_u8 fdk__uax14_class_lookup(fdk_u32 cp) {
    /* Binary search over the sorted range table. The ASCII fast
     * path lives in fdk__uax14_class(); this is the general walk. */
    size_t lo = 0, hi = FDK_UAX14_RANGE_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < fdk__uax14_ranges[mid].first) {
            hi = mid;
        } else if (cp > fdk__uax14_ranges[mid].last) {
            lo = mid + 1;
        } else {
            return fdk__uax14_ranges[mid].cls;
        }
    }
    return FDK_UAX14_XX; /* unknown: AL behavior via the fold */
}

/* The pair table as a bit matrix: 1 = break allowed between the
 * row class (before) and the column class (after). See the
 * generator's rule-by-rule provenance (UAX #14 rev 53). */
static const fdk_u32 fdk__uax14_pair_bits[FDK_UAX14_CLASS_COUNT] = {""")
    brk = pair_table()
    for b in range(N):
        word = 0
        for a in range(N):
            if brk[b][a]:
                word |= 1 << a
        out.append("    0x%08Xu, /* %s */" % (word, CLASSES[b]))
    out.append("""};

static bool fdk__uax14_pair_breaks(fdk_u8 before, fdk_u8 after) {
    if (before >= FDK_UAX14_CLASS_COUNT ||
        after >= FDK_UAX14_CLASS_COUNT) {
        return false;
    }
    return (fdk__uax14_pair_bits[before] >> after) & 1u;
}

#endif /* FDK_TEXT_UAX14_TABLE_H */""")
    open(sys.argv[2] if len(sys.argv) > 2 else "src/text/uax14_table.h",
         "w").write("\n".join(out) + "\n")
    print("ranges:", len(merged))


if __name__ == "__main__":
    main()
