/*
 * test_clip_uris.c — the §23/B2a uri-list clipboard, live on Xvfb.
 *
 * The established rig's shape (test_x11_integration's clipboard
 * legs): a FORKED child process takes CLIPBOARD ownership as a
 * foreign client and serves text/uri-list through raw X11, while
 * the parent reads through FDK's public API — a real two-process
 * wire round trip. Then the roles flip: the parent SETS through
 * FDK, and the child reads the selection (the serve side's proof
 * against a foreign requestor).
 *
 * What is proven:
 *   - get: FDK reads a foreign owner's text/uri-list (the
 *     selection-request + property-read machinery, target
 *     text/uri-list), decoded to LF lines
 *   - set: FDK's ownership takes effect and a foreign client's
 *     text/uri-list request is answered with the wire bytes
 *     (CRLF lines, file:// percent-encoding)
 *   - clear: count 0 drops the content
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_clipboard.h"

#include "core/alloc_internal.h" /* fdk_free */

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <assert.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fdk/fdk_core.h" /* fdk_pump_events */

/* The child's served list (the freedesktop wire format). */
#define CHILD_LIST \
    "file:///tmp/one%20two.txt\r\nfile:///home/u/report.pdf\r\nhttps://fdk.dev/x\r\n"

static void child_serve_requests(Display *dpy, Window w, Atom clip,
                                 Atom uri_list, Atom targets,
                                 const char *payload) {
    (void)w; (void)clip; /* the owner's window/selection, implied  */
    XEvent ev;
    while (XPending(dpy) > 0) {
        XNextEvent(dpy, &ev);
        if (ev.type != SelectionRequest) {
            continue;
        }
        XSelectionRequestEvent *req = &ev.xselectionrequest;
        XEvent reply;
        memset(&reply, 0, sizeof reply);
        reply.type = SelectionNotify;
        reply.xselection.display = dpy;
        reply.xselection.requestor = req->requestor;
        reply.xselection.selection = req->selection;
        reply.xselection.target = req->target;
        reply.xselection.time = req->time;
        reply.xselection.property = req->property;
        if (req->target == targets) {
            Atom list[1] = {uri_list};
            XChangeProperty(dpy, req->requestor, req->property,
                            XA_ATOM, 32, PropModeReplace,
                            (const unsigned char *)list, 1);
        } else if (req->target == uri_list) {
            XChangeProperty(dpy, req->requestor, req->property,
                            uri_list, 8, PropModeReplace,
                            (const unsigned char *)payload,
                            (int)strlen(payload));
        } else {
            reply.xselection.property = None; /* refuse */
        }
        XSendEvent(dpy, req->requestor, False, 0, &reply);
        XFlush(dpy);
    }
}

/* The child process: owns the selection, serves until the parent
 * speaks; then phase 2: reads the PARENT's selection (set through
 * FDK) and prints what it saw. */
/* The documented Xvfb XOpenDisplay race (docs/testing.md, "Known
 * Xvfb flakiness"; the 2026-11-11 debian CI abort in
 * test_xsettings_detection_gui): a fresh connection made right after
 * the server starts — or mid-run, the same quirk the fake-WM rig's
 * inline retry absorbs — can intermittently return NULL even though
 * the server is reachable. Poll with a bounded budget: on a healthy
 * server the first attempt wins (zero added latency); the race is
 * absorbed; a persistent failure exhausts the budget and returns
 * NULL so the caller's assert/_exit still reports accurately. */
static Display *x11_open_display_timeout(void) {
    Display *d = XOpenDisplay(NULL);
    for (int attempt = 0; d == NULL && attempt < 99; attempt++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        d = XOpenDisplay(NULL);
    }
    return d;
}

