/* 17_desktop_services.c — the desktop-service API surface, live
 * (§23 follow-up: the "test the API, not a hardcoded daemon" ask).
 *
 * THE POSTURE THIS DEMO PROVES: FDK's desktop clients DISCOVER
 * what is actually running. Nothing here hardcodes a daemon name,
 * a bus address, or a tray host — the app opens each client, then
 * REPORTS what the desktop answered:
 *
 *   notifications  fdk_notify_open -> the status machine
 *                  (NO_BUS / CONNECTING / NO_DAEMON / READY), the
 *                  daemon's own identity from
 *                  GetServerInformation, and its capability list —
 *                  whatever daemon the session happens to run
 *                  (notifyd, dunst, GNOME Shell, ...). Buttons
 *                  send real notifications whose lifecycle events
 *                  print to the status line: sent ids, closed
 *                  reasons, invoked actions.
 *   tray           fdk_tray_item_open -> the StatusNotifierItem
 *                  export: the watcher is WATCHED (not assumed);
 *                  hosted state, activate/scroll events, and a
 *                  live-editing dbusmenu (add/remove/check rows)
 *                  print as they happen.
 *   toplevels      fdk_toplevel_list_open -> the foreign window
 *                  set, re-snapshotted on every change: the
 *                  taskbar substrate without a taskbar.
 *   session        fdk_session_open -> the session-manager lane,
 *                  the id round-trip printed (CREATED vs
 *                  RESTORED), the window joined.
 *
 * Every panel degrades OUT LOUD: on a bare Xvfb there is no bus,
 * no watcher, no session protocol — the demo shows those STATES,
 * which is exactly the API's contract (states, not errors).
 *
 * Run it on your real desktop session to see the same code paths
 * light up against real daemons.
 */

#include "example_window.h"

#include "fdk/fdk_notify.h"
#include "fdk/fdk_session.h"
#include "fdk/fdk_toplevel.h"
#include "fdk/fdk_tray.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static fdk_example ex;

/* ---- the shared log: a status line + a scrolling text view ---- */

static fdk_widget *g_log;
static char g_log_text[8192];
static size_t g_log_len;

static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_log_len += (size_t)vsnprintf(
        g_log_text + g_log_len, sizeof g_log_text - g_log_len, fmt,
        ap);
    va_end(ap);
    /* The headless smoke rig greps stdout (a GUI app's stdout is
     * otherwise quiet; this is the demo's own audit trail). */
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    g_log_len += (size_t)snprintf(g_log_text + g_log_len,
                                  sizeof g_log_text - g_log_len, "\n");
    if (g_log_len >= sizeof g_log_text - 256) {
        /* keep the tail: drop the head, find the next line break */
        size_t keep = g_log_len / 2;
        while (keep < g_log_len && g_log_text[keep] != '\n') {
            keep++;
        }
        memmove(g_log_text, g_log_text + keep, g_log_len - keep);
        g_log_len -= keep;
        g_log_text[g_log_len] = '\0';
    }
    fdk_textview_set_text(g_log, g_log_text);
    fdk_example_set_status(&ex, g_log_text);
}

/* ---- notifications -------------------------------------------------- */

static fdk_notify *g_notify;
static uint32_t g_last_note_id;

static const char *notify_status_name(fdk_notify_status s) {
    switch (s) {
    case FDK_NOTIFY_STATUS_NO_BUS:
        return "NO_BUS (no session bus reachable — a state)";
    case FDK_NOTIFY_STATUS_CONNECTING:
        return "CONNECTING (handshake in flight)";
    case FDK_NOTIFY_STATUS_NO_DAEMON:
        return "NO_DAEMON (bus up, no notification daemon)";
    case FDK_NOTIFY_STATUS_READY:
        return "READY (a daemon answered GetCapabilities)";
    }
    return "?";
}

static void notify_refresh_header(void) {
    if (g_notify == NULL) {
        return;
    }
    fdk_notify_status s = fdk_notify_get_status(g_notify);
    log_line("[notify] status: %s", notify_status_name(s));
    if (s == FDK_NOTIFY_STATUS_READY) {
        const char *name = fdk_notify_server_name(g_notify);
        const char *vendor = fdk_notify_server_vendor(g_notify);
        log_line("[notify] server: %s (vendor %s)",
                 name != NULL ? name : "(unanswered)",
                 vendor != NULL ? vendor : "(unanswered)");
        const char *const *caps = fdk_notify_capabilities(g_notify);
        if (caps != NULL) {
            log_line("[notify] capabilities:");
            for (size_t i = 0; caps[i] != NULL; i++) {
                log_line("          - %s", caps[i]);
            }
        }
    }
}

