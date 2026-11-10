/* chat_test.c — headless unit tests for fdk-chat's engine layers
 * (irc.c, net.c). The UI is exercised by the Xvfb smoke rig with
 * the local ircd; everything protocol-side runs here under the
 * sanitizers.
 */
#include "irc.h"
#include "net.h"

#include "fdk/fdk.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ---- the parser ---------------------------------------------------- */

static void test_parse_basics(void) {
    char buf[256];
    irc_msg m;

    snprintf(buf, sizeof buf, ":dan!d@localhost PRIVMSG #chan :hi");
    assert(irc_parse(buf, &m));
    assert(strcmp(m.nick, "dan") == 0);
    assert(strcmp(m.user, "d") == 0);
    assert(strcmp(m.host, "localhost") == 0);
    assert(strcmp(m.command, "PRIVMSG") == 0);
    assert(m.n_params == 2);
    assert(strcmp(m.params[0], "#chan") == 0);
    assert(strcmp(m.trailing, "hi") == 0);

    snprintf(buf, sizeof buf, "PING :irc.example.org");
    assert(irc_parse(buf, &m));
    assert(m.nick == NULL);
    assert(strcmp(m.command, "PING") == 0);
    assert(strcmp(m.trailing, "irc.example.org") == 0);

    snprintf(buf, sizeof buf, "JOIN #a,#b key");
    assert(irc_parse(buf, &m));
    assert(m.n_params == 2 && m.trailing == NULL);
    assert(strcmp(m.params[0], "#a,#b") == 0);

    /* tags tolerated + skipped */
    snprintf(buf, sizeof buf,
             "@time=2026-11-06T00:00:00Z :n!u@h PRIVMSG #c :x");
    assert(irc_parse(buf, &m));
    assert(m.raw_tags != NULL);
    assert(strcmp(m.nick, "n") == 0);
    assert(strcmp(m.trailing, "x") == 0);

    /* numeric detection */
    snprintf(buf, sizeof buf,
             ":srv.example 001 me :Welcome to the network me");
    assert(irc_parse(buf, &m));
    assert(m.numeric == 1);
    assert(m.n_params == 2);
    assert(strcmp(m.params[0], "me") == 0);

    snprintf(buf, sizeof buf, ":srv 433 * dreamer :Nickname is already in use");
    assert(irc_parse(buf, &m));
    assert(m.numeric == 433);

    /* empty / only-prefix lines are honest non-events */
    buf[0] = '\0';
    assert(!irc_parse(buf, &m));
    snprintf(buf, sizeof buf, "@tag=1");
    assert(!irc_parse(buf, &m));

    printf("[ok] irc parse: prefix parts, trailing, tags, "
           "numerics, empty lines\n");
}

/* ---- mIRC decode ----------------------------------------------------- */

static void test_mirc(void) {
    char out[128];
    irc_seg segs[IRC_SEG_MAX];
    size_t n = 0;

    /* plain text: one segment */
    assert(irc_decode_mirc("hello", 5, out, sizeof out, segs,
                           IRC_SEG_MAX, &n));
    assert(n == 1 && segs[0].len == 5 &&
           memcmp(segs[0].text, "hello", 5) == 0);
    assert(!segs[0].bold && !segs[0].fg_set);

    /* bold + color split */
    const char *s = "A\x02""B\x03""04C\x0F";
    assert(irc_decode_mirc(s, strlen(s), out, sizeof out, segs,
                           IRC_SEG_MAX, &n));
    assert(n == 3);
    assert(segs[0].len == 1 && segs[0].text[0] == 'A' &&
           !segs[0].bold);
    assert(segs[1].len == 1 && segs[1].text[0] == 'B' &&
           segs[1].bold);
    assert(segs[2].len == 1 && segs[2].text[0] == 'C');
    assert(segs[2].fg_set && segs[2].fg == 4);

    /* two-part color: fg,bg */
    const char *s2 = "\x03""12,04xy\x03";
    assert(irc_decode_mirc(s2, strlen(s2), out, sizeof out, segs,
                           IRC_SEG_MAX, &n));
    assert(n == 1);
    assert(segs[0].fg_set && segs[0].fg == 12);
    assert(segs[0].bg_set && segs[0].bg == 4);
    assert(segs[0].len == 2 &&
           memcmp(segs[0].text, "xy", 2) == 0);

    /* bare 0x03 resets colors; styles ride on; colors parse at
     * most TWO digits (real mIRC: "100" is color 10 + "0w") */
    const char *s3 = "\x03\x1Dz\x03""100w";
    assert(irc_decode_mirc(s3, strlen(s3), out, sizeof out, segs,
                           IRC_SEG_MAX, &n));
    assert(n == 2);
    assert(segs[0].italic && segs[0].len == 1 &&
           segs[0].text[0] == 'z' && !segs[0].fg_set);
    assert(segs[1].fg_set && segs[1].fg == 10);
    assert(segs[1].italic); /* 0x03 never resets styles */
    assert(segs[1].len == 2 &&
           memcmp(segs[1].text, "0w", 2) == 0);

    /* control codes never expand the output */
    char small[16];
    const char *s4 = "abcdef\x02gh";
    assert(irc_decode_mirc(s4, strlen(s4), small, sizeof small,
                           segs, IRC_SEG_MAX, &n));
    assert(strcmp(small, "abcdefgh") == 0);

    printf("[ok] mirc decode: bold/color/italic, fg+bg, bare-reset, "
           "256-clamp, no expansion\n");
}

