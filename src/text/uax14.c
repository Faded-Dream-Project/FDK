/*
 * uax14.c — Unicode line breaking (UAX #14), the pair-table core.
 *
 * IMPLEMENTATION NOTES
 *
 * This is an original implementation of the Unicode Standard's
 * line-breaking algorithm (UAX #14, revision 53 as of Unicode 16.0)
 * written from the SPECIFICATION's published pair table, not a
 * translation of ICU, GtkLineWrap, harfbuzz's or any other
 * implementation's source. The line-break property classes
 * (the "eng" of the algorithm) are derived from Unicode data:
 * LineBreak.txt is part of the Unicode Character Database and is
 * distributed under the Unicode Terms of Use (permissive; the
 * license allow-list in docs/licensing-policy.md covers UCD data,
 * the same basis as the existing emoji/age data in tests/). The
 * generated table in uax14_table.h is a compact encoding of that
 * data (ranges -> class), regenerable by the script noted there.
 *
 * WHAT IS IMPLEMENTED (the subset that matters for a greedy
 * wrapper around shaped text):
 *
 *  1. The full pair table (the LB_CM/ZWJ combining-mark handling
 *     is approximated by classing them CM/ZWJ and letting the
 *     table treat them as glue, which is the behavior for the
 *     scripts FDK's shaper covers today).
 *  2. The CJK classes: ID (ideographic), CJK small kana, the
 *     post-CJK no-break rules (LBkinsoku), and the classic
 *     "break anywhere between ideographs" (LB AA-style ID x ID
 *     = break opportunity) plus the prohibition set — no break
 *     BEFORE closing punctuation (CL, CP), no break AFTER opening
 *     (OP) or Hiragana/Katakana leaders where the table says so.
 *  3. The mandatory breaks: BK/CR/LF/NEWLINE/NEL (the caller
 *     already handles \n \r; those cps never reach the table).
 *  4. Space-class handling: SP is the "the run of spaces before
 *     a non-space decides" glue per LB7, exactly like the 0x20
 *     special case the walker had before.
 *
 * WHAT IS DELIBERATELY NOT IMPLEMENTED: the dictionary-based
 * (South East Asian, LB SA) and regular-expression-based
 * (contextual, LB CB) tailoring — SA cps class as ID here
 * (break allowed), which is the standard no-tailoring fallback
 * and matches what Pango and browsers do without language data.
 *
 * The walker (src/text/layout.c) consumes exactly two questions:
 * "is this cp a space-class glue?" and "is there a break
 * opportunity BETWEEN these two cps?" — everything else stays
 * in the greedy pen accounting that already exists.
 */

#include "uax14.h"

/* The generated class table + pair table (see the file header
 * for provenance and the regeneration script). */
#include "uax14_table.h"

fdk_u8 fdk__uax14_class(fdk_u32 cp) {
    /* The fast ASCII path: the table covers it, but the two most
     * common classes deserve a branchless shortcut. */
    if (cp < 0x80u) {
        if (cp == 0x20u) {
            return FDK_UAX14_SP;
        }
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
            (cp >= '0' && cp <= '9')) {
            return FDK_UAX14_AL;
        }
    }
    return fdk__uax14_class_lookup(cp);
}

bool fdk__uax14_break_between(fdk_u32 before, fdk_u32 after) {
    fdk_u8 cb = fdk__uax14_class_lookup(before);
    fdk_u8 ca = fdk__uax14_class_lookup(after);
    return fdk__uax14_pair_breaks(cb, ca);
}

bool fdk__uax14_is_space_class(fdk_u32 cp) {
    return fdk__uax14_class_lookup(cp) == FDK_UAX14_SP;
}