static int g_notify_reprobes;

static void on_notify_ready(fdk_notify *n, void *user) {
    (void)n; (void)user;
    log_line("[notify] on_ready: the daemon appeared");
    notify_refresh_header();
    /* GetServerInformation often answers AFTER on_ready fires —
     * re-print for the next ~3 s so the identity lands too. */
    g_notify_reprobes = 12;
}

static void on_notify_sent(fdk_notify *n, uint32_t id, void *note_user,
                           void *user) {
    (void)n; (void)user;
    log_line("[notify] on_sent: id %u accepted (note_user=%s)", id,
             note_user == (void *)1 ? "the-simple-one"
                                    : note_user == (void *)2
                                          ? "the-actions-one"
                                          : "?");
}

static void on_notify_failed(fdk_notify *n, void *note_user,
                             const char *error, void *user) {
    (void)n; (void)user;
    log_line("[notify] on_failed: note_user=%p reason=%s", note_user,
             error != NULL ? error : "(null)");
}

static void on_notify_closed(fdk_notify *n, uint32_t id, uint32_t reason,
                             void *note_user, void *user) {
    (void)n; (void)user;
    const char *why = reason == FDK_NOTIFY_CLOSED_EXPIRED   ? "expired"
                      : reason == FDK_NOTIFY_CLOSED_DISMISSED ? "dismissed"
                      : reason == FDK_NOTIFY_CLOSED_REQUEST
                          ? "closed-by-request"
                          : "unknown";
    log_line("[notify] on_closed: id %u (%s) note_user=%p", id, why,
             note_user);
}

static void on_notify_action(fdk_notify *n, uint32_t id, const char *action,
                             void *note_user, void *user) {
    (void)n; (void)user;
    log_line("[notify] on_action: id %u action='%s' note_user=%p", id,
             action, note_user);
}

/* The 250 ms repeating re-probe: the daemon-identity fields answer
 * after on_ready; this lands them in the report. */
static void reprobe_tick(fdk_timer *timer, void *user) {
    (void)timer; (void)user;
    if (g_notify_reprobes > 0) {
        g_notify_reprobes--;
        notify_refresh_header();
    }
}

static void notify_reprobe(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    notify_refresh_header();
}

static void send_simple(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_notify == NULL) {
        return;
    }
    fdk_notify_request req;
    memset(&req, 0, sizeof req);
    req.app_name = "FDK example 17";
    req.summary = "The plain one";
    req.body = "No actions, normal urgency, daemon-default "
               "timeout. Its whole lifecycle prints to the log.";
    req.urgency = FDK_NOTIFY_URGENCY_NORMAL;
    req.timeout_ms = -1;
    int accepted = fdk_notify_send(g_notify, &req, (void *)1);
    log_line("[notify] send(simple): %s",
             accepted ? "queued/accepted" : "malformed (no summary)");
}

static void send_with_actions(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_notify == NULL) {
        return;
    }
    const char *actions[] = {"default", "Open the thing",
                             "snooze",  "Snooze 10 min", NULL};
    fdk_notify_request req;
    memset(&req, 0, sizeof req);
    req.app_name = "FDK example 17";
    req.summary = "The actionable one";
    req.body = "<b>Markup</b> in the body, two actions, low "
               "urgency, an explicit 8 s timeout.";
    req.actions = actions;
    req.urgency = FDK_NOTIFY_URGENCY_LOW;
    req.timeout_ms = 8000;
    req.category = "im.received";
    int accepted = fdk_notify_send(g_notify, &req, (void *)2);
    log_line("[notify] send(actions): %s",
             accepted ? "queued/accepted" : "malformed");
}

static void send_replace(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_notify == NULL || g_last_note_id == 0) {
        log_line("[notify] replace: no earlier id yet — send one first");
        return;
    }
    fdk_notify_request req;
    memset(&req, 0, sizeof req);
    req.app_name = "FDK example 17";
    req.replaces_id = g_last_note_id;
    req.summary = "The replaced one";
    req.body = "Same id, new content — the replaces_id path.";
    req.urgency = FDK_NOTIFY_URGENCY_CRITICAL;
    (void)fdk_notify_send(g_notify, &req, (void *)3);
    log_line("[notify] send(replaces id %u)", g_last_note_id);
}

static void withdraw_last(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_notify == NULL || g_last_note_id == 0) {
        log_line("[notify] withdraw: nothing to withdraw");
        return;
    }
    fdk_notify_withdraw(g_notify, g_last_note_id);
    log_line("[notify] withdraw(id %u) requested", g_last_note_id);
}

