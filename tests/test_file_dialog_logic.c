/*
 * test_file_dialog_logic.c — headless file-dialog logic (1.2.0)
 *
 * The interactive dialog is GUI-tested in the integration suites and
 * the examples rig. What CAN be pinned headlessly is the scan seam
 * the dialog is built on (widgets_internal.h): hidden filtering,
 * dirs-only filtering, dirs-first ordering, the "."/".." exclusion,
 * unreadable directories, the entries' ownership contract — plus the
 * result-model free function's tolerance.
 */

#include "fdk/fdk.h"
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
    snprintf(buf, sizeof(buf), "%s/beta.txt", d);
    FILE *f = fopen(buf, "w");
    if (f != NULL) {
        fputs("x", f);
        fclose(f);
    }
    snprintf(buf, sizeof(buf), "%s/.dotfile", d);
    f = fopen(buf, "w");
    if (f != NULL) {
        fputs("x", f);
        fclose(f);
    }
    g_dir = d;
}

static void rm_scratch(void) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/.dotfile", g_dir);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s/beta.txt", g_dir);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s/zeta", g_dir);
    rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/alpha", g_dir);
    rmdir(buf);
    snprintf(buf, sizeof(buf), "%s/.hidden", g_dir);
    rmdir(buf);
    rmdir(g_dir);
}

static void test_scan_defaults(void) {
    fdk_fd_entries e;
    CHECK(fdk__file_dialog_scan(g_dir, false, false, &e) == 0,
          "scan defaults ok");
    /* dirs first (alpha, zeta), then files (beta.txt); hidden gone;
     * no . or .. rows. */
    CHECK(e.count == 3, "default count hides dot entries");
    if (e.count == 3) {
        CHECK(strcmp(e.v[0].name, "alpha") == 0 && e.v[0].dir,
              "dirs sort first, alpha");
        CHECK(strcmp(e.v[1].name, "zeta") == 0 && e.v[1].dir,
              "zeta second");
        CHECK(strcmp(e.v[2].name, "beta.txt") == 0 && !e.v[2].dir,
              "file after dirs");
    }
    fdk__file_dialog_entries_free(&e);
    CHECK(e.v == NULL && e.count == 0, "entries_free clears");
}

static void test_scan_hidden(void) {
    fdk_fd_entries e;
    CHECK(fdk__file_dialog_scan(g_dir, false, true, &e) == 0,
          "scan hidden ok");
    CHECK(e.count == 5, "hidden count");
    if (e.count == 5) {
        /* .hidden and .dotfile present; . and .. never are. */
        bool have_hidden_dir = false, have_dotfile = false;
        for (size_t i = 0; i < e.count; i++) {
            CHECK(e.v[i].hidden ||
                      strcmp(e.v[i].name, "alpha") == 0 ||
                      strcmp(e.v[i].name, "zeta") == 0 ||
                      strcmp(e.v[i].name, "beta.txt") == 0,
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
    CHECK(fdk__file_dialog_scan(g_dir, true, true, &e) == 0,
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
                                false, &e) != 0,
          "unreadable dir reports failure");
    CHECK(e.v == NULL && e.count == 0,
          "failed scan leaves empty result");
    fdk__file_dialog_entries_free(&e);
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
    test_result_free();
    rm_scratch();

    if (failures != 0) {
        fprintf(stderr, "file dialog logic: %d failure(s)\n", failures);
        return 1;
    }
    printf("file dialog logic: all checks passed\n");
    return 0;
}
