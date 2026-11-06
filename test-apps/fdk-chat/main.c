/* main.c — fdk-chat: a real IRC chat client on FDK.
 *
 * App B of the test-apps brief: it pushes the streaming textview
 * (append per message), the fd extension loop (the socket), links
 * (attrs + hit-test + open), notifications and tray, i18n, prefs,
 * a11y — and the IRC protocol itself. The engine layers (irc.c,
 * net.c, log.c) are unit-tested headless (chat_test.c).
 */
#include "chat_app.h"

#include "fdk/fdk_i18n.h"
#include "fdk/fdk_notify.h"
#include "fdk/fdk_tray.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

chat_app capp;

/* ---- conversations -------------------------------------------------- */

static void on_open_link(void *user, const char *href);

static const char *conv_display(const conversation *cv) {
    return cv->is_server ? ta_("*server") : cv->name;
}

conversation *chat_conv_by_name(const char *name) {
    for (size_t i = 0; i < capp.n_convs; i++) {
        if (strcmp(capp.convs[i].name, name) == 0) {
            return &capp.convs[i];
        }
    }
    return NULL;
}

int chat_notebook_page_of(const conversation *cv) {
    return (int)(cv - capp.convs);
}

conversation *chat_conv_open(const char *name, bool is_server,
                             bool is_pm) {
    conversation *cv = chat_conv_by_name(name);
    if (cv != NULL) {
        return cv;
    }
    if (capp.n_convs == CHAT_MAX_CONVS) {
        return NULL;
    }
    cv = &capp.convs[capp.n_convs++];
    memset(cv, 0, sizeof *cv);
    snprintf(cv->name, sizeof cv->name, "%s", name);
    cv->is_server = is_server;
    cv->is_pm = is_pm;
    fdk_widget *page = NULL;
    (void)fdk_box_create(capp.notebook, FDK_VERTICAL, &page);
    /* the log's TextView carries its own internal scrollview */
    cv->log = chat_log_create(page, capp.base.font_mono,
                              on_open_link, NULL);
    if (cv->log != NULL) {
        fdk_widget_set_expand(chat_log_widget(cv->log), true, true);
    }
    (void)fdk_notebook_append_page(capp.notebook, page,
                                   conv_display(cv));
    return cv;
}

void chat_conv_close(conversation *cv) {
    if (cv == NULL) {
        return;
    }
    int idx = chat_notebook_page_of(cv);
    /* remove the notebook page: the log dies with the widgets */
    (void)fdk_notebook_remove_page(capp.notebook, (size_t)idx);
    if (cv->log != NULL) {
        chat_log_destroy(cv->log);
        cv->log = NULL;
    }
    memmove(&capp.convs[idx], &capp.convs[idx + 1],
            (capp.n_convs - (size_t)idx - 1) *
                sizeof capp.convs[0]);
    capp.n_convs--;
    if ((size_t)capp.active >= capp.n_convs) {
        capp.active = (int)capp.n_convs - 1;
    }
    chat_refresh_userlist();
}

void chat_show_conv(int idx) {
    if (idx < 0 || (size_t)idx >= capp.n_convs) {
        return;
    }
    capp.active = idx;
    conversation *cv = &capp.convs[idx];
    cv->unread = false;
    cv->highlight = false;
    (void)fdk_notebook_set_current_page(capp.notebook,
                                        (size_t)idx);
    (void)fdk_notebook_set_page_label(capp.notebook, (size_t)idx,
                                      conv_display(cv));
    chat_set_topic(cv);
    chat_refresh_userlist();
}

void chat_note_unread(int idx, bool highlight) {
    if (idx < 0 || (size_t)idx >= capp.n_convs || idx == capp.active) {
        return;
    }
    conversation *cv = &capp.convs[idx];
    cv->unread = true;
    cv->highlight = cv->highlight || highlight;
    char label[IRC_CHAN_MAX + 4];
    snprintf(label, sizeof label, "%s%s",
             cv->highlight ? "●" : "•", conv_display(cv));
    (void)fdk_notebook_set_page_label(capp.notebook, (size_t)idx,
                                      label);
}