/* ---- the tray item ---------------------------------------------------- */

static fdk_tray_item *g_item;
static fdk_tray_menu *g_menu;
static fdk_widget *g_tray_state;

static void tray_refresh_state(void) {
    if (g_tray_state != NULL) {
        fdk_label_set_text(
            g_tray_state, fdk_tray_item_hosted(g_item)
                              ? "hosted: YES (a watcher holds our "
                                "registration)"
                              : "hosted: no (no StatusNotifier "
                                "Watcher on the bus)");
    }
}

static void on_tray_host(fdk_tray_item *item, int hosted, void *user) {
    (void)item; (void)user;
    log_line("[tray] on_host: %s",
             hosted ? "a watcher registered us — we are IN a tray"
                    : "the watcher vanished — waiting again");
    tray_refresh_state();
}

static void on_tray_activate(fdk_tray_item *item, int secondary,
                             int32_t x, int32_t y, void *user) {
    (void)item; (void)user;
    log_line("[tray] on_activate: %s click at %d,%d",
             secondary ? "secondary" : "primary", x, y);
    /* Cycle the status as the visible response: ACTIVE ->
     * NEEDS_ATTENTION -> PASSIVE -> ACTIVE. */
    static fdk_tray_status next = FDK_TRAY_STATUS_NEEDS_ATTENTION;
    fdk_tray_item_set_status(g_item, next);
    log_line("[tray] status -> %d", next);
    next = next == FDK_TRAY_STATUS_ACTIVE
               ? FDK_TRAY_STATUS_NEEDS_ATTENTION
               : next == FDK_TRAY_STATUS_NEEDS_ATTENTION
                     ? FDK_TRAY_STATUS_PASSIVE
                     : FDK_TRAY_STATUS_ACTIVE;
}

static void on_tray_scroll(fdk_tray_item *item, int32_t dx, int32_t dy,
                           void *user) {
    (void)item; (void)user;
    log_line("[tray] on_scroll: dx=%d dy=%d", dx, dy);
}

static void on_menu_click(fdk_tray_menu *menu, uint32_t id, void *user) {
    (void)menu; (void)user;
    log_line("[tray] menu clicked: node %u", id);
}

static void on_menu_show(fdk_tray_menu *menu, uint32_t parent, void *user) {
    (void)menu; (void)user;
    log_line("[tray] about-to-show: subtree under %u", parent);
}

static void tray_grow_menu(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_menu == NULL) {
        return;
    }
    static int counter = 1;
    char label[48];
    snprintf(label, sizeof label, "_Live row %d", counter++);
    uint32_t id = fdk_tray_menu_add(g_menu, 0, label);
    log_line("[tray] menu add '%s' -> node %u (revision now %u, "
             "%zu nodes)",
             label, id, fdk_tray_menu_revision(g_menu),
             fdk_tray_menu_node_count(g_menu));
}

static void tray_toggle_check(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_menu == NULL) {
        return;
    }
    static bool on;
    static uint32_t check_id;
    if (check_id == 0) {
        check_id = fdk_tray_menu_add(g_menu, 0, "_Toggle me");
    }
    on = !on;
    fdk_tray_menu_set_check(g_menu, check_id, FDK_TRAY_MENU_CHECK_CHECK,
                            on);
    log_line("[tray] menu check node %u -> %s (revision %u)", check_id,
             on ? "checked" : "unchecked",
             fdk_tray_menu_revision(g_menu));
}

static void tray_attention(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_item == NULL) {
        return;
    }
    fdk_tray_item_set_status(g_item, FDK_TRAY_STATUS_NEEDS_ATTENTION);
    fdk_tray_item_set_attention_icon_name(g_item, "dialog-warning");
    log_line("[tray] status NEEDS_ATTENTION + attention icon set");
}

static void tray_tooltip(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_item == NULL) {
        return;
    }
    fdk_tray_item_set_tooltip(g_item, "dialog-information",
                              "FDK example 17",
                              "The tray-item API: watcher discovery, "
                              "live menu edits, status cycling.");
    log_line("[tray] tooltip set");
}

/* ---- the foreign toplevel list ---------------------------------------- */

static fdk_toplevel_list *g_toplevels;
static fdk_widget *g_top_summary;

