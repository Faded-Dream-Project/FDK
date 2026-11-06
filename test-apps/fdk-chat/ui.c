/* ui.c — the fdk-chat window (see chat_app.h).
 *
 * Layout: [menubar][ notebook | user sidebar ][topic][ entry ].
 * The notebook holds one page per conversation (server log,
 * channels, PMs); the sidebar is the active channel's member list;
 * the entry speaks /commands, Ctrl+Up/Down history and Tab nick
 * completion.
 */
#include "chat_app.h"

#include "fdk/fdk_a11y.h"
#include "fdk/fdk_dialog.h"
#include "fdk/fdk_i18n.h"

#include <stdarg.h>
#include <stdint.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- status / topic -------------------------------------------------- */

void chat_ui_status(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ta_status(&capp.base, "%s", buf);
}

void chat_set_topic(const conversation *cv) {
    if (capp.topic_label == NULL) {
        return;
    }
    char text[IRC_TOPIC_MAX + IRC_CHAN_MAX + 8];
    if (cv != NULL && cv->topic[0] != '\0') {
        snprintf(text, sizeof text, "%s — %s", cv->name, cv->topic);
    } else if (cv != NULL) {
        snprintf(text, sizeof text, "%s", cv->name);
    } else {
        snprintf(text, sizeof text, "%s", capp.server);
    }
    (void)fdk_label_set_text(capp.topic_label, text);
}

static const char *conv_label(const conversation *cv) {
    return cv->is_server ? ta_("*server") : cv->name;
}

/* ---- the entry: history + completion --------------------------------- */

static bool entry_key(fdk_widget *w, const fdk_widget_event *ev,
                      void *user) {
    (void)w;
    (void)user;
    if (ev->type != FDK_WIDGET_KEY_DOWN) {
        return false;
    }
    if (ev->key.scancode == FDK_KEY_UP &&
        (ev->key.modifiers & FDK_MOD_CTRL) != 0) {
        if (capp.hist_count > 0) {
            if (capp.hist_pos > 0) {
                capp.hist_pos--;
            }
            (void)fdk_entry_set_text(capp.entry,
                                     capp.history[capp.hist_pos]);
            (void)fdk_entry_set_cursor(capp.entry, (size_t)-1);
        }
        return true;
    }
    if (ev->key.scancode == FDK_KEY_DOWN &&
        (ev->key.modifiers & FDK_MOD_CTRL) != 0) {
        if (capp.hist_pos < capp.hist_count) {
            capp.hist_pos++;
            (void)fdk_entry_set_text(
                capp.entry,
                capp.hist_pos < capp.hist_count
                    ? capp.history[capp.hist_pos]
                    : "");
        }
        return true;
    }
    if (ev->key.scancode == FDK_KEY_TAB &&
        (ev->key.modifiers & ~(fdk_u32)FDK_MOD_SHIFT) == 0) {
        /* nick completion: the word before the caret grows to the
         * first member that prefix-matches */
        size_t caret = 0, anchor = 0;
        (void)fdk_entry_get_selection(capp.entry, &anchor, &caret);
        const char *text = fdk_entry_get_text(capp.entry);
        if (capp.active < 0 ||
            (size_t)capp.active >= capp.n_convs ||
            text == NULL || caret > strlen(text)) {
            return true; /* consumed, nothing to do */
        }
        conversation *cv = &capp.convs[capp.active];
        irc_chan *ch = irc_chan_find(&capp.irc, cv->name);
        if (ch == NULL || ch->n_users == 0) {
            return true;
        }
        size_t start = caret;
        while (start > 0 && text[start - 1] != ' ' &&
               text[start - 1] != ',' && text[start - 1] != ':') {
            start--;
        }
        size_t word_len = caret - start;
        for (size_t i = 0; i < ch->n_users; i++) {
            const char *nick = ch->users[i].nick;
            if (word_len > 0 &&
                (strlen(nick) < word_len ||
                 strncasecmp(nick, text + start, word_len) != 0)) {
                continue;
            }
            char repl[IRC_NICK_MAX + 4];
            if (start == 0 && word_len == 0) {
                snprintf(repl, sizeof repl, "%s: ", nick);
            } else {
                snprintf(repl, sizeof repl, "%s", nick);
            }
            char next[512];
            snprintf(next, sizeof next, "%.*s%s%s", (int)start,
                     text, repl, text + caret);
            (void)fdk_entry_set_text(capp.entry, next);
            (void)fdk_entry_set_cursor(capp.entry,
                                       start + strlen(repl));
            return true; /* keep the focus traversal off Tab */
        }
        return true;
    }
    return false;
}