void chat_refresh_userlist(void) {
    if (capp.userlist == NULL) {
        return;
    }
    /* rebuild the list widget's rows for the active conversation */
    fdk_list_clear(capp.userlist);
    if (capp.active < 0 ||
        (size_t)capp.active >= capp.n_convs) {
        return;
    }
    conversation *cv = &capp.convs[capp.active];
    irc_chan *ch = irc_chan_find(&capp.irc, cv->name);
    if (ch == NULL) {
        return;
    }
    for (size_t i = 0; i < ch->n_users; i++) {
        (void)fdk_list_append(capp.userlist, ch->users[i].nick,
                              FDK_ROW_ICON_NONE);
    }
    fdk_widget_set_accessible_name(capp.userlist,
                                   ta_("Channel members"));
    char status[96];
    snprintf(status, sizeof status, ta_("%zu members"),
             ch->n_users);
    chat_ui_status("%s", status);
}

void chat_on_highlight(conversation *cv, const char *nick,
                       const char *body) {
    (void)cv;
    if (capp.notify == NULL) {
        return;
    }
    char summary[IRC_NICK_MAX + 32];
    snprintf(summary, sizeof summary, ta_("Mention from %s"),
             nick != NULL ? nick : "?");
    char body_line[256];
    snprintf(body_line, sizeof body_line, "%.240s", body);
    fdk_notify_request req;
    memset(&req, 0, sizeof req);
    req.app_name = "fdk-chat";
    req.summary = summary;
    req.body = body_line;
    req.urgency = FDK_NOTIFY_URGENCY_NORMAL;
    (void)fdk_notify_send((fdk_notify *)capp.notify, &req, NULL);
}

/* ---- the outgoing side ----------------------------------------------- */

void chat_send_line(const char *line) {
    if (capp.conn == NULL) {
        chat_ui_status(ta_("Not connected"));
        return;
    }
    if (!chat_send(capp.conn, line)) {
        chat_ui_status(ta_("Send failed — connection closing"));
    }
}

/* The entry's /commands (the IRC-client minimum). Returns true when
 * the text WAS a command. */
