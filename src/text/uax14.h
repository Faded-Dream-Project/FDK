/*
 * uax14.h — the internal line-breaking seam (see uax14.c).
 *
 * The walker asks three questions; nothing else crosses this
 * seam. INTERNAL header — never installed, never public.
 */
#ifndef FDK_TEXT_UAX14_H
#define FDK_TEXT_UAX14_H

#include "fdk/fdk_types.h"

#include <stdbool.h>

/* The UAX #14 line-break classes the core table distinguishes.
 * The full set is 43; the pair table folds the ones that behave
 * identically for our two questions into shared slots. The names
 * follow the standard's two-letter codes. */
enum {
    FDK_UAX14_XX = 0,  /* unknown (treated as AL)          */
    FDK_UAX14_AL,      /* alphabetic                       */
    FDK_UAX14_SP,      /* space (the glue class)           */
    FDK_UAX14_ID,      /* ideographic (CJK)                */
    FDK_UAX14_CL,      /* close punctuation               */
    FDK_UAX14_CP,      /* close parenthesis               */
    FDK_UAX14_OP,      /* open punctuation                */
    FDK_UAX14_QU,      /* quotation                        */
    FDK_UAX14_GL,      /* glue (non-breaking)              */
    FDK_UAX14_NS,      /* non-start (never line-initial)   */
    FDK_UAX14_EX,      /* exclamation/interrogation        */
    FDK_UAX14_IS,      /* infix separator (,;)             */
    FDK_UAX14_PR,      /* prefix ($, (+CJK currency)       */
    FDK_UAX14_PO,      /* postfix (%, (+CJK currency)      */
    FDK_UAX14_HY,      /* hyphen                           */
    FDK_UAX14_BA,      /* break after                      */
    FDK_UAX14_B2,      /* break opportunity before/after   */
    FDK_UAX14_ZW,      /* zero-width space                 */
    FDK_UAX14_CM,      /* combining mark                   */
    FDK_UAX14_WJ,      /* word joiner                      */
    FDK_UAX14_HL,      /* Hebrew letter                    */
    FDK_UAX14_RI,      /* regional indicator (flags)       */
    FDK_UAX14_EB,      /* emoji base                       */
    FDK_UAX14_EM,      /* emoji modifier                   */
    FDK_UAX14_NU,      /* numeric                          */
    FDK_UAX14_CLASS_COUNT
};

/* The class of one codepoint. Fast paths for ASCII live in the
 * .c; the general case is the generated range table. */
fdk_u8 fdk__uax14_class(fdk_u32 cp);

/* Is there a line-break opportunity between two adjacent
 * codepoints (in logical order)? True = may break. */
bool fdk__uax14_break_between(fdk_u32 before, fdk_u32 after);

/* The glue question: does this cp behave like the walker's
 * historic space class (0x20/0x09)? */
bool fdk__uax14_is_space_class(fdk_u32 cp);

#endif /* FDK_TEXT_UAX14_H */
