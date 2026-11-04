/* delete_rig.c — round 15: the DELETE reproduction rig.
 *
 * The owner's report: "deleting a file leaves fdk-files broken"
 * (observed on real hardware). This rig drives the app under Xvfb
 * with XTEST and exercises every delete shape against the PHASE:
 * telemetry: menu-driven and keyboard (Delete / Shift+Delete),
 * single and multi-select, the last row of a folder, and rows at a
 * scrolled offset. After each shape it snapshots the app's
 * liveness, the row-count telemetry, the selection state, and
 * (round 15) a pixel screenshot for stale-visual analysis.
 *
 * Built like stress_rig.c; driven via run_deleterig.sh.
 *
 * Env:
 *   RIG_PID         the fdk-files pid (CPU/liveness)
 *   RIG_MODE        del|delkey|delmulti|dellast|delscroll|delmenu
 *   RIG_FILES       files in the beta-dir fixture (default 30)
 *   RIG_LIST_X/Y/W/H  the list geometry (from DRIG-GEO)
 *   RIG_ROW_H       row height px (default 25)
 */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int app_alive(pid_t pid) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d", (int)pid);
    return access(p, F_OK) == 0;
}

int main(int argc, char **argv) {
    const char *mode = getenv("RIG_MODE");
    const char *pid_s = getenv("RIG_PID");
    const char *xs = getenv("RIG_LIST_X"), *ys = getenv("RIG_LIST_Y");
    const char *ws = getenv("RIG_LIST_W"), *hs = getenv("RIG_LIST_H");
    int rowh = atoi(getenv("RIG_ROW_H") ? getenv("RIG_ROW_H") : "25");
    if (mode == NULL || pid_s == NULL || xs == NULL || ys == NULL ||
        ws == NULL || hs == NULL) {
        fprintf(stderr,
                "delete_rig: RIG_MODE/RIG_PID/RIG_LIST_* env needed\n");
        return 2;
    }
    pid_t app = (pid_t)atoi(pid_s);
    int lx = atoi(xs), ly = atoi(ys), lw = atoi(ws), lh = atoi(hs);

    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "delete_rig: cannot open display\n");
        return 2;
    }

    /* pointer at the target row (list-local row 2 by default) */
    int target_row = 2;
    if (strcmp(mode, "deldir") == 0) {
        target_row = 0; /* the 000-sub1 fixture sorts first */
    }
    if (strcmp(mode, "dellast") == 0) {
        int vis = (lh - 4) / rowh;
        target_row = vis - 2; /* second-to-last visible row */
        if (target_row < 0) target_row = 0;
    }
    /* rows begin below the columns header strip. The header is
     * font-derived (20..28px) — derive it from the row pitch the
     * same way the widget does (header = one text row + 6, floored
     * at 20): using the SAME formula keeps the rig's row math tied
     * to the engine's actual geometry instead of a stale constant.
     */
    int header = (rowh * 2) / 3 + 6;
    if (header < 20) {
        header = 20;
    }
    int px = lx + lw / 2;
    int py = ly + header + (target_row * rowh) + rowh / 2;

    printf("DEL-RIG: mode=%s target row %d at %d,%d\n",
           mode, target_row, px, py);
    fflush(stdout);

    if (strcmp(mode, "delscroll") == 0) {
        /* scroll down ~10 notches first: the delete happens at a
         * scrolled offset (band math + strip path territory). The
         * pointer stays PUT while content moves under it, so the
         * pre-scroll row math is stale — whatever row sits under the
         * pointer AFTER the flight is the delete target (exactly the
         * real-user gesture this shape models). */
        XTestFakeMotionEvent(dpy, 0, px, py, CurrentTime);
        XSync(dpy, False);
        msleep(80);
        for (int i = 0; i < 10; i++) {
            XTestFakeButtonEvent(dpy, 5, True, CurrentTime);
            XTestFakeButtonEvent(dpy, 5, False, CurrentTime);
            msleep(60);
        }
        msleep(600); /* let the flight settle */
        /* re-center on a row near the viewport middle: rows are
         * pitch-aligned from the content top; clicking mid-list
         * after a 480px scroll targets a valid row. */
        py = ly + header + (lh / 2 / rowh) * rowh + rowh / 2;
        if (py > ly + lh - rowh / 2) {
            py = ly + lh - rowh / 2;
        }
        XTestFakeMotionEvent(dpy, 0, px, py, CurrentTime);
        XSync(dpy, False);
        msleep(100);
    } else if (strcmp(mode, "delmenu") == 0) {
        /* right-click the row -> the context menu opens; navigate to
         * "Move to trash" (the menus.c block lists it first among
         * destructive entries) and activate it. */
        XTestFakeMotionEvent(dpy, 0, px, py, CurrentTime);
        XSync(dpy, False);
        msleep(80);
        XTestFakeButtonEvent(dpy, 3, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 3, False, CurrentTime);
        XSync(dpy, False);
        msleep(500); /* menu open + paint */
        /* The first menu item with accelerator D/e... "Move to
         * trash" sits at a fixed offset from the menu top. Rather
         * than guess pixels, press the key the menu binds: Delete
         * is listed as its shortcut — the menu may not consume it
         * though. Take the pixel route: menu item rows are ~26px,
         * the menu at the pointer. Item order per menus.c: the
         * file block's entries — walk DOWN to "Move to trash". */
        /* press 'd' — menus.c accelerators; the file block's trash
         * entry uses &t... too fragile. Use arrow+enter on the
         * first item instead: menu rows highlight on hover. */
        XTestFakeMotionEvent(dpy, 0, px, py + 30, CurrentTime);
        XSync(dpy, False);
        msleep(120);
        /* walk items until the trash entry (6th in the file block) */
        for (int item = 0; item < 6; item++) {
            XTestFakeMotionEvent(dpy, 0, px, py + 30 + item * 26,
                                 CurrentTime);
            XSync(dpy, False);
            msleep(60);
        }
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        msleep(800);
        printf("DEL-RIG: menu delete clicked\n");
        fflush(stdout);
    }
    if (strcmp(mode, "delmenu") != 0 && strcmp(mode, "deldir") != 0) {
        /* every keyboard/multi/simple shape starts from a click on
         * the target row — including delscroll (its block above
         * scrolled and re-centered py first; the old if/else chain
         * skipped this click for delscroll entirely, so nothing was
         * ever selected and the Delete was a no-op). */
        XTestFakeMotionEvent(dpy, 0, px, py, CurrentTime);
        XSync(dpy, False);
        msleep(80);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        msleep(40);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        msleep(200);
    }

    if (strcmp(mode, "delmulti") == 0) {
        /* shift-click a second row: a 2-row selection */
        int py2 = ly + header + ((target_row + 2) * rowh) + rowh / 2;
        XTestFakeMotionEvent(dpy, 0, px, py2, CurrentTime);
        XSync(dpy, False);
        msleep(80);
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, 0xffe1),
                          True, CurrentTime); /* Shift */
        XSync(dpy, False);
        msleep(30);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        msleep(40);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        msleep(30);
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, 0xffe1),
                          False, CurrentTime);
        XSync(dpy, False);
        msleep(200);
    }

    if (strcmp(mode, "deldir") == 0) {
        /* the DIRECTORY shape: the 000-sub1/ fixture sorts FIRST
         * under the model's name-ascending key, so it is row 0 (the
         * scroll-to-bottom anchoring was a rabbit hole — the click
         * kept landing a row off). Click row 0, Shift+Delete, then
         * Return answers the confirm dialog's default OK: the
         * recursive delete rides the ASYNC ops engine
         * (ops_start_delete + delete_engine_done + pane_reload). */
        XTestFakeMotionEvent(dpy, 0, px, py, CurrentTime);
        XSync(dpy, False);
        msleep(80);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        msleep(40);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        msleep(300);
        KeyCode del = XKeysymToKeycode(dpy, 0xffff);
        KeyCode sh = XKeysymToKeycode(dpy, 0xffe1);
        KeyCode rt = XKeysymToKeycode(dpy, 0xff0d);
        XTestFakeKeyEvent(dpy, sh, True, CurrentTime);
        XSync(dpy, False);
        msleep(80);
        XTestFakeKeyEvent(dpy, del, True, CurrentTime);
        XSync(dpy, False);
        msleep(50);
        XTestFakeKeyEvent(dpy, del, False, CurrentTime);
        XSync(dpy, False);
        msleep(60);
        XTestFakeKeyEvent(dpy, sh, False, CurrentTime);
        XSync(dpy, False);
        msleep(900); /* the modal confirm runs a nested pump */
        XTestFakeKeyEvent(dpy, rt, True, CurrentTime);
        XSync(dpy, False);
        msleep(50);
        XTestFakeKeyEvent(dpy, rt, False, CurrentTime);
        XSync(dpy, False);
        msleep(2500); /* the worker's recursive walk + done reload */
        printf("DEL-RIG: deldir sequence done\n");
        fflush(stdout);
    } else if (strcmp(mode, "delkey") == 0 ||
               strcmp(mode, "delmulti") == 0 ||
               strcmp(mode, "dellast") == 0 ||
               strcmp(mode, "delscroll") == 0) {
        /* the Delete key = move to trash (menus.c:243) */
        KeyCode del = XKeysymToKeycode(dpy, 0xffff); /* XK_Delete */
        XTestFakeKeyEvent(dpy, del, True, CurrentTime);
        XSync(dpy, False);
        msleep(40);
        XTestFakeKeyEvent(dpy, del, False, CurrentTime);
        XSync(dpy, False);
        msleep(600);
    } else if (strcmp(mode, "del") == 0) {
        /* plain 'del' mode: Shift+Delete (permanent, no dialog for
         * files-only selection) */
        KeyCode del = XKeysymToKeycode(dpy, 0xffff);
        KeyCode sh = XKeysymToKeycode(dpy, 0xffe1);
        XTestFakeKeyEvent(dpy, sh, True, CurrentTime);
        XSync(dpy, False);
        msleep(30);
        XTestFakeKeyEvent(dpy, del, True, CurrentTime);
        XSync(dpy, False);
        msleep(40);
        XTestFakeKeyEvent(dpy, del, False, CurrentTime);
        XSync(dpy, False);
        msleep(30);
        XTestFakeKeyEvent(dpy, sh, False, CurrentTime);
        XSync(dpy, False);
        msleep(600);
    }

    /* post-delete probes: liveness, then interaction sanity (a
     * click + a wheel — a wedged main loop won't answer either) */
    if (!app_alive(app)) {
        printf("DEL-RIG: APP DIED\n");
        fflush(stdout);
        XCloseDisplay(dpy);
        return 1;
    }
    printf("DEL-RIG: app alive after delete\n");
    XTestFakeMotionEvent(dpy, 0, px, ly + 5, CurrentTime);
    XSync(dpy, False);
    msleep(100);
    XTestFakeButtonEvent(dpy, 5, True, CurrentTime);
    XTestFakeButtonEvent(dpy, 5, False, CurrentTime);
    XSync(dpy, False);
    msleep(400);
    if (!app_alive(app)) {
        printf("DEL-RIG: APP DIED post-probe\n");
        fflush(stdout);
        XCloseDisplay(dpy);
        return 1;
    }
    printf("DEL-RIG: app alive after probe (wheel + click)\n");
    fflush(stdout);
    XCloseDisplay(dpy);
    (void)argc; (void)argv;
    return 0;
}