static bool chat_command(const char *text) {
    if (text[0] != '/') {
        return false;
    }
    char word[64];
    const char *rest = strchr(text, ' ');
    size_t wl = (rest != NULL) ? (size_t)(rest - text - 1)
                               : strlen(text + 1);
    if (wl >= sizeof word) {
        wl = sizeof word - 1;
    }
    memcpy(word, text + 1, wl);
    word[wl] = '\0';
    rest = (rest != NULL) ? rest + 1 : "";

    if (strcmp(word, "join") == 0 && rest[0] != '\0') {
        char chan[IRC_CHAN_MAX];
        snprintf(chan, sizeof chan, "%s", rest);
        chat_send_line(text + 1); /* "join #chan" is valid IRC */
        chat_conv_open(chan, false, false);
        return true;
    }
    if (strcmp(word, "part") == 0) {
        conversation *cv = (capp.active >= 0)
                               ? &capp.convs[capp.active]
                               : NULL;
        const char *target = rest[0] != '\0' ? rest
                             : (cv != NULL && !cv->is_server
                                    ? cv->name
                                    : NULL);
        if (target != NULL) {
            char line[IRC_CHAN_MAX + 8];
            snprintf(line, sizeof line, "part %s", target);
            chat_send_line(line);
        }
        return true;
    }
    if (strcmp(word, "me") == 0) {
        conversation *cv = (capp.active >= 0)
                               ? &capp.convs[capp.active]
                               : NULL;
        if (cv != NULL && !cv->is_server) {
            char line[IRC_LINE_MAX];
            snprintf(line, sizeof line, "PRIVMSG %s :\001ACTION "
                                        "%s\001",
                     cv->name, rest);
            chat_send_line(line);
            chat_log_append(cv->log, CHAT_MSG_ACTION,
                            capp.irc.nick, rest);
        }
        return true;
    }
    if (strcmp(word, "msg") == 0 || strcmp(word, "query") == 0) {
        char to[IRC_NICK_MAX];
        const char *sp = strchr(rest, ' ');
        if (sp == NULL) {
            return true;
        }
        size_t tl = (size_t)(sp - rest);
        if (tl >= sizeof to) {
            tl = sizeof to - 1;
        }
        memcpy(to, rest, tl);
        to[tl] = '\0';
        const char *body = sp + 1;
        char line[IRC_LINE_MAX];
        snprintf(line, sizeof line, "PRIVMSG %s :%s", to, body);
        chat_send_line(line);
        conversation *cv = chat_conv_open(to, false, true);
        if (cv != NULL) {
            chat_log_append(cv->log, CHAT_MSG_PRIVMSG,
                            capp.irc.nick, body);
        }
        return true;
    }
    if (strcmp(word, "nick") == 0 && rest[0] != '\0') {
        char line[IRC_NICK_MAX + 8];
        snprintf(line, sizeof line, "NICK %s", rest);
        chat_send_line(line);
        return true;
    }
    if (strcmp(word, "topic") == 0) {
        conversation *cv = (capp.active >= 0)
                               ? &capp.convs[capp.active]
                               : NULL;
        if (cv != NULL && !cv->is_server) {
            char line[IRC_TOPIC_MAX + IRC_CHAN_MAX];
            snprintf(line, sizeof line, "TOPIC %s :%s", cv->name,
                     rest);
            chat_send_line(line);
        }
        return true;
    }
    if (strcmp(word, "away") == 0) {
        if (rest[0] != '\0') {
            char line[256];
            snprintf(line, sizeof line, "AWAY :%s", rest);
            chat_send_line(line);
        } else {
            chat_send_line("AWAY");
        }
        return true;
    }
    if (strcmp(word, "quit") == 0) {
        char line[256];
        snprintf(line, sizeof line, "QUIT :%s",
                 rest[0] != '\0' ? rest : "fdk-chat out");
        chat_send_line(line);
        chat_quit();
        return true;
    }
    if (strcmp(word, "help") == 0) {
        conversation *cv = (capp.active >= 0)
                               ? &capp.convs[capp.active]
                               : NULL;
        static const char *const HELP[] = {
            "/join #chan — join a channel",
            "/part [#chan] — leave",
            "/me action — the /me form",
            "/msg nick text — a private line",
            "/nick name — change nick",
            "/topic text — set the channel topic",
            "/away [reason]",
            "/quit [reason]",
        };
        if (cv != NULL) {
            for (size_t i = 0; i < sizeof HELP / sizeof *HELP;
                 i++) {
                chat_log_sysline(cv->log, HELP[i]);
            }
        }
        return true;
    }
    return false; /* not a known command: send as text? NO —
                     an unknown /word is refused (the honest
                     client), the entry shows why */
}

void chat_entry_submit(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }
    /* history */
    if (capp.hist_count < CHAT_ENTRY_HISTORY) {
        snprintf(capp.history[capp.hist_count++],
                 sizeof capp.history[0], "%s", text);
    } else {
        memmove(capp.history, capp.history[1],
                (CHAT_ENTRY_HISTORY - 1) * sizeof capp.history[0]);
        snprintf(capp.history[CHAT_ENTRY_HISTORY - 1],
                 sizeof capp.history[0], "%s", text);
    }
    capp.hist_pos = capp.hist_count;

    if (text[0] == '/') {
        if (!chat_command(text)) {
            chat_ui_status(ta_("Unknown command: %s"), text);
        }
        return;
    }
    conversation *cv = (capp.active >= 0)
                           ? &capp.convs[capp.active]
                           : NULL;
    if (cv == NULL || cv->is_server) {
        chat_ui_status(ta_("Join a channel to talk"));
        return;
    }
    char line[IRC_LINE_MAX];
    snprintf(line, sizeof line, "PRIVMSG %s :%s", cv->name, text);
    chat_send_line(line);
    chat_log_append(cv->log, CHAT_MSG_PRIVMSG, capp.irc.nick, text);
}

/* ---- the IRC dispatch ------------------------------------------------- */