static void toplevel_refresh(void) {
    if (g_top_summary == NULL) {
        return;
    }
    fdk_u32 n = fdk_toplevel_count(g_toplevels);
    char head[96];
    snprintf(head, sizeof head, "%u toplevel(s) on this desktop:",
             n);
    fdk_label_set_text(g_top_summary, head);
    log_line("[toplevel] snapshot: %u window(s)", n);
    for (fdk_u32 i = 0; i < n && i < 8; i++) {
        fdk_toplevel_info info;
        memset(&info, 0, sizeof info);
        info.struct_size = sizeof info;
        if (fdk_ok(fdk_toplevel_get_info(g_toplevels, i, &info))) {
            log_line("[toplevel]   %u: '%.48s' app '%.24s' states "
                     "%s%s%s%s",
                     i, info.title, info.app_id,
                     info.states & FDK_TOPLEVEL_MAXIMIZED ? "max " : "",
                     info.states & FDK_TOPLEVEL_MINIMIZED ? "min " : "",
                     info.states & FDK_TOPLEVEL_FULLSCREEN ? "full " : "",
                     info.states & FDK_TOPLEVEL_ACTIVATED ? "focus"
                                                          : "-");
        }
    }
}

static void on_toplevels_changed(fdk_toplevel_list *list, void *user) {
    (void)list; (void)user;
    log_line("[toplevel] on_change: the set changed — re-snapshot");
    toplevel_refresh();
}

static void toplevel_reprobe(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    toplevel_refresh();
}

static void toplevel_activate_first(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (fdk_toplevel_count(g_toplevels) == 0) {
        log_line("[toplevel] activate: the list is empty here");
        return;
    }
    fdk_result r = fdk_toplevel_activate(g_toplevels, 0);
    log_line("[toplevel] activate(0): %s",
             fdk_ok(r) ? "requested" : "refused/unsupported");
}

/* ---- the session ------------------------------------------------------- */

static fdk_session *g_session;
static fdk_widget *g_session_state;

static void session_refresh(void) {
    if (g_session_state == NULL) {
        return;
    }
    if (g_session == NULL) {
        fdk_label_set_text(g_session_state,
                           "session: none (this backend offers no "
                           "session protocol — the documented "
                           "degradation)");
        return;
    }
    fdk_session_state s = fdk_session_get_state(g_session);
    const char *id = fdk_session_id(g_session);
    char buf[192];
    snprintf(buf, sizeof buf,
             "session: %s%s%s",
             s == FDK_SESSION_STATE_PENDING   ? "PENDING"
             : s == FDK_SESSION_STATE_CREATED ? "CREATED (persist this id)"
             : s == FDK_SESSION_STATE_RESTORED ? "RESTORED"
                                               : "REPLACED",
             id != NULL ? " id=" : " (no id yet)",
             id != NULL ? id : "");
    fdk_label_set_text(g_session_state, buf);
}

static void on_session_notify(fdk_session *session, void *user) {
    (void)user;
    fdk_session_state s = fdk_session_get_state(session);
    log_line("[session] notify: %s",
             s == FDK_SESSION_STATE_CREATED   ? "CREATED"
             : s == FDK_SESSION_STATE_RESTORED ? "RESTORED"
                                               : "REPLACED");
    session_refresh();
}