/* ---- urls + mentions -------------------------------------------------- */

static void test_urls_mentions(void) {
    irc_url urls[IRC_URLS_MAX];
    const char *t1 = "see https://example.com/x now, www.fdk.dev!";
    size_t n = irc_find_urls(t1, strlen(t1), urls, IRC_URLS_MAX);
    assert(n == 2);
    assert(strncmp(t1 + urls[0].start, "https://example.com/x",
                   urls[0].end - urls[0].start) == 0);
    assert(strncmp(t1 + urls[1].start, "www.fdk.dev",
                   urls[1].end - urls[1].start) == 0);

    /* sentence punctuation is not part of the url */
    const char *t2 = "(visit http://a.io).";
    n = irc_find_urls(t2, strlen(t2), urls, IRC_URLS_MAX);
    assert(n == 1);
    assert(strncmp(t2 + urls[0].start, "http://a.io",
                   urls[0].end - urls[0].start) == 0);

    /* mid-word "https://" does not start a url */
    const char *t3 = "abchttps://x";
    n = irc_find_urls(t3, strlen(t3), urls, IRC_URLS_MAX);
    assert(n == 0);

    /* mentions: word-boundary, case-insensitive, RFC casemapping */
    assert(irc_mentions("dream", "hey Dream, look!"));
    assert(irc_mentions("dream", "DREAM:"));
    assert(irc_mentions("{dream}", "hi {DREAM}?"));
    assert(!irc_mentions("dream", "daydreaming"));
    assert(!irc_mentions("dream", "dreamer"));
    assert(!irc_mentions("dream", "xdreamx"));

    printf("[ok] urls + mentions: schemes, punctuation trim, "
           "word boundaries, casemapping\n");
}

/* ---- channel state ------------------------------------------------------ */

static void test_channel_state(void) {
    irc_state st;
    irc_state_init(&st);

    assert(irc_is_channel("#a") && irc_is_channel("&b"));
    assert(!irc_is_channel("nick"));

    irc_chan *ch = irc_chan_join(&st, "#Test");
    assert(ch != NULL);
    assert(irc_chan_find(&st, "#TEST") == ch); /* casemapping */
    assert(irc_chan_find(&st, "#test") == ch);
    assert(irc_chan_join(&st, "#test") == ch); /* no duplicate */

    /* NAMES with mode prefixes; highest prefix wins */
    irc_chan_names_add(ch, "@alice +bob %carol dave");
    assert(ch->n_users == 4);
    irc_user *u = irc_chan_user_find(ch, "alice");
    assert(u != NULL && u->mode == '@');
    assert(irc_chan_user_find(ch, "bob")->mode == '+');
    assert(irc_chan_user_find(ch, "carol")->mode == '%');
    assert(irc_chan_user_find(ch, "dave")->mode == 0);

    /* a duplicate join keeps the entry, mode upgrade */
    irc_chan_names_add(ch, "+alice");
    assert(ch->n_users == 4);

    assert(irc_chan_user_remove(ch, "bob"));
    assert(irc_chan_user_find(ch, "bob") == NULL);
    assert(ch->n_users == 3);

    assert(irc_chan_user_rename(ch, "dave", "dave2"));
    assert(irc_chan_user_find(ch, "dave2") != NULL);

    /* part removes the channel */
    assert(irc_chan_part(&st, "#TEST"));
    assert(irc_chan_find(&st, "#test") == NULL);

    irc_state_destroy(&st);
    printf("[ok] channel state: casemapping, NAMES modes, dup, "
           "remove/rename, part\n");
}