static void entry_activate(fdk_widget *e, void *user) {
    (void)user;
    const char *text = fdk_entry_get_text(e);
    char buf[512];
    snprintf(buf, sizeof buf, "%s", text != NULL ? text : "");
    (void)fdk_entry_set_text(e, "");
    capp.hist_pos = capp.hist_count;
    chat_entry_submit(buf);
}

/* ---- menus ------------------------------------------------------------ */

typedef struct {
    void (*fn)(fdk_widget *, void *);
    void *arg;
} chat_menu_act;
static chat_menu_act g_acts[32];
static size_t g_act_count;
static fdk_menu *g_menus[8];
static size_t g_menu_count;

static void act_trampoline(fdk_menu_item *it, void *user) {
    (void)it;
    size_t idx = (size_t)(uintptr_t)user;
    if (idx < g_act_count && g_acts[idx].fn != NULL) {
        g_acts[idx].fn(NULL, g_acts[idx].arg);
    }
}

static void menu_add(fdk_menu *m, const char *label,
                     const char *shortcut,
                     void (*fn)(fdk_widget *, void *)) {
    fdk_menu_item *it = NULL;
    if (fdk_menu_append(m, label, &it) == FDK_OK && it != NULL) {
        if (shortcut != NULL && shortcut[0] != '\0') {
            (void)fdk_menu_item_set_shortcut(it, shortcut);
        }
        if (fn != NULL && g_act_count < 32) {
            g_acts[g_act_count].fn = fn;
            g_acts[g_act_count].arg = NULL;
            fdk_menu_item_set_on_activate(
                it, act_trampoline, (void *)(uintptr_t)g_act_count);
            g_act_count++;
        }
    }
}

static void menu_connect(fdk_widget *w, void *u) {
    (void)w;
    (void)u;
    chat_connect_now();
}

static void menu_disconnect(fdk_widget *w, void *u) {
    (void)w;
    (void)u;
    if (capp.conn != NULL) {
        chat_send_line("QUIT :closing");
    }
    chat_disconnect_now(ta_("by user"));
}

static void menu_join(fdk_widget *w, void *u) {
    (void)w;
    (void)u;
    chat_entry_submit("/join #faded-dream");
}

static void menu_quit(fdk_widget *w, void *u) {
    (void)w;
    (void)u;
    if (capp.conn != NULL) {
        chat_send_line("QUIT :closing");
    }
    chat_quit();
}

static void menu_clear_log(fdk_widget *w, void *u) {
    (void)w;
    (void)u;
    if (capp.active >= 0) {
        chat_log_clear(capp.convs[capp.active].log);
    }
}

static void menu_about(fdk_widget *w, void *u) {
    (void)w;
    (void)u;
    fdk_dialog_options opts;
    memset(&opts, 0, sizeof opts);
    opts.title = ta_("About fdk-chat");
    opts.text = ta_(
        "fdk-chat — an IRC client built on FDK.\n\n"
        "App B of the test-app validation suite: the streaming "
        "TextView, the fd extension loop, link attributes, "
        "notifications, prefs, i18n and a11y — public API only.");
    opts.font = capp.base.font_ui;
    opts.parent = capp.base.window;
    (void)fdk_dialog_show_message(capp.base.ctx, &opts, NULL, NULL,
                                  NULL);
}

static void menus_build(void) {
    fdk_font *f = capp.base.font_ui;
    fdk_widget *bar = NULL;
    (void)fdk_menu_bar_create(capp.base.menubar_host, f, &bar);

    fdk_menu *m_chat = NULL;
    (void)fdk_menu_create(f, &m_chat);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_chat;
    }
    menu_add(m_chat, ta_("&Connect"), "Ctrl+Shift+L", menu_connect);
    menu_add(m_chat, ta_("&Disconnect"), "Ctrl+Shift+K",
             menu_disconnect);
    menu_add(m_chat, ta_("&Join #faded-dream"), "Ctrl+J",
             menu_join);
    menu_add(m_chat, ta_("&Quit"), "Ctrl+Q", menu_quit);
    (void)fdk_menu_bar_append(bar, ta_("&Chat"), m_chat);

    fdk_menu *m_conv = NULL;
    (void)fdk_menu_create(f, &m_conv);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_conv;
    }
    menu_add(m_conv, ta_("&Clear this log"), NULL, menu_clear_log);
    (void)fdk_menu_bar_append(bar, ta_("&Conversation"), m_conv);

    fdk_menu *m_help = NULL;
    (void)fdk_menu_create(f, &m_help);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_help;
    }
    menu_add(m_help, ta_("&About"), NULL, menu_about);
    (void)fdk_menu_bar_append(bar, ta_("&Help"), m_help);
}