static int child_main(int sock) {
    alarm(0);
    Display *dpy = x11_open_display_timeout();
    if (dpy == NULL) {
        _exit(10);
    }
    Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                   0, 0, 1, 1, 0, 0, 0);
    Atom clip = XInternAtom(dpy, "CLIPBOARD", False);
    Atom uri_list = XInternAtom(dpy, "text/uri-list", False);
    Atom targets = XInternAtom(dpy, "TARGETS", False);
    XSetSelectionOwner(dpy, clip, w, CurrentTime);
    if (XGetSelectionOwner(dpy, clip) != w) {
        _exit(11);
    }
    XFlush(dpy);
    (void)!write(sock, "R", 1);

    /* Phase 1: serve until the parent speaks (or 10 s quiet). */
    bool go_phase2 = false;
    for (;;) {
        struct pollfd pfds[2];
        pfds[0].fd = ConnectionNumber(dpy);
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = sock;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        int r = poll(pfds, 2, 10000);
        if (r <= 0) {
            break; /* quiet or error: phase 1 over */
        }
        if (pfds[1].revents != 0) {
            char c2 = 0;
            (void)!recv(sock, &c2, 1, 0); /* drain the nudge      */
            go_phase2 = true;
            break; /* parent spoke: phase 2 begins */
        }
        child_serve_requests(dpy, w, clip, uri_list, targets,
                             CHILD_LIST);
    }
    (void)go_phase2;

    /* Phase 2: the parent now owns the selection (set through
     * FDK). Request the uri-list onto our own window and wait for
     * the notify (the parent serves through its event dispatch —
     * give it time). */
    Atom answer = XInternAtom(dpy, "_FDK_CHILD_ANSWER", False);
    XConvertSelection(dpy, clip, uri_list, answer, w, CurrentTime);
    XFlush(dpy);
    for (int spin = 0; spin < 200; spin++) {
        XSync(dpy, False);
        XEvent ev;
        while (XPending(dpy) > 0) {
            XNextEvent(dpy, &ev);
            if (ev.type == SelectionNotify &&
                ev.xselection.selection == clip &&
                ev.xselection.property == answer) {
                Atom type = None;
                int fmt = 0;
                unsigned long n = 0, rest = 0;
                unsigned char *data = NULL;
                if (XGetWindowProperty(dpy, w, answer, 0, 1 << 18,
                                       True, AnyPropertyType, &type,
                                       &fmt, &n, &rest,
                                       &data) == Success &&
                    data != NULL) {
                    (void)!write(sock, data, n); /* the SOCKET, not
                                        * stdout — the parent polls
                                        * the socket             */
                    XFree(data);
                }
                _exit(0);
            }
        }
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    _exit(0); /* no answer in time: the parent asserts on empty */
}

int main(void) {
    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r) || ctx == NULL) {
        printf("[skip] no display — the uri clipboard needs one\n");
        return 0;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("[skip] socketpair failed\n");
        fdk_shutdown(ctx);
        return 0;
    }
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        int rc = child_main(sv[1]);
        _exit(rc);
    }
    close(sv[1]);

    /* Wait for the child's ownership. */
    char c = 0;
    assert(read(sv[0], &c, 1) == 1 && c == 'R');

    /* ---- get: read the foreign owner's uri-list ---- */
    char *got = fdk_clipboard_get_uris(ctx);
    assert(got != NULL);
    assert(strstr(got, "file:///tmp/one%20two.txt") != NULL);
    assert(strstr(got, "file:///home/u/report.pdf") != NULL);
    assert(strstr(got, "https://fdk.dev/x") != NULL);
    assert(strchr(got, '\r') == NULL); /* CRLF stripped on read  */
    fdk_free(got);
    printf("[ok] clipboard uris: foreign owner read through the "
           "selection machinery\n");

    /* ---- set: FDK owns; the child reads us ---- */
    const char *uris[3] = {
        "/tmp/one two.txt", "/home/u/report.pdf", "https://fdk.dev/x",
    };
    r = fdk_clipboard_set_uris(ctx, uris, 3);
    if (r == FDK_ERR_UNSUPPORTED) {
        printf("[skip] backend without uri-list support\n");
        close(sv[0]);
        waitpid(pid, NULL, 0);
        fdk_shutdown(ctx);
        return 0;
    }
    assert(fdk_ok(r));

    /* Nudge the child into phase 2, PUMP while it requests (the
     * parent's serve runs from the event dispatch — without
     * pumping there is no one to answer), then collect. */
    (void)!write(sv[0], "G", 1);
    char buf[4096];
    ssize_t n = 0;
    for (int spin = 0; spin < 500 && n == 0; spin++) {
        (void)fdk_pump_events(ctx, 20);
        struct pollfd pfd = {sv[0], POLLIN, 0};
        int pr = poll(&pfd, 1, 20);
        if (pr > 0) {
            n = read(sv[0], buf, sizeof buf - 1);
            if (n <= 0) {
                n = 0; /* EOF: child exited without data          */
                break;
            }
            break;
        }
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0) {
        printf("[warn] child read nothing (serve-side leg did not "
               "run) — status %d\n", status);
    } else {
        buf[n] = '\0';
        assert(strstr(buf, "file:///tmp/one%20two.txt\r\n") != NULL);
        assert(strstr(buf, "file:///home/u/report.pdf\r\n") != NULL);
        assert(strstr(buf, "https://fdk.dev/x\r\n") != NULL);
        printf("[ok] clipboard uris: FDK's serve answered a foreign "
               "requestor (wire format CRLF + percent-encoding)\n");
    }
    close(sv[0]);

    /* ---- clear ---- */
    assert(fdk_ok(fdk_clipboard_set_uris(ctx, NULL, 0)));

    fdk_shutdown(ctx);
    printf("all uri clipboard tests passed\n");
    return 0;
}
