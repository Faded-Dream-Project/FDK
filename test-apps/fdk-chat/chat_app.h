/* chat_app.h — the fdk-chat application state (shared ui/main). */
#ifndef FDKCHAT_APP_H
#define FDKCHAT_APP_H

#include "irc.h"
#include "log.h"
#include "net.h"

#include "../common/app.h"

#include <stdbool.h>
#include <stddef.h>

#define CHAT_MAX_CONVS 16
#define CHAT_ENTRY_HISTORY 64

typedef struct conversation {
    char name[IRC_CHAN_MAX]; /* "#chan", "*server", or a PM nick */
    bool is_server;
    bool is_pm;
    chat_log *log;
    char topic[IRC_TOPIC_MAX];
    bool unread;
    bool highlight;
} conversation;

typedef struct chat_app {
    ta_app base;
    irc_state irc;
    chat_conn *conn;
    char server[128];
    int port;
    char want_nick[IRC_NICK_MAX];
    char username[64];
    char realname[96];
    char join[256]; /* FDK_CHAT_JOIN: comma-separated boot joins */
    bool autoconnect; /* FDK_CHAT_HOST boot (the rig path) */

    conversation convs[CHAT_MAX_CONVS];
    size_t n_convs;
    int active; /* index into convs; -1 = none */

    fdk_widget *notebook;
    fdk_widget *userlist;
    fdk_widget *entry;
    fdk_widget *topic_label;

    char history[CHAT_ENTRY_HISTORY][512];
    int hist_count, hist_pos;

    bool focused; /* the window's focus (highlight gate) */

    /* menu + tray ownership (the app-owned model contract) */
    void *menus[8];
    size_t n_menus;
    void *tray;
    void *notify;

    long msg_total; /* the storm's counter (telemetry) */
} chat_app;

extern chat_app capp;

/* main.c */
void chat_quit(void);
void chat_connect_now(void);
void chat_disconnect_now(const char *why);
void chat_send_line(const char *line); /* one outgoing IRC line */
conversation *chat_conv_by_name(const char *name);
conversation *chat_conv_open(const char *name, bool is_server,
                             bool is_pm);
void chat_conv_close(conversation *cv);
void chat_show_conv(int idx);
void chat_refresh_userlist(void);
void chat_note_unread(int idx, bool highlight);
void chat_on_highlight(conversation *cv, const char *nick,
                       const char *body);

/* ui.c */
bool chat_ui_build(void);
void chat_menus_shutdown(void);
void chat_ui_status(const char *fmt, ...);
void chat_entry_submit(const char *text);
void chat_set_topic(const conversation *cv);
int chat_notebook_page_of(const conversation *cv);

#endif /* FDKCHAT_APP_H */