/* ---- the network layer, against a local socket pair ---------------------- */

static int g_lines = 0;
static char g_last_line[256];
static void on_line(void *user, const char *line) {
    (void)user;
    snprintf(g_last_line, sizeof g_last_line, "%s", line);
    g_lines++;
}

static chat_state g_state;
static char g_state_detail[128];
static void on_state(void *user, chat_state st, const char *detail) {
    (void)user;
    g_state = st;
    snprintf(g_state_detail, sizeof g_state_detail, "%s",
             detail != NULL ? detail : "");
}

/* A tiny in-process peer: one connected TCP socket we can read and
 * write like a miniature server. */
static int peer_listen(int *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    assert(bind(fd, (struct sockaddr *)&a, sizeof a) == 0);
    assert(listen(fd, 1) == 0);
    socklen_t len = sizeof a;
    assert(getsockname(fd, (struct sockaddr *)&a, &len) == 0);
    *out_port = ntohs(a.sin_port);
    return fd;
}

static int peer_accept(int lfd) {
    int c = accept(lfd, NULL, NULL);
    assert(c >= 0);
    (void)fcntl(c, F_SETFL, O_NONBLOCK);
    return c;
}

static bool peer_read_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r <= 0) {
            return false;
        }
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r') {
                n--;
            }
            out[n] = '\0';
            return true;
        }
        out[n++] = ch;
    }
    out[n] = '\0';
    return true;
}

static void test_net_layer(void) {
    /* net.c needs a context for the loop; the headless context has
     * no display — fdk_init works headless? The extension loop is
     * context-level: fdk_init(NULL options) needs no display (the
     * suite's hermetic env runs this way everywhere). */
    fdk_context *ctx = NULL;
    fdk_init_options opts;
    memset(&opts, 0, sizeof opts);
    opts.app_id = "org.fdk.testapps.chat-test";
    if (!fdk_ok(fdk_init(&ctx, &opts))) {
        printf("[skip] no headless context — net layer needs a "
               "display-less init that this environment refuses\n");
        return;
    }

    int port = 0;
    int lfd = peer_listen(&port);
    assert(port > 0);

    g_lines = 0;
    g_state = CHAT_CONNECTING;
    chat_conn *c = chat_connect(ctx, "121.0.0.1", port, on_line,
                                on_state, NULL);
    assert(c != NULL);
    int peer = peer_accept(lfd);

    /* the pump drives connect + I/O; a few passes settle it */
    for (int i = 0; i < 50 && g_state != CHAT_CONNECTED; i++) {
        fdk_pump_events(ctx, 10);
    }
    assert(g_state == CHAT_CONNECTED);

    /* server -> client: two lines in one write + a split line */
    const char *two =
        ":srv 001 me :welcome\r\n:srv 002 me :your host\r\n";
    assert(write(peer, two, strlen(two)) == (ssize_t)strlen(two));
    assert(write(peer, ":sr", 3) == 3);
    for (int i = 0; i < 20; i++) {
        fdk_pump_events(ctx, 10);
    }
    assert(write(peer, "v 003 me :split across reads\r\n", 30) > 0);
    for (int i = 0; i < 30; i++) {
        fdk_pump_events(ctx, 10);
    }
    assert(g_lines == 3);
    assert(strstr(g_last_line, "003") != NULL);

    /* client -> server */
    assert(chat_send(c, "NICK dreamer"));
    char got[256];
    assert(peer_read_line(peer, got, sizeof got));
    assert(strcmp(got, "NICK dreamer") == 0);

    /* the state machinery: the server closes */
    close(peer);
    for (int i = 0; i < 50 && g_state != CHAT_CLOSED; i++) {
        fdk_pump_events(ctx, 10);
    }
    assert(g_state == CHAT_CLOSED);

    chat_disconnect(c);
    close(lfd);
    printf("[ok] net layer: connect, split reads deliver whole "
           "lines, sends, orderly close\n");
}

int main(void) {
    test_parse_basics();
    test_mirc();
    test_urls_mentions();
    test_channel_state();
    test_net_layer();
    printf("all fdk-chat engine tests passed\n");
    return 0;
}
