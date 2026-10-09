/*
 * test_file_dialog_logic.c — headless file-dialog logic (1.2.0; 1.2.3)
 *
 * The interactive dialog is GUI-tested in the integration suites and
 * the examples rig. What CAN be pinned headlessly is the scan seam
 * the dialog is built on (widgets_internal.h): hidden filtering,
 * dirs-only filtering, dirs-first ordering, the "."/".." exclusion,
 * unreadable directories, the entries' ownership contract — plus,
 * since 1.2.3, the glob matcher, filter parsing, filesystem
 * discovery's invariants, the path helpers, and SAVE name
 * validation — plus the result-model free function's tolerance.
 */

#include "fdk/fdk.h"
#include "core/alloc_internal.h"
#include "widget/widgets_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);       \
            failures++;                                                    \
        }                                                                  \
    } while (0)

/* Builds a scratch tree under /tmp with a fixed shape:
 *
 *   dir/            (the scanned directory)
 *     zeta/         (dir, sorts after files below are still FIRST)
 *     alpha/        (dir)
 *     .hidden/      (dir, hidden)
 *     beta.txt      (file)
 *     photo.PNG     (file — case-insensitive filter bait)
 *     notes.c       (file)
 *     .dotfile      (file, hidden)
 */
static const char *g_dir;

static void make_scratch(void) {
    static char tmpl[] = "/tmp/fdk-fdtest-XXXXXX";
    char *d = mkdtemp(tmpl);
    if (d == NULL) {
        perror("mkdtemp");
        exit(2);
    }
    static char buf[512];
    const char *subs[] = { "/zeta", "/alpha", "/.hidden", NULL };
    for (int i = 0; subs[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), "%s%s", d, subs[i]);
        mkdir(buf, 0755);
    }
    const char *files[] = { "/beta.txt", "/photo.PNG", "/notes.c",
                            "/.dotfile", NULL };
    for (int i = 0; files[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), "%s%s", d, files[i]);
        FILE *f = fopen(buf, "w");
        if (f != NULL) {
            fputs("x", f);
            fclose(f);
        }
    }
    g_dir = d;
}

static void rm_scratch(void) {
    char buf[512];
    const char *files[] = { "/.dotfile", "/notes.c", "/photo.PNG",
                            "/beta.txt", NULL };
    for (int i = 0; files[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), "%s%s", g_dir, files[i]);
        unlink(buf);
    }
    const char *subs[] = { "/zeta", "/alpha", "/.hidden", NULL };
    for (int i = 0; subs[i] != NULL; i++) {
        snprintf(buf, sizeof(buf), "%s%s", g_dir, subs[i]);
        rmdir(buf);
    }
    rmdir(g_dir);
}

static void test_scan_defaults(void) {
    fdk_fd_entries e;
    CHECK(fdk__file_dialog_scan(g_dir, false, false, NULL, 0, &e) == 0,
          "scan defaults ok");
    /* dirs first (alpha, zeta), then files (beta.txt, notes.c,
     * photo.PNG); hidden gone; no . or .. rows. */
    CHECK(e.count == 5, "default count hides dot entries");
    if (e.count == 5) {
        CHECK(strcmp(e.v[0].name, "alpha") == 0 && e.v[0].dir,
              "dirs sort first, alpha");
        CHECK(strcmp(e.v[1].name, "zeta") == 0 && e.v[1].dir,
              "zeta second");
        CHECK(strcmp(e.v[2].name, "beta.txt") == 0 && !e.v[2].dir,
              "file after dirs");
        CHECK(strcmp(e.v[3].name, "notes.c") == 0,
              "alphabetical within files");
        CHECK(strcmp(e.v[4].name, "photo.PNG") == 0,
              "alphabetical within files (uppercase sorts later)");
    }
    fdk__file_dialog_entries_free(&e);
    CHECK(e.v == NULL && e.count == 0, "entries_free clears");
}

