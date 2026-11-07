/* xdnd_sink.c — a RAW-Xlib XDND drop target (an "external
 * application" for FDK's drag-SOURCE rigs).
 *
 * No FDK, no toolkit: a plain mapped window speaking the XDND spec,
 * like a real editor's window. It advertises XdndAware, accepts
 * every drag that offers UTF8_STRING or text/uri-list, and prints
 * what it received — the interop proof for FDK->other-application
 * drags.
 *
 * Usage: xdnd_sink <x> <y> <w> <h>
 * Prints:
 *   SINK: ready <xid>
 *   SINK: entered
 *   SINK: status sent (accept=1)
 *   SINK: dropped
 *   SINK: payload <uri|text> <bytes> <content>
 *   SINK: finished sent
 * Runs until stdin closes or SIGTERM.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_term(int sig) { (void)sig; g_stop = 1; }

static Atom a_xdnd_aware, a_xdnd_enter, a_xdnd_position, a_xdnd_status,
    a_xdnd_leave, a_xdnd_drop, a_xdnd_finished, a_xdnd_action_copy,
    a_xdnd_selection, a_utf8, a_uri_list, a_prop;

static Window g_source = 0;
static int g_version = 5;

static void send_clientmsg(Display *dpy, Window to, Atom type, long l0,
                           long l1, long l2, long l3, long l4) {
    XClientMessageEvent m;
    memset(&m, 0, sizeof(m));
    m.type = ClientMessage;
    m.display = dpy;
    m.window = to;
    m.message_type = type;
    m.format = 32;
    m.data.l[0] = l0;
    m.data.l[1] = l1;
    m.data.l[2] = l2;
    m.data.l[3] = l3;
    m.data.l[4] = l4;
    XSendEvent(dpy, to, False, NoEventMask, (XEvent *)&m);
    XFlush(dpy);
}

/* One-paragraph line printer: payload bytes with newlines -> spaces. */
static void print_payload(const char *kind, const unsigned char *data,
                          unsigned long len) {
    printf("SINK: payload %s %lu \"", kind, len);
    for (unsigned long i = 0; i < len && i < 400; i++) {
        unsigned char c = data[i];
        putchar(c == '\n' || c == '\r' ? ' ' : c);
    }
    printf("\"\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <x> <y> <w> <h>\n", argv[0]);
        return 2;
    }
    int x = atoi(argv[1]), y = atoi(argv[2]), w = atoi(argv[3]),
        h = atoi(argv[4]);

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "SINK: no display\n");
        return 1;
    }
    a_xdnd_aware = XInternAtom(dpy, "XdndAware", False);
    a_xdnd_enter = XInternAtom(dpy, "XdndEnter", False);
    a_xdnd_position = XInternAtom(dpy, "XdndPosition", False);
    a_xdnd_status = XInternAtom(dpy, "XdndStatus", False);
    a_xdnd_leave = XInternAtom(dpy, "XdndLeave", False);
    a_xdnd_drop = XInternAtom(dpy, "XdndDrop", False);
    a_xdnd_finished = XInternAtom(dpy, "XdndFinished", False);
    a_xdnd_action_copy = XInternAtom(dpy, "XdndActionCopy", False);
    a_xdnd_selection = XInternAtom(dpy, "XdndSelection", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_uri_list = XInternAtom(dpy, "text/uri-list", False);
    a_prop = XInternAtom(dpy, "_XDND_SINK_PROP", False);

    Window root = DefaultRootWindow(dpy);
    Window win = XCreateSimpleWindow(dpy, root, x, y, w, h, 1, 0, 0xFFFFFF);
    Atom ver = 5;
    XChangeProperty(dpy, win, a_xdnd_aware, XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&ver, 1);
    XSelectInput(dpy, win,
                 StructureNotifyMask |
                     ButtonPressMask | ButtonReleaseMask);
    XMapWindow(dpy, win);
    XFlush(dpy);

    printf("SINK: ready 0x%lx\n", (unsigned long)win);
    fflush(stdout);

    while (!g_stop) {
        XEvent ev;
        while (XPending(dpy) > 0) {
            XNextEvent(dpy, &ev);
            if (ev.type == ClientMessage) {
                const XClientMessageEvent *m = &ev.xclient;
                if (m->message_type == a_xdnd_enter) {
                    g_source = (Window)m->data.l[0];
                    g_version = (int)((m->data.l[1] >> 24) & 0xFF);
                    if (g_version > 5) g_version = 5;
                    printf("SINK: entered\n");
                    fflush(stdout);
                } else if (m->message_type == a_xdnd_position) {
                    /* Accept anything: we read both known targets. */
                    send_clientmsg(dpy, g_source, a_xdnd_status,
                                   (long)win, 0x5 /* accept + want pos */,
                                   0, 0, 0);
                    printf("SINK: status sent (accept=1)\n");
                    fflush(stdout);
                } else if (m->message_type == a_xdnd_leave) {
                    printf("SINK: left\n");
                    fflush(stdout);
                    g_source = 0;
                } else if (m->message_type == a_xdnd_drop) {
                    printf("SINK: dropped\n");
                    fflush(stdout);
                    /* Convert BOTH targets; print what we get. */
                    unsigned long time =
                        (unsigned long)m->data.l[2];
                    const Atom wanted[2] = { a_uri_list, a_utf8 };
                    const char *names[2] = { "uri", "text" };
                    for (int t = 0; t < 2; t++) {
                        XConvertSelection(dpy, a_xdnd_selection,
                                          wanted[t], a_prop, win,
                                          (Time)time);
                        /* bounded wait for the notify */
                        for (int spin = 0; spin < 40; spin++) {
                            XEvent ne;
                            if (XCheckTypedEvent(dpy, SelectionNotify,
                                                 &ne)) {
                                if (ne.xselection.property != None) {
                                    Atom type = None;
                                    int fmt = 0;
                                    unsigned long n = 0, after = 0;
                                    unsigned char *data = NULL;
                                    if (XGetWindowProperty(
                                            dpy, win, a_prop, 0, 1 <<
                                                                     20,
                                            True, AnyPropertyType,
                                            &type, &fmt, &n, &after,
                                            &data) == Success &&
                                        data != NULL) {
                                        print_payload(names[t], data,
                                                      n);
                                        XFree(data);
                                    }
                                }
                                break;
                            }
                            usleep(25000);
                        }
                    }
                    send_clientmsg(dpy, g_source, a_xdnd_finished,
                                   (long)win, 0x1, 0, 0, 0);
                    printf("SINK: finished sent\n");
                    fflush(stdout);
                    g_source = 0;
                }
            }
        }
        struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&pfd, 1, 100);
    }
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    printf("SINK: done\n");
    return 0;
}