static void irc_line(void *user, const char *line) {
    (void)user;
    char buf[IRC_LINE_MAX];
    snprintf(buf, sizeof buf, "%s", line);
    irc_msg m;
    if (!irc_parse(buf, &m)) {
        return;
    }
    capp.msg_total++;

    if (strcmp(m.command, "PING") == 0) {
        char pong[IRC_LINE_MAX];
        snprintf(pong, sizeof pong, "PONG :%s",
                 m.trailing != NULL ? m.trailing : capp.server);
        chat_send_line(pong);
        return;
    }
    if (m.numeric >= 400 && m.numeric < 600 && m.trailing != NULL) {
        conversation *sv = chat_conv_open("*server", true, false);
        if (sv != NULL) {
            chat_log_sysline(sv->log, m.trailing);
        }
    }
    switch (m.numeric) {
    case 1: /* RPL_WELCOME */
        if (m.trailing != NULL) {
            snprintf(capp.irc.nick, sizeof capp.irc.nick, "%s",
                     m.n_params >= 1 && m.params[0] != NULL
                         ? m.params[0]
                         : capp.irc.nick);
            conversation *sv =
                chat_conv_open("*server", true, false);
            if (sv != NULL) {
                chat_log_sysline(sv->log, m.trailing);
            }
            chat_ui_status(ta_("Connected as %s"), capp.irc.nick);
        }
        return;
    case 5: /* RPL_ISUPPORT — the server's casemapping etc.; log */
        if (m.trailing != NULL) {
            conversation *sv =
                chat_conv_open("*server", true, false);
            if (sv != NULL) {
                chat_log_sysline(sv->log, m.trailing);
            }
        }
        return;
    case 433: { /* ERR_NICKNAMEINUSE: retry with a backtick */
        size_t nl = strlen(capp.irc.nick);
        if (nl + 2 < sizeof capp.irc.nick) {
            capp.irc.nick[nl] = '`';
            capp.irc.nick[nl + 1] = '\0';
        }
        char out[IRC_NICK_MAX + 8];
        snprintf(out, sizeof out, "NICK %s", capp.irc.nick);
        chat_send_line(out);
        return;
    }
    case 332: /* RPL_TOPIC */
        if (m.n_params >= 3) {
            irc_chan *ch = irc_chan_find(&capp.irc, m.params[1]);
            if (ch != NULL) {
                snprintf(ch->topic, sizeof ch->topic, "%s",
                         m.trailing != NULL ? m.trailing : "");
                conversation *cv = chat_conv_by_name(m.params[1]);
                if (cv == capp.convs + capp.active) {
                    chat_set_topic(cv);
                }
            }
        }
        return;
    case 353: /* RPL_NAMREPLY */
        if (m.n_params >= 4) {
            irc_chan *ch = irc_chan_join(&capp.irc, m.params[2]);
            if (ch != NULL) {
                irc_chan_names_add(ch, m.trailing);
            }
        }
        return;
    case 366: /* RPL_ENDOFNAMES */
        if (m.n_params >= 2) {
            chat_refresh_userlist();
            conversation *cv = chat_conv_by_name(m.params[1]);
            if (cv != NULL) {
                chat_set_topic(cv);
            }
        }
        return;
    default:
        break;
    }

    const char *from = (m.nick != NULL) ? m.nick : capp.server;
    if (strcmp(m.command, "PRIVMSG") == 0 && m.n_params >= 2) {
        const char *target = m.params[0];
        const char *body = (m.trailing != NULL) ? m.trailing : "";
        bool to_me = strcmp(target, capp.irc.nick) == 0;
        bool mention = irc_mentions(capp.irc.nick, body);
        conversation *cv = to_me ? chat_conv_by_name(from)
                                 : chat_conv_by_name(target);
        if (cv == NULL && to_me) {
            cv = chat_conv_open(from, false, true);
        }
        if (cv == NULL) {
            cv = chat_conv_open(target, false, false);
        }
        if (cv != NULL) {
            chat_log_append(cv->log,
                            mention ? CHAT_MSG_HIGHLIGHT
                                    : CHAT_MSG_PRIVMSG,
                            from, body);
            int idx = chat_notebook_page_of(cv);
            chat_note_unread(idx, mention);
            if (mention && idx != capp.active) {
                chat_on_highlight(cv, from, body);
            }
        }
        return;
    }
    if (strcmp(m.command, "JOIN") == 0 && m.trailing != NULL) {
        irc_chan *ch = irc_chan_join(&capp.irc, m.trailing);
        if (from == NULL) {
            return;
        }
        bool ours = strcmp(from, capp.irc.nick) == 0;
        if (ours) {
            conversation *cv =
                chat_conv_open(m.trailing, false, false);
            if (cv != NULL) {
                chat_show_conv(chat_notebook_page_of(cv));
            }
        } else if (ch != NULL) {
            irc_chan_user_add(ch, from);
            conversation *cv = chat_conv_by_name(m.trailing);
            if (cv != NULL) {
                chat_log_append(cv->log, CHAT_MSG_JOIN, from, "");
                chat_note_unread(chat_notebook_page_of(cv), false);
            }
        }
        chat_refresh_userlist();
        return;
    }
    if ((strcmp(m.command, "PART") == 0 ||
         strcmp(m.command, "KICK") == 0)) {
        const char *chan = m.params[0];
        const char *who = (strcmp(m.command, "KICK") == 0 &&
                           m.n_params >= 2)
                              ? m.params[1]
                              : from;
        irc_chan *ch = irc_chan_find(&capp.irc, chan);
        if (ch != NULL && who != NULL) {
            irc_chan_user_remove(ch, who);
        }
        if (from != NULL && who == from &&
            strcmp(from, capp.irc.nick) == 0) {
            conversation *cv = chat_conv_by_name(chan);
            if (cv != NULL) {
                chat_conv_close(cv);
            }
            irc_chan_part(&capp.irc, chan);
        } else {
            conversation *cv = chat_conv_by_name(chan);
            if (cv != NULL && who != NULL) {
                chat_log_append(cv->log,
                                strcmp(m.command, "KICK") == 0
                                    ? CHAT_MSG_PART
                                    : CHAT_MSG_PART,
                                who, "");
                chat_note_unread(chat_notebook_page_of(cv), false);
            }
        }
        chat_refresh_userlist();
        return;
    }
    if (strcmp(m.command, "QUIT") == 0 && from != NULL) {
        for (size_t i = 0; i < capp.irc.n_chans; i++) {
            irc_chan *ch = &capp.irc.chans[i];
            if (irc_chan_user_remove(ch, from)) {
                conversation *cv = chat_conv_by_name(ch->name);
                if (cv != NULL) {
                    chat_log_append(cv->log, CHAT_MSG_QUIT, from,
                                    m.trailing != NULL
                                        ? m.trailing
                                        : "");
                }
            }
        }
        chat_refresh_userlist();
        return;
    }
    if (strcmp(m.command, "NICK") == 0 && from != NULL) {
        const char *to = (m.trailing != NULL) ? m.trailing
                                              : (m.n_params >= 1
                                                     ? m.params[0]
                                                     : NULL);
        if (to == NULL) {
            return;
        }
        if (strcmp(from, capp.irc.nick) == 0) {
            snprintf(capp.irc.nick, sizeof capp.irc.nick, "%s",
                     to);
        }
        for (size_t i = 0; i < capp.irc.n_chans; i++) {
            irc_chan *ch = &capp.irc.chans[i];
            if (irc_chan_user_rename(ch, from, to)) {
                conversation *cv = chat_conv_by_name(ch->name);
                if (cv != NULL) {
                    chat_log_append(cv->log, CHAT_MSG_NICK, from,
                                    to);
                }
            }
        }
        chat_refresh_userlist();
        return;
    }
    if (strcmp(m.command, "TOPIC") == 0 && m.n_params >= 1) {
        irc_chan *ch = irc_chan_find(&capp.irc, m.params[0]);
        if (ch != NULL) {
            snprintf(ch->topic, sizeof ch->topic, "%s",
                     m.trailing != NULL ? m.trailing : "");
        }
        conversation *cv = chat_conv_by_name(m.params[0]);
        if (cv != NULL) {
            if (m.trailing != NULL) {
                chat_log_append(cv->log, CHAT_MSG_STATUS, NULL,
                                m.trailing);
            }
            if (cv == capp.convs + capp.active) {
                chat_set_topic(cv);
            }
        }
        return;
    }
    if (strcmp(m.command, "MODE") == 0) {
        /* v1: modes arrive, the user list keeps the NAMES view —
         * honest, logged */
        return;
    }
    if (strcmp(m.command, "ERROR") == 0 && m.trailing != NULL) {
        conversation *sv = chat_conv_open("*server", true, false);
        if (sv != NULL) {
            chat_log_sysline(sv->log, m.trailing);
        }
        return;
    }
}

