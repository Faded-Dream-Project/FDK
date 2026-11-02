/* §29/G-11: the SELF-DROP — a drag started by an FDK window and
 * released onto THE SAME window (the drag-between-panes gesture of
 * any split view). The §28/§29 rig found it dead: the drop gesture
 * arrived with an EMPTY payload (drag_done said SUCCEEDED, uris
 * NULL). Root cause: the receiver's drop fetch reads the XdndSelection
 * whose owner is THIS SAME CONNECTION — the SelectionRequest that
 * must be served for the notify to ever arrive sits in OUR OWN
 * queue, and the read's wait loop never dispatched it. The fix (in
 * wait_selection_notify_on) pumps queued SelectionRequests and
 * helper ClientMessages while waiting. */
static struct {
    bool armed;
    bool drag_started_ok;
    bool reported;
    fdk_drag_status status;
    bool dropped;
    bool drop_has_uris;
    size_t drop_uri_count;
} g11;

static void g11_done(fdk_drag_status status, void *user);

static void g11_window_event(fdk_window *w, const fdk_event_data *ev,
                             void *user) {
    (void)user;
    if (ev->type == FDK_EVENT_POINTER_BUTTON_DOWN) {
        g11.armed = true;
    } else if (ev->type == FDK_EVENT_POINTER_MOTION && g11.armed) {
        g11.armed = false;
        const char *uris[2] = {"/etc/hostname", "/etc/os-release"};
        fdk_result r = fdk_drag_begin(
            w, FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST,
            "self drop payload", uris, 2, g11_done, NULL);
        g11.drag_started_ok = fdk_ok(r);
    } else if (ev->type == FDK_EVENT_DRAG_DROP) {
        g11.dropped = true;
        g11.drop_has_uris = ev->drag.uris != NULL;
        g11.drop_uri_count = ev->drag.uri_count;
    }
}

static void g11_done(fdk_drag_status status, void *user) {
    (void)user;
    g11.status = status;
    g11.reported = true;
}

static void test_dnd_self_drop_gui(void) {
    const char *xtest = getenv("FDK_XTEST_BIN");
    if (xtest == NULL) {
        xtest = "/tmp/xtest_driver";
    }
    const char *sink_bin = getenv("FDK_XDND_SINK_BIN");
    if (sink_bin == NULL) {
        sink_bin = "/home/z/my-project/scripts/xdnd_sink";
    }
    if (access(xtest, X_OK) != 0 || access(sink_bin, X_OK) != 0) {
        printf("[skip] dnd self-drop: %s or %s not built (interop "
               "rig)\n", xtest, sink_bin);
        return;
    }
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "g11 self-drop",
                                 .width = 320, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_set_event_callback(win, g11_window_event, NULL);
    fdk_window_set_drop_formats(win, FDK_DRAG_FORMAT_URI_LIST);
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 50);

    unsigned long xid = fdk_window_xid(win);

    /* window-local (60,60) -> root coords on a private display */
    int sx = 0, sy = 0;
    {
        Display *td = XOpenDisplay(NULL);
        assert(td != NULL);
        Window child_ret;
        int wx, wy;
        (void)XTranslateCoordinates(td, (Window)xid,
                                    DefaultRootWindow(td), 0, 0,
                                    &wx, &wy, &child_ret);
        XCloseDisplay(td);
        sx = -wx + 60;
        sy = -wy + 60;
    }

    memset(&g11, 0, sizeof g11);
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "%s 0x%lx m:%d,%d w:200 d w:200 m:%d,%d w:200 u w:500",
             xtest, xid, sx, sy, sx + 120, sy + 40);
    int rc = system(cmd);
    (void)rc;
    for (int i = 0; i < 120 && !(g11.reported && g11.dropped); i++) {
        (void)fdk_pump_events(ctx, 25);
    }
    assert(g11.drag_started_ok);
    assert(g11.dropped);
    assert(g11.drop_has_uris);
    assert(g11.drop_uri_count == 2);
    assert(g11.reported);
    assert(g11.status == FDK_DRAG_SUCCEEDED);

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] dnd self-drop: the gesture delivered the URI "
           "payload to its own window (drag_done SUCCEEDED)\n");
}

/* Source side: FDK drags into a raw-Xlib sink window, driven by the
 * REAL pointer through XTEST. */
static struct {
    bool armed;
    bool drag_started_ok;
    bool reported;
    fdk_drag_status status;