static void test_scan_hidden(void) {
    fdk_fd_entries e;
    CHECK(fdk__file_dialog_scan(g_dir, false, true, NULL, 0, &e) == 0,
          "scan hidden ok");
    CHECK(e.count == 7, "hidden count");
    if (e.count == 7) {
        /* .hidden and .dotfile present; . and .. never are. */
        bool have_hidden_dir = false, have_dotfile = false;
        for (size_t i = 0; i < e.count; i++) {
            CHECK(e.v[i].hidden ||
                      strcmp(e.v[i].name, "alpha") == 0 ||
                      strcmp(e.v[i].name, "zeta") == 0 ||
                      strcmp(e.v[i].name, "beta.txt") == 0 ||
                      strcmp(e.v[i].name, "notes.c") == 0 ||
                      strcmp(e.v[i].name, "photo.PNG") == 0,
                  "unexpected entry");
            if (strcmp(e.v[i].name, ".hidden") == 0 && e.v[i].dir) {
                have_hidden_dir = true;
            }
            if (strcmp(e.v[i].name, ".dotfile") == 0 && !e.v[i].dir) {
                have_dotfile = true;
            }
        }
        CHECK(have_hidden_dir && have_dotfile, "dot entries included");
        /* dirs first overall: .hidden sorts before alpha? Both are
         * dirs; alphabetical: ".hidden" < "alpha" ('.' = 0x2E). */
        CHECK(strcmp(e.v[0].name, ".hidden") == 0,
              "hidden dir sorts within dirs");
    }
    fdk__file_dialog_entries_free(&e);
}

static void test_scan_dirs_only(void) {
    fdk_fd_entries e;
    CHECK(fdk__file_dialog_scan(g_dir, true, true, NULL, 0, &e) == 0,
          "scan dirs-only ok");
    CHECK(e.count == 3, "dirs-only count");
    for (size_t i = 0; i < e.count; i++) {
        CHECK(e.v[i].dir, "dirs-only yields only dirs");
    }
    fdk__file_dialog_entries_free(&e);
}

static void test_scan_unreadable(void) {
    fdk_fd_entries e;
    CHECK(fdk__file_dialog_scan("/nonexistent-fdk-test-dir", false,
                                false, NULL, 0, &e) != 0,
          "unreadable dir reports failure");
    CHECK(e.v == NULL && e.count == 0,
          "failed scan leaves empty result");
    fdk__file_dialog_entries_free(&e);
}

/* ---- 1.2.3: filters ---- */

static void test_glob_match(void) {
    CHECK(fdk__file_dialog_glob_match("*", "anything.at-all"),
          "* matches everything");
    CHECK(fdk__file_dialog_glob_match("*.txt", "beta.txt"),
          "*.txt matches beta.txt");
    CHECK(fdk__file_dialog_glob_match("*.TXT", "beta.txt"),
          "case-insensitive: *.TXT matches beta.txt");
    CHECK(!fdk__file_dialog_glob_match("*.txt", "photo.PNG"),
          "*.txt does not match a .png file");
    CHECK(fdk__file_dialog_glob_match("*.png", "photo.PNG"),
          "*.png matches photo.PNG case-insensitively");
    CHECK(!fdk__file_dialog_glob_match("*.c", "beta.txt"),
          "*.c does not match beta.txt");
    CHECK(fdk__file_dialog_glob_match("Makefile", "Makefile"),
          "literal matches itself");
    CHECK(fdk__file_dialog_glob_match("Makefile", "makefile"),
          "literal match is case-insensitive too");
    CHECK(!fdk__file_dialog_glob_match("Makefile", "Makefiles"),
          "literal does not match a longer name");
    CHECK(fdk__file_dialog_glob_match("note?.c", "notes.c"),
          "? matches one char");
    CHECK(!fdk__file_dialog_glob_match("note?.c", "notes.cc"),
          "? does not match two chars");
    CHECK(fdk__file_dialog_glob_match("a*b*c", "aXXbYYc"),
          "multiple stars");
    CHECK(fdk__file_dialog_glob_match("*", ""), "* matches empty");
    CHECK(fdk__file_dialog_glob_match("*x*", "axbxc"),
          "star sandwich");
    CHECK(!fdk__file_dialog_glob_match("*x", "abc"),
          "star must still find the x");
    CHECK(fdk__file_dialog_glob_match("", ""), "empty == empty");
    CHECK(!fdk__file_dialog_glob_match("", "x"), "empty != x");
}