/* ---- the connection states -------------------------------------------- */

static void conn_state(void *user, chat_state st, const char *detail) {
    (void)user;
    switch (st) {
    case CHAT_CONNECTING:
        chat_ui_status(ta_("Connecting to %s ..."), detail);
        break;
    case CHAT_CONNECTED: {
        char line[IRC_NICK_MAX + IRC_NICK_MAX + 128];
        snprintf(line, sizeof line,
                 "NICK %s", capp.want_nick);
        chat_send_line(line);
        snprintf(line, sizeof line, "USER %s 0 * :%s",
                 capp.username, capp.realname);
        chat_send_line(line);
        snprintf(capp.irc.nick, sizeof capp.irc.nick, "%s",
                 capp.want_nick);
        /* the boot joins (env or the session's channels) */
        if (capp.join[0] != '\0') {
            char joins[300];
            snprintf(joins, sizeof joins, "JOIN %s", capp.join);
            chat_send_line(joins);
        }
        chat_ui_status(ta_("Connected to %s"), detail);
        break;
    }
    case CHAT_CLOSED:
        chat_disconnect_now(detail);
        break;
    }
}

void chat_connect_now(void) {
    if (capp.conn != NULL) {
        return;
    }
    capp.conn = chat_connect(capp.base.ctx, capp.server, capp.port,
                             irc_line, conn_state, &capp);
    if (capp.conn == NULL) {
        chat_ui_status(ta_("Cannot resolve %s"), capp.server);
        conversation *sv = chat_conv_open("*server", true, false);
        if (sv != NULL) {
            char msg[256];
            snprintf(msg, sizeof msg, "cannot resolve %s:%d",
                     capp.server, capp.port);
            chat_log_sysline(sv->log, msg);
        }
    }
}

