/* widget_sizes.c — the D1 measurement probe (make sizes).
 *
 * Prints the numbers the embed-budget decision (PRE-1.0-DECISIONS D1)
 * is ruled by: the real internal widget size, the public embed budget,
 * and the headroom between them. The budget stays 1024 only while the
 * largest widget is under 640 bytes (the decision's own threshold);
 * the printed headroom is the growth runway before that re-decision.
 *
 * Internal-header include (widget_internal.h) is fine here: this is a
 * measurement TOOL, not shipped code — it compiles against the same
 * tree it measures, so the numbers are always the tree's truth.
 */
#include <stdio.h>

#include "widget_internal.h"
#include "fdk/fdk_widget.h"

int main(void) {
    size_t w = sizeof(struct fdk_widget);
    size_t e = sizeof(fdk_widget_embed);
    printf("sizeof(struct fdk_widget)   = %zu bytes\n", w);
    printf("sizeof(fdk_widget_embed)    = %zu bytes (FDK_WIDGET_EMBED_U64 = %u)\n",
           e, (unsigned)FDK_WIDGET_EMBED_U64);
    printf("headroom                    = %zu bytes (%.1fx the current size)\n",
           e - w, (double)e / (double)w);
    printf("budget verdict (D1 rule: keep 1024 iff the largest is under 640): %s\n",
           w < 640 ? "KEEP 1024" : "RAISE TO 2048");
    /* The assert the build already carries (widget_internal.h) —
     * repeated here so `make sizes` can never print a green table
     * for a tree that does not compile. */
    _Static_assert(sizeof(struct fdk_widget) <= sizeof(fdk_widget_embed),
                   "struct fdk_widget outgrew the FDK_WIDGET_EMBED_U64 budget");
    return 0;
}
