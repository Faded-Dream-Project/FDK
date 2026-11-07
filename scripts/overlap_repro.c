/* overlap-repro.c — the round-20 root cause, isolated.
 *
 * pane_reload/pane_new_tab pass t->dir to pane_navigate, which does
 * snprintf(t->dir, sizeof t->dir, "%.400s", abs_path) — dst == src.
 * C11 7.21.6.5: overlapping snprintf is undefined behavior. This
 * repro shows what this glibc actually does (built with the same
 * sanitizer flags as the app).
 */
#include <stdio.h>
#include <string.h>

int main(void) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", "/tmp/fdk-drig/left");
    printf("before: [%s] len=%zu\n", buf, strlen(buf));
    snprintf(buf, sizeof buf, "%.400s", buf); /* dst == src */
    printf("after:  [%s] len=%zu\n", buf, strlen(buf));
    return 0;
}