static void test_parse_filters(void) {
    char **v = NULL;
    size_t n = fdk__file_dialog_parse_filters("*.c;*.h; Makefile ", &v);
    CHECK(n == 3, "three patterns parsed");
    if (n == 3) {
        CHECK(strcmp(v[0], "*.c") == 0, "pattern 0");
        CHECK(strcmp(v[1], "*.h") == 0, "pattern 1");
        CHECK(strcmp(v[2], "Makefile") == 0,
              "pattern 2 (whitespace trimmed)");
    }
    fdk__file_dialog_free_filters(v, n);

    n = fdk__file_dialog_parse_filters(";;  ;", &v);
    CHECK(n == 0 && v == NULL, "all-empty input -> no filter");

    n = fdk__file_dialog_parse_filters(NULL, &v);
    CHECK(n == 0 && v == NULL, "NULL input -> no filter");

    n = fdk__file_dialog_parse_filters("*.png", &v);
    CHECK(n == 1 && v != NULL && strcmp(v[0], "*.png") == 0,
          "single pattern");
    fdk__file_dialog_free_filters(v, n);
}

static void test_scan_filtered(void) {
    char pat_c[] = "*.c";
    char *pats[1] = { pat_c };
    fdk_fd_entries e;
    CHECK(fdk__file_dialog_scan(g_dir, false, false, pats, 1, &e) == 0,
          "filtered scan ok");
    /* dirs never filtered: alpha, zeta survive; only notes.c among
     * the files. */
    CHECK(e.count == 3, "filtered count (dirs + notes.c)");
    if (e.count == 3) {
        CHECK(strcmp(e.v[2].name, "notes.c") == 0,
              "only the matching file survives");
    }
    fdk__file_dialog_entries_free(&e);

    /* Case-insensitive file filtering. */
    char pat_png[] = "*.png";
    char *png[1] = { pat_png };
    CHECK(fdk__file_dialog_scan(g_dir, false, false, png, 1, &e) == 0,
          "case-insensitive scan ok");
    CHECK(e.count == 3, "photo.PNG survives *.png (dirs + 1)");
    if (e.count == 3) {
        CHECK(strcmp(e.v[2].name, "photo.PNG") == 0,
              "photo.PNG matched *.png");
    }
    fdk__file_dialog_entries_free(&e);

    /* Dirs-only + filter: the filter is irrelevant for dirs. */
    CHECK(fdk__file_dialog_scan(g_dir, true, false, png, 1, &e) == 0,
          "dirs-only + filter ok");
    CHECK(e.count == 2, "dirs-only ignores the file filter");
    fdk__file_dialog_entries_free(&e);
}

/* ---- 1.2.3: filesystem discovery ---- */

static void test_places(void) {
    fdk_fs_place *places = NULL;
    size_t count = 0;
    CHECK(fdk__fs_discover_places(&places, &count) == 0,
          "discovery succeeds");
    CHECK(count >= 1, "at least one place exists");
    if (places == NULL || count == 0) {
        return;
    }
    bool have_root = false;
    for (size_t i = 0; i < count; i++) {
        struct stat st;
        CHECK(places[i].path != NULL && places[i].label != NULL,
              "place fields populated");
        CHECK(stat(places[i].path, &st) == 0 && S_ISDIR(st.st_mode),
              "place exists and is a directory");
        CHECK(places[i].path[0] == '/', "place path is absolute");
        if (strcmp(places[i].path, "/") == 0) {
            have_root = true;
        }
        /* No duplicate canonical paths. */
        for (size_t j = i + 1; j < count; j++) {
            CHECK(strcmp(places[i].path, places[j].path) != 0,
                  "no duplicate places");
        }
    }
    CHECK(have_root, "root filesystem is a place");
    /* $HOME should be there too when set (the sandbox always sets
     * it; the check is env-conditional to stay honest). */
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        bool have_home = false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(places[i].path, home) == 0) {
                have_home = true;
            }
        }
        CHECK(have_home, "HOME is a place");
        CHECK(strcmp(places[0].label, "Home") == 0 &&
                  strcmp(places[0].path, home) == 0,
              "Home is first");
    }
    CHECK(count <= 24, "places capped");
    fdk__fs_places_free(places, count);
}

/* ---- 1.2.3: path helpers ---- */

