/*
 * test_dnd_logic.c — headless DnD logic (1.2.0)
 *
 * The protocol halves (XDND, wl_data_device) need a display and live
 * in the integration suites. What CAN be pinned headlessly is the
 * codec both backends share — parse (CR/LF tolerance, comments,
 * file:// decoding, percent-decoding, non-file URIs verbatim, empty
 * and hostile payloads) and build (escaping, file:// encoding,
 * already-URI passthrough, round-trip stability) — plus the argument
 * safety of the public API surface.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_dnd.h"
#include "platform/platform_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);       \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void test_parse_basic(void) {
    char **list = NULL;
    size_t n = 0;

    /* The canonical CRLF form, three files. */
    const char *payload =
        "file:///home/user/a.txt\r\n"
        "file:///home/user/b dir/c.png\r\n"
        "file://localhost/tmp/d\r\n";
    CHECK(fdk__dnd_parse_uri_list(payload, strlen(payload), &list, &n) ==
              FDK_OK,
          "parse basic ok");
    CHECK(n == 3, "parse basic count");
    if (n == 3) {
        CHECK(strcmp(list[0], "/home/user/a.txt") == 0,
              "plain file URI -> path");
        CHECK(strcmp(list[1], "/home/user/b dir/c.png") == 0,
              "spaces preserved");
        CHECK(strcmp(list[2], "/tmp/d") == 0,
              "localhost authority stripped");
    }
    fdk__dnd_free_uri_list(list, n);

    /* LF-only lines (some sources) and a comment. */
    payload = "# comment line\nfile:///x\nfile:///y\n";
    CHECK(fdk__dnd_parse_uri_list(payload, strlen(payload), &list, &n) ==
              FDK_OK,
          "parse lf/comment ok");
    CHECK(n == 2, "comment skipped");
    if (n == 2) {
        CHECK(strcmp(list[0], "/x") == 0 &&
                  strcmp(list[1], "/y") == 0,
              "lf lines parsed");
    }
    fdk__dnd_free_uri_list(list, n);
}

static void test_parse_percent(void) {
    char **list = NULL;
    size_t n = 0;
    const char *payload =
        "file:///home/user/100%25%20done/caf%C3%A9.txt";
    CHECK(fdk__dnd_parse_uri_list(payload, strlen(payload), &list, &n) ==
              FDK_OK,
          "parse percent ok");
    CHECK(n == 1, "percent count");
    if (n == 1) {
        CHECK(strcmp(list[0], "/home/user/100% done/caf\xc3\xa9.txt") ==
                  0,
              "percent-decoding applied to file URIs");
    }
    fdk__dnd_free_uri_list(list, n);

    /* Malformed escape: contained verbatim, never a crash. */
    payload = "file:///a%2";
    CHECK(fdk__dnd_parse_uri_list(payload, strlen(payload), &list, &n) ==
              FDK_OK,
          "malformed parse ok");
    CHECK(n == 1, "malformed count");
    if (n == 1) {
        CHECK(strcmp(list[0], "/a%2") == 0,
              "malformed escape kept verbatim");
    }
    fdk__dnd_free_uri_list(list, n);
}

static void test_parse_non_file(void) {
    char **list = NULL;
    size_t n = 0;
    /* Non-file schemes pass through untouched; a remote file host
     * keeps the full URI (the app can decide what it means). */
    const char *payload =
        "https://example.com/x\r\nfile://nas/share/doc\r\n";
    CHECK(fdk__dnd_parse_uri_list(payload, strlen(payload), &list, &n) ==
              FDK_OK,
          "non-file parse ok");
    CHECK(n == 2, "non-file count");
    if (n == 2) {
        CHECK(strcmp(list[0], "https://example.com/x") == 0,
              "https verbatim");
        CHECK(strcmp(list[1], "file://nas/share/doc") == 0,
              "remote host verbatim");
    }
    fdk__dnd_free_uri_list(list, n);
}

static void test_parse_empty_hostile(void) {
    char **list = NULL;
    size_t n = 0;

    CHECK(fdk__dnd_parse_uri_list(NULL, 0, &list, &n) == FDK_OK &&
              list == NULL && n == 0,
          "NULL payload -> empty");
    CHECK(fdk__dnd_parse_uri_list("", 0, &list, &n) == FDK_OK &&
              list == NULL && n == 0,
          "empty payload -> empty");
    /* No separators, no NUL-termination guarantee, embedded binary
     * junk: bounded, safe, contained — the entry after the comment
     * line survives with its embedded NUL truncating the C-string
     * view (bytes are preserved; strlen sees 0). */
    const char junk[] = {'#', '\r', '\n', '\0', 'x'};
    CHECK(fdk__dnd_parse_uri_list(junk, sizeof(junk), &list, &n) ==
              FDK_OK,
          "junk parse ok");
    CHECK(n == 1 && list[0] != NULL && strlen(list[0]) == 0,
          "junk contained, no crash");
    fdk__dnd_free_uri_list(list, n);
    fdk__dnd_free_uri_list(NULL, 0); /* NULL free is legal */
}