void chat_disconnect_now(const char *why) {
    if (capp.conn != NULL) {
        chat_disconnect(capp.conn);
        capp.conn = NULL;
    }
    conversation *sv = chat_conv_open("*server", true, false);
    if (sv != NULL) {
        char msg[256];
        snprintf(msg, sizeof msg, ta_("Disconnected — %s"),
                 why != NULL ? why : "closed");
        chat_log_sysline(sv->log, msg);
    }
    chat_ui_status(ta_("Disconnected — %s"),
                   why != NULL ? why : "closed");
}

void chat_quit(void) {
    capp.base.quit = true;
}

/* ---- boot -------------------------------------------------------------- */

static void boot_prefs(void) {
    char def_server[128];
    if (!ta_pref_str(&capp.base, "server", "irc.fdk.test",
                     def_server, sizeof def_server)) {
        snprintf(def_server, sizeof def_server, "irc.fdk.test");
    }
    snprintf(capp.server, sizeof capp.server, "%s", def_server);
    capp.port = ta_pref_int(&capp.base, "port", 6667);
    ta_pref_str(&capp.base, "nick", "dreamer", capp.want_nick,
                sizeof capp.want_nick);
    ta_pref_str(&capp.base, "username", "fdk", capp.username,
                sizeof capp.username);
    ta_pref_str(&capp.base, "realname", "Faded Dream chat",
                capp.realname, sizeof capp.realname);

    /* the rig path: env overrides (never persisted) */
    const char *env;
    if ((env = getenv("FDK_CHAT_HOST")) != NULL &&
        env[0] != '\0') {
        snprintf(capp.server, sizeof capp.server, "%s", env);
    }
    if ((env = getenv("FDK_CHAT_PORT")) != NULL) {
        capp.port = atoi(env);
    }
    if ((env = getenv("FDK_CHAT_NICK")) != NULL && env[0] != '\0') {
        snprintf(capp.want_nick, sizeof capp.want_nick, "%s", env);
    }
    if ((env = getenv("FDK_CHAT_JOIN")) != NULL && env[0] != '\0') {
        snprintf(capp.join, sizeof capp.join, "%s", env);
    }
    capp.autoconnect = getenv("FDK_CHAT_NOCONNECT") == NULL;
}