/* ---- notebook switch ----------------------------------------------------- */

static void notebook_switch(fdk_widget *nb, size_t page,
                            void *user) {
    (void)nb;
    (void)user;
    if ((int)page == capp.active ||
        (size_t)page >= capp.n_convs) {
        return;
    }
    capp.active = (int)page;
    conversation *cv = &capp.convs[page];
    cv->unread = false;
    cv->highlight = false;
    (void)fdk_notebook_set_page_label(capp.notebook, page,
                                      conv_label(cv));
    chat_set_topic(cv);
    chat_refresh_userlist();
}

/* ---- shortcuts ---------------------------------------------------------- */

static void sc_connect(fdk_window *w, void *u) {
    (void)w;
    (void)u;
    chat_connect_now();
}

static void sc_quit(fdk_window *w, void *u) {
    (void)w;
    (void)u;
    if (capp.conn != NULL) {
        chat_send_line("QUIT :closing");
    }
    chat_quit();
}

/* ---- build ---------------------------------------------------------------- */

bool chat_ui_build(void) {
    /* content: [main area][topic][entry] */
    fdk_widget *content = NULL;
    if (!fdk_ok(fdk_box_create(capp.base.content, FDK_VERTICAL,
                               &content))) {
        return false;
    }
    fdk_box_set_spacing(content, 4);

    fdk_widget *main = NULL;
    if (!fdk_ok(fdk_box_create(content, FDK_HORIZONTAL, &main))) {
        return false;
    }
    fdk_box_set_spacing(main, 6);
    fdk_widget_set_expand(main, true, true);

    if (!fdk_ok(fdk_notebook_create(main, capp.base.font_ui,
                                    &capp.notebook))) {
        return false;
    }
    fdk_widget_set_expand(capp.notebook, true, true);
    fdk_widget_set_accessible_name(capp.notebook,
                                   ta_("Conversations"));
    fdk_notebook_set_on_switch(capp.notebook, notebook_switch,
                               NULL);

    /* the member sidebar */
    fdk_widget *sidebar = NULL;
    if (!fdk_ok(fdk_box_create(main, FDK_VERTICAL, &sidebar))) {
        return false;
    }
    fdk_widget *members_label = NULL;
    (void)fdk_label_create(sidebar, capp.base.font_ui,
                           ta_("Members"), &members_label);
    if (!fdk_ok(fdk_list_create(sidebar, capp.base.font_ui,
                                &capp.userlist))) {
        return false;
    }
    fdk_widget_set_expand(capp.userlist, true, true);
    fdk_list_set_selection_mode(capp.userlist,
                                FDK_LIST_SELECTION_NONE);
    fdk_widget_set_accessible_name(capp.userlist,
                                   ta_("Channel members"));

    /* the topic line */
    if (!fdk_ok(fdk_label_create(content, capp.base.font_ui,
                                 capp.server,
                                 &capp.topic_label))) {
        return false;
    }
    fdk_widget_set_accessible_name(capp.topic_label, ta_("Topic"));

    /* the entry */
    if (!fdk_ok(fdk_entry_create(content, capp.base.font_ui, "",
                                 &capp.entry))) {
        return false;
    }
    fdk_widget_set_accessible_name(capp.entry, ta_("Message"));
    fdk_entry_set_placeholder(capp.entry,
                              ta_("Say something, or /help"));
    fdk_entry_set_on_activate(capp.entry, entry_activate, NULL);
    fdk_widget_set_event_callback(capp.entry, entry_key, NULL);

    menus_build();
    (void)fdk_window_add_shortcut(capp.base.window, "Ctrl+Shift+L",
                                  sc_connect, NULL);
    (void)fdk_window_add_shortcut(capp.base.window, "Ctrl+Q",
                                  sc_quit, NULL);
    return true;
}

void chat_menus_shutdown(void) {
    for (size_t i = 0; i < g_menu_count; i++) {
        if (g_menus[i] != NULL) {
            fdk_menu_destroy(g_menus[i]);
            g_menus[i] = NULL;
        }
    }
    g_menu_count = 0;
}
