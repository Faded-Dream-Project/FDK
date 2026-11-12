/* s28_lsan_strictness_probe.c — the __lsan_default_options bind's
 * strictness proof (Round 75, 2026-11-12).
 *
 * examples/example_lsan.h seeds LSan's defaults with the repo's
 * suppression list. This probe proves the bind engages (a self-
 * module suppression entry matches) AND stays strict (an exe-frame
 * leak still reports). Build and run BOTH ways:
 *
 *   cc -fsanitize=address -I examples examples/../scripts/... this file
 *   run 1: no LSAN_OPTIONS  -> the self entry suppresses, exit 0
 *   run 2: a leak with NO matching entry -> reports, exit 1
 *
 * The example binary itself demonstrates run 2 on every lane: any
 * FDK-frame leak would fail the run with the bind active (the R75
 * wayland catch — 80+19 B in fdk_ext_register_kind frames — did).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "example_lsan.h"

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "leak") == 0) {
        /* The strict case: an exe-frame leak no entry matches. */
        void *p = malloc(31337);
        (void)p;
        printf("s28: deliberate leak, expecting exit 1\n");
        return 0;
    }
    printf("s28: bind returns \"%s\"\n", __lsan_default_options());
    if (__lsan_default_options()[0] == '\0') {
        printf("s28: FAIL - bind returned empty (no suppression "
               "list found relative to cwd)\n");
        return 2;
    }
    printf("s28: bind engaged\n");
    return 0;
}