static void window_event(fdk_window *w, const fdk_event_data *ev,
                         void *user) {
    (void)w;
    (void)user;
    if (ev->type == FDK_EVENT_WINDOW_FOCUS) {
        capp.focused = ev->focus.focused != 0;
        if (capp.focused && capp.active >= 0) {
            conversation *cv = &capp.convs[capp.active];
            cv->unread = false;
            cv->highlight = false;
        }
    }
}

static void on_open_link(void *user, const char *href) {
    (void)user;
    if (href != NULL) {
        (void)fdk_desktop_open_uri(href);
    }
}


int main(void) {
    if (!ta_init(&capp.base, "fdk-chat")) {
        return 1;
    }
    irc_state_init(&capp.irc);
    capp.active = -1;
    boot_prefs();

    if (!ta_open(&capp.base, ta_("fdk-chat"), 1000, 680)) {
        return 1;
    }
    fdk_window_set_event_callback(capp.base.window, window_event,
                                  &capp);
    if (!chat_ui_build()) {
        return 1;
    }
    /* i18n: EN msgids + the RO catalog (the four-app standard) */
    (void)ta_i18n_load(&capp.base, PO_DIR, "ui.language");

    /* notifications + tray (fail-soft: no bus in the sandbox) */
    capp.notify = fdk_notify_open(capp.base.ctx, NULL);
    /* the tray: opened lazily after the window paints (the bus
     * handshakes race the boot otherwise — the §27 lesson) */

    const char *frames = getenv("FDK_TESTAPP_FRAMES");
    long limit = (frames != NULL) ? atol(frames) : 0;
    long frame = 0;
    bool connected_started = false;

    while (!capp.base.quit) {
        if (!ta_pump(&capp.base)) {
            break;
        }
        frame++;
        if (!connected_started && capp.autoconnect && frame > 3) {
            connected_started = true;
            chat_connect_now();
        }
        if (limit > 0 && frame == limit - 5) {
            /* the smoke dump: the active conversation's rendered
             * document + the member count, for the rig's asserts */
            conversation *cv = (capp.active >= 0 &&
                                capp.active < (int)capp.n_convs)
                                   ? &capp.convs[capp.active]
                                   : NULL;
            printf("=== CHAT DUMP BEGIN ===\n");
            if (cv != NULL && cv->log != NULL) {
                printf("%s", chat_log_text(cv->log));
            }
            printf("=== CHAT DUMP END ===\n");
            if (capp.userlist != NULL) {
                printf("=== USERS %zu ===\n",
                       fdk_list_row_count(capp.userlist));
            }
            printf("=== MSGS %ld ===\n", capp.msg_total);
            fflush(stdout);
        }
        if (limit > 0 && frame >= limit) {
            break;
        }
    }

    chat_disconnect_now("exit");
    if (capp.notify != NULL) {
        fdk_notify_close((fdk_notify *)capp.notify);
        capp.notify = NULL;
    }
    irc_state_destroy(&capp.irc);
    for (size_t i = 0; i < capp.n_convs; i++) {
        chat_log_destroy(capp.convs[i].log);
        capp.convs[i].log = NULL;
    }
    ta_close(&capp.base);
    chat_menus_shutdown();
    return 0;
}