/* ---- main ---------------------------------------------------------------- */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "17")) {
        return 1;
    }
    if (!fdk_example_open(&ex, ctx, "17",
                          "desktop services (the live API)", 720,
                          620)) {
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_font *font = ex.ui;

    /* ---- the notifications panel ---- */
    fdk_widget *row1 = NULL;
    (void)fdk_box_create(ex.content, FDK_HORIZONTAL, &row1);
    fdk_widget *b = NULL;
    (void)fdk_button_create(row1, font, "Send simple", &b);
    fdk_button_set_on_activate(b, send_simple, NULL);
    (void)fdk_button_create(row1, font, "Send with actions", &b);
    fdk_button_set_on_activate(b, send_with_actions, NULL);
    (void)fdk_button_create(row1, font, "Replace last", &b);
    fdk_button_set_on_activate(b, send_replace, NULL);
    (void)fdk_button_create(row1, font, "Withdraw last", &b);
    fdk_button_set_on_activate(b, withdraw_last, NULL);
    (void)fdk_button_create(row1, font, "Re-probe", &b);
    fdk_button_set_on_activate(b, notify_reprobe, NULL);

    fdk_notify_callbacks nc;
    memset(&nc, 0, sizeof nc);
    nc.on_ready = on_notify_ready;
    nc.on_sent = on_notify_sent;
    nc.on_failed = on_notify_failed;
    nc.on_closed = on_notify_closed;
    nc.on_action = on_notify_action;
    g_notify = fdk_notify_open(ex.ctx, &nc);
    log_line("[notify] opened — discovering what the desktop runs");

    /* ---- the tray panel ---- */
    fdk_widget *row2 = NULL;
    (void)fdk_box_create(ex.content, FDK_HORIZONTAL, &row2);
    (void)fdk_button_create(row2, font, "Grow menu", &b);
    fdk_button_set_on_activate(b, tray_grow_menu, NULL);
    (void)fdk_button_create(row2, font, "Toggle check", &b);
    fdk_button_set_on_activate(b, tray_toggle_check, NULL);
    (void)fdk_button_create(row2, font, "Attention", &b);
    fdk_button_set_on_activate(b, tray_attention, NULL);
    (void)fdk_button_create(row2, font, "Set tooltip", &b);
    fdk_button_set_on_activate(b, tray_tooltip, NULL);

    g_tray_state = NULL;
    (void)fdk_label_create(ex.content, font, "", &g_tray_state);

    fdk_tray_item_callbacks tc;
    memset(&tc, 0, sizeof tc);
    tc.on_host = on_tray_host;
    tc.on_activate = on_tray_activate;
    tc.on_scroll = on_tray_scroll;
    fdk_tray_item_options opts;
    memset(&opts, 0, sizeof opts);
    opts.id = "org-fdk-example17";
    opts.title = "FDK example 17";
    opts.category = "ApplicationStatus";
    g_item = fdk_tray_item_open(ex.ctx, &opts, &tc, NULL);
    g_menu = fdk_tray_menu_create();
    (void)fdk_tray_menu_add(g_menu, 0, "_About FDK example 17");
    (void)fdk_tray_menu_add_separator(g_menu, 0);
    (void)fdk_tray_menu_add(g_menu, 0, "_Quit (host side demo row)");
    fdk_tray_menu_on_click(g_menu, on_menu_click, NULL);
    fdk_tray_menu_on_about_to_show(g_menu, on_menu_show, NULL);
    fdk_tray_item_set_menu(g_item, g_menu);
    fdk_tray_item_set_icon_name(g_item, "dialog-information");
    fdk_tray_item_set_status(g_item, FDK_TRAY_STATUS_ACTIVE);
    log_line("[tray] item opened, menu attached (%zu nodes) — "
             "watching for a StatusNotifier Watcher",
             fdk_tray_menu_node_count(g_menu));
    tray_refresh_state();

    /* ---- the toplevel panel ---- */
    fdk_widget *row3 = NULL;
    (void)fdk_box_create(ex.content, FDK_HORIZONTAL, &row3);
    g_top_summary = NULL;
    (void)fdk_label_create(ex.content, font, "", &g_top_summary);
    (void)fdk_button_create(row3, font, "Re-snapshot", &b);
    fdk_button_set_on_activate(b, toplevel_reprobe, NULL);
    (void)fdk_button_create(row3, font, "Activate [0]", &b);
    fdk_button_set_on_activate(b, toplevel_activate_first, NULL);
    g_toplevels =
        fdk_toplevel_list_open(ex.ctx, on_toplevels_changed, NULL);
    toplevel_refresh();

    /* ---- the session panel ---- */
    g_session_state = NULL;
    (void)fdk_label_create(ex.content, font, "", &g_session_state);
    g_session = fdk_session_open(ex.ctx, FDK_SESSION_REASON_LAUNCH, NULL,
                                 on_session_notify, NULL);
    if (g_session != NULL && ex.window != NULL) {
        (void)fdk_window_join_session(ex.window, g_session,
                                      "example-17-window");
        log_line("[session] opened (LAUNCH) — window joined; the "
                 "id prints when the compositor answers");
    } else {
        log_line("[session] open returned NULL — no session "
                 "mechanism on this backend (the documented "
                 "degradation)");
    }
    session_refresh();

    /* ---- the log pane ---- */
    g_log = NULL;
    (void)fdk_textview_create(ex.content, font, &g_log);
    fdk_textview_set_text(g_log, g_log_text);

    fdk_timer *reprobe = fdk_timer_add(ex.ctx, 250, true,
                                       reprobe_tick, NULL);

    fdk_example_run(&ex);

    if (reprobe != NULL) {
        fdk_timer_remove(reprobe);
    }
    if (g_session != NULL) {
        fdk_session_destroy(g_session);
    }
    if (g_toplevels != NULL) {
        fdk_toplevel_list_close(g_toplevels);
    }
    if (g_item != NULL) {
        fdk_tray_item_close(g_item);
    }
    if (g_menu != NULL) {
        fdk_tray_menu_close(g_menu);
    }
    if (g_notify != NULL) {
        fdk_notify_close(g_notify);
    }
    fdk_example_close(&ex);
    return 0;
}