static void test_path_helpers(void) {
    char *j = fdk__path_join("/a/b", "c.txt");
    CHECK(j != NULL && strcmp(j, "/a/b/c.txt") == 0, "join basic");
    fdk_free(j);
    j = fdk__path_join("/", "c.txt");
    CHECK(j != NULL && strcmp(j, "/c.txt") == 0,
          "join root does not double the slash");
    fdk_free(j);
    j = fdk__path_join("/a/", "c.txt");
    CHECK(j != NULL && strcmp(j, "/a/c.txt") == 0,
          "join tolerates trailing slash");
    fdk_free(j);

    char *n = fdk__path_normalize_dir("/a/b/");
    CHECK(n != NULL && strcmp(n, "/a/b") == 0, "normalize trims");
    fdk_free(n);
    n = fdk__path_normalize_dir("/");
    CHECK(n != NULL && strcmp(n, "/") == 0,
          "normalize keeps the root");
    fdk_free(n);
    n = fdk__path_normalize_dir("/a///");
    CHECK(n != NULL && strcmp(n, "/a") == 0,
          "normalize trims all trailing slashes");
    fdk_free(n);
    CHECK(fdk__path_normalize_dir("") == NULL, "empty normalizes to NULL");

    /* Tilde expansion. */
    const char *home = getenv("HOME");
    char *t = fdk__path_expand_tilde("~");
    CHECK(t != NULL && home != NULL && strcmp(t, home) == 0,
          "~ expands to HOME");
    fdk_free(t);
    t = fdk__path_expand_tilde("~/Documents");
    char *expect = fdk__path_join(home, "Documents");
    CHECK(t != NULL && strcmp(t, expect) == 0, "~/x expands");
    fdk_free(t);
    fdk_free(expect);
    t = fdk__path_expand_tilde("/abs");
    CHECK(t != NULL && strcmp(t, "/abs") == 0,
          "absolute path unchanged");
    fdk_free(t);
    t = fdk__path_expand_tilde("~user");
    CHECK(t != NULL && strcmp(t, "~user") == 0,
          "~user is passed through (no name-service lookup)");
    fdk_free(t);
}

static void test_save_name_validation(void) {
    CHECK(fdk__save_name_validate("notes.txt") == 0, "valid name");
    CHECK(fdk__save_name_validate("a b c.txt") == 0,
          "spaces are legal in names");
    CHECK(fdk__save_name_validate(".hidden") == 0,
          "dotfiles are legal save targets");
    CHECK(fdk__save_name_validate("") == 1, "empty rejected");
    CHECK(fdk__save_name_validate(NULL) == 1, "NULL rejected");
    CHECK(fdk__save_name_validate("  ") == 5, "whitespace-only");
    CHECK(fdk__save_name_validate("a/b.txt") == 2, "slash rejected");
    CHECK(fdk__save_name_validate(".") == 3, "dot rejected");
    CHECK(fdk__save_name_validate("..") == 3, "dotdot rejected");
    char big[300];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(fdk__save_name_validate(big) == 4, "255-byte cap enforced");
    big[255] = '\0';
    CHECK(fdk__save_name_validate(big) == 0, "255 bytes is legal");
}

static void test_result_free(void) {
    fdk_file_dialog_result r = {0};
    fdk_file_dialog_result_free(NULL); /* legal no-op */
    fdk_file_dialog_result_free(&r);   /* NULL paths: legal */
    r.paths = malloc(2 * sizeof(char *));
    r.paths[0] = strdup("/a");
    r.paths[1] = strdup("/b");
    r.count = 2;
    fdk_file_dialog_result_free(&r);
    CHECK(r.paths == NULL && r.count == 0, "result_free resets");
}

/* ---- recents engine (1.4.4) ---- */

/* Writes `text` to `path` (truncate). */
static void put_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