static void test_build(void) {
    /* Escaping + file:// encoding of plain paths. */
    const char *paths[2] = { "/tmp/a b.txt", "/tmp/caf\xc3\xa9.txt" };
    char *payload = fdk__dnd_build_uri_list(paths, 2);
    CHECK(payload != NULL, "build ok");
    if (payload != NULL) {
        CHECK(strstr(payload, "file:///tmp/a%20b.txt") != NULL,
              "space escaped");
        CHECK(strstr(payload, "file:///tmp/caf%C3%A9.txt") != NULL,
              "UTF-8 escaped");
        CHECK(strstr(payload, "\r\n") != NULL, "CRLF separator");
        CHECK(payload[strlen(payload) - 1] != '\n',
              "no trailing separator");
    }
    free(payload); /* fdk_alloc is heap-backed; the examples' contract
                      allows plain free for app-owned buffers */

    /* Already-URI entries pass through. */
    const char *mixed[2] = { "https://e/x", "/tmp/z" };
    payload = fdk__dnd_build_uri_list(mixed, 2);
    CHECK(payload != NULL &&
              strstr(payload, "https://e/x") != NULL &&
              strstr(payload, "file:///tmp/z") != NULL,
          "uri passthrough + path encoding in one build");
    free(payload);

    CHECK(fdk__dnd_build_uri_list(NULL, 3) == NULL, "NULL list");
    CHECK(fdk__dnd_build_uri_list(paths, 0) == NULL, "zero count");
}

static void test_round_trip(void) {
    const char *paths[3] = {
        "/tmp/fdk round trip/a.txt",
        "/tmp/fdk'quote.bin",
        "/tmp/plain",
    };
    char *payload = fdk__dnd_build_uri_list(paths, 3);
    CHECK(payload != NULL, "round trip build");
    if (payload != NULL) {
        char **list = NULL;
        size_t n = 0;
        CHECK(fdk__dnd_parse_uri_list(payload, strlen(payload), &list,
                                      &n) == FDK_OK,
              "round trip parse");
        CHECK(n == 3, "round trip count");
        if (n == 3) {
            for (size_t i = 0; i < 3; i++) {
                if (strcmp(list[i], paths[i]) != 0) {
                    fprintf(stderr,
                            "FAIL: round trip %zu: %s != %s\n", i,
                            list[i], paths[i]);
                    failures++;
                    break;
                }
            }
        }
        fdk__dnd_free_uri_list(list, n);
    }
    free(payload);
}

static void drag_done_noop(fdk_drag_status s, void *u) {
    (void)s; (void)u;
}

static void test_api_argument_safety(void) {
    CHECK(fdk_window_set_drop_formats(NULL, FDK_DRAG_FORMAT_TEXT) ==
              FDK_ERR_INVALID_ARGUMENT,
          "drop formats NULL window");
    CHECK(fdk_window_get_drop_formats(NULL) == 0, "get NULL window");

    CHECK(fdk_drag_begin(NULL, FDK_DRAG_FORMAT_TEXT, "x", NULL, 0,
                         drag_done_noop, NULL) ==
              FDK_ERR_INVALID_ARGUMENT,
          "drag NULL window");
    CHECK(fdk_drag_begin(NULL, 0, "x", NULL, 0, drag_done_noop,
                         NULL) == FDK_ERR_INVALID_ARGUMENT,
          "drag zero formats");
    CHECK(fdk_drag_begin(NULL, FDK_DRAG_FORMAT_URI_LIST, NULL, NULL,
                         0, drag_done_noop, NULL) ==
              FDK_ERR_INVALID_ARGUMENT,
          "drag uris without entries");
    CHECK(fdk_drag_begin(NULL, 0x40, "x", NULL, 0, drag_done_noop,
                         NULL) == FDK_ERR_INVALID_ARGUMENT,
          "drag unknown format bits");
}

int main(void) {
    test_parse_basic();
    test_parse_percent();
    test_parse_non_file();
    test_parse_empty_hostile();
    test_build();
    test_round_trip();
    test_api_argument_safety();

    if (failures != 0) {
        fprintf(stderr, "dnd logic: %d failure(s)\n", failures);
        return 1;
    }
    printf("dnd logic: all checks passed\n");
    return 0;
}
