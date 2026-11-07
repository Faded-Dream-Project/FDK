/* click_rig.c — round 10: the wrong-row proof rig.
 *
 * Boots nothing itself: run_clickrig.sh starts fdk-files (DRIG
 * layout) under Xvfb, waits for the DRIG-GEO lines, then THIS rig
 * clicks a vertical line down pane0's list with REAL XTest events
 * and reads the app's "PHASE: sel" telemetry from the shared log —
 * the selection must be exactly the row whose painted rect contains
 * the click (header clicks select nothing, empty space deselects).
 *
 * Two lessons this rig exists to enforce (see CONTINUATION round 10):
 *  - clicks are spaced >=600ms: inside FDK_WINDOW_DBLCLICK_MS (400)
 *    consecutive same-row clicks consume as double-clicks and
 *    ACTIVATE instead of selecting — a rig artifact that perfectly
 *    impersonates an app bug;
 *  - the telemetry distinguishes "nothing fired" (no new PHASE
 *    line) from "fired with zero" ("sel 0 (none)") — both are
 *    answers, only one is silence.
 *
 * Env: CLICKRIG_X (default 60+pane x), CLICKRIG_FROM / CLICKRIG_TO
 * (y range, window coords), CLICKRIG_STEP (px, default 5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

static void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Reads /tmp/clickrig-app.log from *offset, returning the LAST new
 * "PHASE: sel" line (or "" if none landed). Updates *offset. */
static void last_new_sel(long *offset, char *out, size_t cap) {
    out[0] = '\0';
    FILE *f = fopen("/tmp/clickrig-app.log", "r");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    if (end < *offset) {
        *offset = end; /* log was truncated (new boot) */
    }
    fseek(f, *offset, SEEK_SET);
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "PHASE: sel", 10) == 0) {
            snprintf(out, cap, "%.*s",
                     (int)(strlen(line) >= cap ? cap - 1
                                               : strlen(line)),
                     line);
            /* strip the newline */
            char *nl = strchr(out, '\n');
            if (nl != NULL) {
                *nl = '\0';
            }
        }
    }
    *offset = end;
    fclose(f);
}

int main(void) {
    const char *env;
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "click_rig: no display\n");
        return 2;
    }
    long offset = 0;
    last_new_sel(&offset, (char[256]){0}, 256); /* skip boot lines */

    int x = (env = getenv("CLICKRIG_X")) != NULL ? atoi(env) : 198;
    int y0 = (env = getenv("CLICKRIG_FROM")) != NULL ? atoi(env) : 20;
    int y1 = (env = getenv("CLICKRIG_TO")) != NULL ? atoi(env) : 110;
    int step = (env = getenv("CLICKRIG_STEP")) != NULL ? atoi(env) : 5;
    if (step < 1) {
        step = 1;
    }

    for (int y = y0; y <= y1; y += step) {
        XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
        XSync(dpy, False);
        msleep(80);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        msleep(50);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        /* 600ms between clicks — OUTSIDE the 400ms double-click
         * window (FDK_WINDOW_DBLCLICK_MS). At ~330ms the sweep's
         * consecutive clicks on the same row consumed as
         * DOUBLE-clicks: the second of each pair ran
         * list_fire_activated (open/navigate) instead of a plain
         * re-select, and the gesture fallout silenced the rest of
         * the sweep — a rig artifact, not an app bug. A probe
         * clicks like a slow human. */
        msleep(600);

        char sel[256];
        last_new_sel(&offset, sel, sizeof sel);
        if (sel[0] == '\0') {
            printf("y=%3d -> (no selection change)\n", y);
        } else {
            printf("y=%3d -> %s\n", y, sel);
        }
        fflush(stdout);
    }
    XCloseDisplay(dpy);
    return 0;
}