static void test_recents_parse(void) {
    /* A representative xbel: newest-first stamps, a percent-encoded
     * href with UTF-8 + spaces, a non-local URI skipped, a duplicate
     * href (the newest wins), and an out-of-order stamp proving the
     * mtime sort. */
    char path[512];
    snprintf(path, sizeof(path), "%s/recently-used.xbel", g_dir);
    put_file(path,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<xbel version=\"1.0\">\n"
        "  <bookmark href=\"file:///tmp/old.txt\" "
            "modified=\"2020-01-02T03:04:05Z\"/>\n"
        "  <bookmark href=\"file:///tmp/fdk%20docs/notes%20%C3%A9.txt\" "
            "modified=\"2026-10-09T12:00:00Z\"/>\n"
        "  <bookmark href=\"http://example.com/page\" "
            "modified=\"2026-10-09T13:00:00Z\"/>\n"
        "  <bookmark href=\"file:///tmp/old.txt\" "
            "modified=\"2026-10-09T14:00:00Z\"/>\n"
        "  <bookmark href=\"file:///tmp/abs-bare\" "
            "modified=\"2026-10-08T10:00:00Z\"/>\n"
        "</xbel>\n");

    fdk_fd_recent *v = NULL;
    size_t n = 0;
    CHECK(fdk__recent_load(path, &v, &n) == 0, "recents: load ok");
    CHECK(n == 3, "recents: 3 local entries (dup merged, http skipped)");
    if (n == 3) {
        /* Sorted desc by mtime: old.txt (14:00), bare (Oct 8 10:00),
         * notes é.txt (Oct 9 12:00)... wait — 2026-10-09 12:00 is
         * NEWER than 2026-10-08 10:00. Order: old(10-09 14:00),
         * notes(10-09 12:00), bare(10-08 10:00). */
        CHECK(strcmp(v[0].path, "/tmp/old.txt") == 0,
              "recents: newest-first head");
        CHECK(v[0].mtime > 1760000000LL, "recents: 2026 stamp epoch");
        CHECK(strcmp(v[1].path,
                     "/tmp/fdk docs/notes \xC3\xA9.txt") == 0,
              "recents: percent + UTF-8 decode");
        CHECK(strcmp(v[2].path, "/tmp/abs-bare") == 0,
              "recents: bare path accepted");
    }
    fdk__recent_free(v, n);

    /* Missing file: -1, empty out. */
    v = (fdk_fd_recent *)1;
    n = 7;
    CHECK(fdk__recent_load("/no/such/recently-used.xbel", &v, &n) == -1,
          "recents: missing file fails");
    CHECK(v == NULL && n == 0, "recents: failed load clears out");

    /* Not-an-xbel garbage: refused. */
    put_file(path, "definitely not xml\n");
    CHECK(fdk__recent_load(path, &v, &n) == -1,
          "recents: non-xbel refused");
    printf("[ok] recents parse: dedupe-newest, http skipped, "
           "percent+UTF-8 href decode, bare paths, mtime-desc sort, "
           "missing/garbage refused\n");
}

