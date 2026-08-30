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
    rm_scratch();

    if (failures != 0) {
        fprintf(stderr, "file dialog logic: %d failure(s)\n", failures);
        return 1;
    }
    printf("file dialog logic: all checks passed\n");
    return 0;
}