static void test_recents_touch(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/recently-used.xbel", g_dir);

    /* Fresh creation: touch a path with spaces, parse it back. */
    unlink(path);
    CHECK(fdk__recent_touch(path, "/tmp/my docs/report v2.txt") == 0,
          "recents: fresh touch ok");
    fdk_fd_recent *v = NULL;
    size_t n = 0;
    CHECK(fdk__recent_load(path, &v, &n) == 0 && n == 1,
          "recents: fresh file parses to 1");
    if (n == 1) {
        CHECK(strcmp(v[0].path, "/tmp/my docs/report v2.txt") == 0,
              "recents: URI round-trip (spaces + spaces back)");
        CHECK(v[0].mtime > 1760000000LL, "recents: fresh stamp is now");
    }
    fdk__recent_free(v, n);

    /* Second touch: moves to front, no duplicate. */
    CHECK(fdk__recent_touch(path, "/tmp/second.txt") == 0,
          "recents: second touch ok");
    CHECK(fdk__recent_load(path, &v, &n) == 0 && n == 2,
          "recents: two entries after second touch");
    if (n == 2) {
        CHECK(strcmp(v[0].path, "/tmp/second.txt") == 0,
              "recents: touched lands first");
        CHECK(strcmp(v[1].path, "/tmp/my docs/report v2.txt") == 0,
              "recents: previous entry survives");
    }
    fdk__recent_free(v, n);

    /* Re-touch the OLDER entry: it moves to the front, no dup. */
    CHECK(fdk__recent_touch(path, "/tmp/my docs/report v2.txt") == 0,
          "recents: re-touch ok");
    CHECK(fdk__recent_load(path, &v, &n) == 0 && n == 2,
          "recents: still two entries (dedupe)");
    if (n == 2) {
        CHECK(strcmp(v[0].path, "/tmp/my docs/report v2.txt") == 0,
              "recents: re-touched head");
    }
    fdk__recent_free(v, n);

    /* Foreign metadata survives byte-for-byte: a bookmark with an
     * info block stays intact except when ITSELF re-touched. */
    put_file(path,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<xbel version=\"1.0\">\n"
        "  <bookmark href=\"file:///tmp/foreign.txt\" "
            "added=\"2024-01-01T00:00:00Z\">\n"
        "    <info>\n"
        "      <metadata owner=\"gtk-3.0\">\n"
        "        <bookmark:applications>\n"
        "        </bookmark:applications>\n"
        "      </metadata>\n"
        "    </info>\n"
        "  </bookmark>\n"
        "</xbel>\n");
    CHECK(fdk__recent_touch(path, "/tmp/mine.txt") == 0,
          "recents: touch alongside foreign metadata");
    char buf[4096];
    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "recents: file readable after touch");
    if (f != NULL) {
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        fclose(f);
        CHECK(strstr(buf, "owner=\"gtk-3.0\"") != NULL,
              "recents: foreign metadata preserved");
        CHECK(strstr(buf, "/tmp/mine.txt") != NULL,
              "recents: mine present");
        CHECK(strstr(buf, "/tmp/mine.txt") < strstr(buf, "foreign.txt"),
              "recents: mine inserted before the older entry");
    }
    /* Re-touching the FOREIGN entry removes it whole (no dup) but
     * the metadata of OTHERS stays. */
    CHECK(fdk__recent_touch(path, "/tmp/mine.txt") == 0,
          "recents: touch again");
    CHECK(fdk__recent_load(path, &v, &n) == 0 && n == 2,
          "recents: foreign + mine, no dupes");
    fdk__recent_free(v, n);

    /* Argument safety. */
    CHECK(fdk__recent_touch(path, "relative.txt") == -1,
          "recents: relative path refused");
    CHECK(fdk__recent_touch(NULL, "/tmp/x") == -1,
          "recents: NULL xbel refused");

    unlink(path);
    printf("[ok] recents touch: fresh-file creation (data dir "
           "included), move-to-front, re-touch dedupe, URI "
           "round-trip, foreign metadata preserved byte-for-byte, "
           "relative paths refused\n");
}

static void test_recents_file_resolution(void) {
    /* XDG_DATA_HOME wins when absolute; otherwise HOME/.local/share;
     * no HOME -> false. */
    char buf[512];
    const char *saved_xdg = getenv("XDG_DATA_HOME");
    const char *saved_home = getenv("HOME");
    setenv("XDG_DATA_HOME", "/xdg-data", 1);
    CHECK(fdk__recent_file(buf, sizeof(buf)),
          "recents: XDG path resolves");
    CHECK(strcmp(buf, "/xdg-data/recently-used.xbel") == 0,
          "recents: XDG path exact");
    unsetenv("XDG_DATA_HOME");
    setenv("HOME", "/home/tester", 1);
    CHECK(fdk__recent_file(buf, sizeof(buf)),
          "recents: HOME fallback resolves");
    CHECK(strcmp(buf, "/home/tester/.local/share/recently-used.xbel") ==
              0,
          "recents: HOME fallback exact");
    unsetenv("HOME");
    CHECK(!fdk__recent_file(buf, sizeof(buf)),
          "recents: no home -> false");
    /* Restore. */
    if (saved_xdg != NULL) {
        setenv("XDG_DATA_HOME", saved_xdg, 1);
    }
    if (saved_home != NULL) {
        setenv("HOME", saved_home, 1);
    }
    printf("[ok] recents path: XDG_DATA_HOME first, HOME/.local/share "
           "fallback, no-home false\n");
}

int main(void) {
    make_scratch();
    test_scan_defaults();
    test_scan_hidden();
    test_scan_dirs_only();
    test_scan_unreadable();
    test_glob_match();
    test_parse_filters();
    test_scan_filtered();
    test_places();
    test_path_helpers();
    test_save_name_validation();
    test_result_free();
    test_recents_parse();
    test_recents_touch();
    test_recents_file_resolution();
    rm_scratch();

    if (failures != 0) {
        fprintf(stderr, "file dialog logic: %d failure(s)\n", failures);
        return 1;
    }
    printf("file dialog logic: all checks passed\n");
    return 0;
}
