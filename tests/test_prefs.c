/* test_prefs.c — headless preferences-store tests (1.3.7).
 *
 * Everything runs against temp-directory files and the
 * FDK_PREFS_FILE override: no display, no window, deterministic.
 * HOME / XDG_CONFIG_HOME / FDK_PREFS_FILE are sandboxed for the
 * whole binary (saved in main, restored in the same block, torn
 * down per group by the env_* helpers).
 *
 * What is proven here:
 *   - argument safety on every entry point (NULL store, NULL /
 *     malformed keys, NULL app ids)
 *   - the "section.name" key grammar: every acceptance and every
 *     rejection rule (dots, lengths, character classes)
 *   - path resolution: FDK_PREFS_FILE absolute override, relative
 *     override ignored, XDG_CONFIG_HOME, HOME fallback, memory-only
 *     when nothing resolves (save() = FDK_ERR_UNSUPPORTED, honestly)
 *   - the parser: the full acceptance surface (comments both
 *     spellings, blanks, leading whitespace, CRLF, empty values,
 *     UTF-8 values, interleaved/repeating sections, values with
 *     internal spaces) and the full rejection matrix from
 *     docs/fdk-prefs-format.md (key before section, duplicates,
 *     bad names, over-long names/values/lines, control characters,
 *     invalid UTF-8, garbage syntax) — each rejection asserts
 *     RECOVERED + an EMPTY store + defaults from every getter
 *     (the resilience rule: a dead file must not kill the app)
 *   - typed access: exact strtol/strtod/bool-table semantics, the
 *     default-on-unparsable contract, and the "one DEBUG line on
 *     first read, not per call" warn-once contract (captured sink)
 *   - setters: validation, replacement, the 8192-key and 256-byte
 *     value bounds
 *   - persistence: exact save() bytes (the golden file), the
 *     bit-for-bit set/save/reopen/get round trip, CRLF
 *     normalization on rewrite, atomic-rename residue checks
 *   - the exact dirty flag: an unchanged store save()s without
 *     touching the disk (proven by inode identity — rename always
 *     mints a new inode), set-back-to-file-value returns to clean,
 *     and a fresh store never creates a file at all
 *   - remove() semantics (quiet no-op on absent keys, gone after
 *     save) and iteration order (section-then-insertion, the
 *     save() layout, NULL past the end)
 */

#include "fdk/fdk.h"
#include "fdk/fdk_prefs.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- helpers ---------------------------------------------------------- */

static char g_dir[256]; /* one mkdtemp sandbox for the whole binary */

static char *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    buf[n] = '\0';
    if (len_out != NULL) {
        *len_out = (size_t)n;
    }
    return buf;
}

static void write_file(const char *name, const char *data) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(data, 1, strlen(data), f) == strlen(data));
    fclose(f);
}

/* Points FDK_PREFS_FILE at <g_dir>/<name> (a fresh file the group
 * writes with write_file()). */
static void use_file(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    setenv("FDK_PREFS_FILE", path, 1);
}

/* ---- env sandboxing ---------------------------------------------------
 * The whole binary sandboxes HOME / XDG_CONFIG_HOME / FDK_PREFS_FILE;
 * main() saves the originals and restores them before exit so the
 * battery's other binaries see the ambient environment. */

static char *g_home, *g_xdg, *g_override;

static void env_reset(void) {
    unsetenv("FDK_PREFS_FILE");
    unsetenv("XDG_CONFIG_HOME");
    /* A stable HOME that is NOT the user's: path-resolution tests
     * need to observe what the store picked, not inherit it. */
    setenv("HOME", g_dir, 1);
}

/* ---- log capture (the warn-once contract) ------------------------------ */

static char g_log_lines[8][256];
static int g_log_n;

static void capture_sink(fdk_log_level level, const char *tag,
                         const char *msg, void *ud) {
    (void)ud;
    if (g_log_n < 8 && strcmp(tag, "prefs") == 0) {
        snprintf(g_log_lines[g_log_n], sizeof g_log_lines[g_log_n],
                 "[%d] %s", (int)level, msg);
        g_log_n++;
    }
}

static void log_capture_on(void) {
    g_log_n = 0;
    fdk_log_set_sink(capture_sink, NULL);
    fdk_log_set_level(FDK_LOG_DEBUG);
}

static void log_capture_off(void) {
    fdk_log_set_sink(NULL, NULL);
    fdk_log_set_level(FDK_LOG_INFO);
}

/* ---- inode identity (the no-op save proof) -----------------------------
 * rename(2) always mints a new inode; an untouched disk leaves the
 * old one. mtime alone is too coarse on tmpfs. */

static ino_t inode_of(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return (ino_t)0; /* no file */
    }
    return st.st_ino;
}

static int tmp_residue(void) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s/*.tmp*", g_dir);
    /* glob without executing: opendir walk */
    (void)cmd;
    DIR *d = opendir(g_dir);
    if (d == NULL) {
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strstr(e->d_name, ".tmp") != NULL) {
            n++;
        }
    }
    closedir(d);
    return n;
}

/* ---- group: argument safety ------------------------------------------- */

static void test_argument_safety(void) {
    fdk_prefs *p = NULL;
    assert(fdk_prefs_open(NULL, &p) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_prefs_open("", &p) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_prefs_open("app", NULL) == FDK_ERR_INVALID_ARGUMENT);
    assert(p == NULL);

    /* Every getter: NULL store serves the default, never crashes. */
    assert(fdk_prefs_get(NULL, "a.b", "d") != NULL);
    assert(strcmp(fdk_prefs_get(NULL, "a.b", "d"), "d") == 0);
    assert(fdk_prefs_get_int(NULL, "a.b", 7) == 7);
    assert(fdk_prefs_get_bool(NULL, "a.b", true) == true);
    assert(fdk_prefs_get_double(NULL, "a.b", 0.5) == 0.5);
    assert(fdk_prefs_count(NULL) == 0);
    assert(fdk_prefs_key_at(NULL, 0) == NULL);
    assert(fdk_prefs_value_at(NULL, 0) == NULL);
    assert(fdk_prefs_path(NULL) == NULL);
    assert(fdk_prefs_source(NULL) == FDK_PREFS_SOURCE_FRESH);

    /* Every setter: NULL store / NULL key is an error, not a crash. */
    assert(fdk_prefs_set(NULL, "a.b", "v") ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_prefs_set_int(NULL, "a.b", 1) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_prefs_set_bool(NULL, "a.b", true) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_prefs_set_double(NULL, "a.b", 1.0) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_prefs_remove(NULL, "a.b") ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_prefs_save(NULL) == FDK_ERR_INVALID_ARGUMENT);

    fdk_prefs_destroy(NULL); /* documented no-op */

    env_reset();
    use_file("args.prefs");
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_set(p, NULL, "v") == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_prefs_get(p, NULL, "d") != NULL);
    assert(strcmp(fdk_prefs_get(p, NULL, "d"), "d") == 0);
    assert(fdk_prefs_set(p, "a.b", NULL) == FDK_OK); /* NULL = "" */
    assert(strcmp(fdk_prefs_get(p, "a.b", "d"), "") == 0);
    fdk_prefs_destroy(p);
    printf("[ok] prefs: argument safety (every NULL path is either "
           "the documented default or FDK_ERR_INVALID_ARGUMENT)\n");
}

/* ---- group: key grammar ------------------------------------------------ */

static void test_key_grammar(void) {
    env_reset();
    use_file("keys.prefs");
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("app", &p)));

    static const char *good[] = {
        "a.b",       "A.B",       "Window.width", "x-y.z_9",
        "_._",       "0.0",       "Sec-tion.Name_9",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        assert(fdk_ok(fdk_prefs_set(p, good[i], "v")));
        assert(strcmp(fdk_prefs_get(p, good[i], "d"), "v") == 0);
    }

    /* Length bounds: 64 is legal (both halves), 65 is not. Built
     * programmatically — hand-counted literals lie (found live). */
    char sec64[65], name64[65], k64[160];
    memset(sec64, 's', 64);
    sec64[64] = '\0';
    memset(name64, 'n', 64);
    name64[64] = '\0';
    snprintf(k64, sizeof k64, "%s.%s", sec64, name64);
    assert(strlen(k64) == 64 + 1 + 64);
    assert(fdk_ok(fdk_prefs_set(p, k64, "v")));
    assert(strcmp(fdk_prefs_get(p, k64, "d"), "v") == 0);

    static const char *bad[] = {
        "ab",        /* no dot */
        "a.b.c",     /* two dots */
        ".b",        /* empty section */
        "a.",        /* empty name */
        "a.b.",      /* trailing dot */
        ".b.c",      /* leading dot + second dot */
        "a b.c",     /* space */
        "a.b/c",     /* slash */
        "a.b:c",     /* colon */
        "a.\xc3\xa9", /* UTF-8 name (foreign class) */
        "\xc3\xa9.b", /* UTF-8 section */
        "a..b",      /* double dot */
        "a.b c",     /* space in name half */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        assert(fdk_prefs_set(p, bad[i], "v") ==
               FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_prefs_get(p, bad[i], "d") != NULL);
        assert(strcmp(fdk_prefs_get(p, bad[i], "d"), "d") == 0);
        assert(fdk_prefs_get_int(p, bad[i], 9) == 9);
    }

    /* Over-length halves: 65 chars each. */
    char sec65[80], name65[80];
    memset(sec65, 'a', 65);
    sec65[65] = '\0';
    memset(name65, 'n', 65);
    name65[65] = '\0';
    char k65[200];
    snprintf(k65, sizeof k65, "%s.ok", sec65);
    assert(fdk_prefs_set(p, k65, "v") == FDK_ERR_INVALID_ARGUMENT);
    snprintf(k65, sizeof k65, "ok.%s", name65);
    assert(fdk_prefs_set(p, k65, "v") == FDK_ERR_INVALID_ARGUMENT);

    fdk_prefs_destroy(p);
    printf("[ok] prefs: key grammar (one dot, 1..64 chars, "
           "[A-Za-z0-9_-] halves; everything else rejected)\n");
}

/* ---- group: path resolution -------------------------------------------- */

static void test_path_resolution(void) {
    fdk_prefs *p = NULL;

    /* 1. Absolute FDK_PREFS_FILE wins over everything. */
    env_reset();
    use_file("resolve-a.prefs");
    setenv("XDG_CONFIG_HOME", "/nonexistent-xdg", 1);
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(p != NULL);
    assert(fdk_prefs_path(p) != NULL);
    assert(strstr(fdk_prefs_path(p), "resolve-a.prefs") != NULL);
    fdk_prefs_destroy(p);

    /* 2. A RELATIVE override is ignored — falls through to XDG. */
    env_reset();
    setenv("FDK_PREFS_FILE", "relative/ignored.prefs", 1);
    setenv("XDG_CONFIG_HOME", g_dir, 1);
    assert(fdk_ok(fdk_prefs_open("myapp", &p)));
    char want[512];
    snprintf(want, sizeof want, "%s/myapp.prefs", g_dir);
    assert(strcmp(fdk_prefs_path(p), want) == 0);
    fdk_prefs_destroy(p);

    /* 3. XDG must be absolute to count; relative XDG falls to HOME. */
    env_reset();
    unsetenv("FDK_PREFS_FILE");
    setenv("XDG_CONFIG_HOME", "relative-xdg", 1);
    assert(fdk_ok(fdk_prefs_open("myapp", &p)));
    snprintf(want, sizeof want, "%s/.config/myapp.prefs", g_dir);
    assert(strcmp(fdk_prefs_path(p), want) == 0);
    fdk_prefs_destroy(p);

    /* 4. No XDG: plain HOME/.config. */
    env_reset();
    assert(fdk_ok(fdk_prefs_open("myapp", &p)));
    assert(fdk_prefs_path(p) != NULL);
    assert(strstr(fdk_prefs_path(p), "/.config/myapp.prefs") !=
           NULL);
    fdk_prefs_destroy(p);

    /* 5. Nothing resolves: memory-only, and save() says so instead
     * of inventing a location. */
    env_reset();
    unsetenv("HOME");
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_path(p) == NULL);
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FRESH);
    assert(fdk_prefs_count(p) == 0);
    assert(fdk_ok(fdk_prefs_set(p, "a.b", "v")));
    assert(strcmp(fdk_prefs_get(p, "a.b", "d"), "v") == 0);
    assert(fdk_prefs_save(p) == FDK_ERR_UNSUPPORTED);
    fdk_prefs_destroy(p);

    printf("[ok] prefs: path resolution (override > XDG > HOME > "
           "honest memory-only)\n");
}

/* ---- group: parser acceptance ------------------------------------------ */

static void test_parse_acceptance(void) {
    fdk_prefs *p = NULL;

    /* The full acceptance surface in one file. */
    write_file("accept.prefs",
               "# leading comment\n"
               "// another comment spelling\n"
               "\n"
               "   [Window]\n"
               "   width = 880\n"
               "height =540\n"
               "tight= 27\n"
               "loose   =  28\n"
               "title = FDK Files — persistence demo\n"
               "empty =\n"
               "spaced =  value with  inner spaces  \n"
               "\n"
               "[ui]\n"
               "dark = true\n"
               "\n"
               "[Window]\n" /* repeated section: rows keep file order */
               "x = -12\n");
    use_file("accept.prefs");
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FILE);
    assert(fdk_prefs_count(p) == 9);
    assert(fdk_prefs_get_int(p, "Window.width", 0) == 880);
    assert(fdk_prefs_get_int(p, "Window.height", 0) == 540);
    assert(fdk_prefs_get_int(p, "Window.tight", 0) == 27);
    assert(fdk_prefs_get_int(p, "Window.loose", 0) == 28);
    assert(strcmp(fdk_prefs_get(p, "Window.title", "d"),
                  "FDK Files — persistence demo") == 0);
    assert(strcmp(fdk_prefs_get(p, "Window.empty", "SET"), "") == 0);
    /* Inner spaces survive; the file's trailing double-space is
     * trimmed like every sibling format trims. */
    assert(strcmp(fdk_prefs_get(p, "Window.spaced", "d"),
                  "value with  inner spaces") == 0);
    assert(fdk_prefs_get_bool(p, "ui.dark", false) == true);
    assert(fdk_prefs_get_int(p, "Window.x", 999) == -12);
    /* Order: file order, interleaved section repeated in place. */
    assert(strcmp(fdk_prefs_key_at(p, 0), "Window.width") == 0);
    assert(strcmp(fdk_prefs_key_at(p, 4), "Window.title") == 0);
    assert(strcmp(fdk_prefs_key_at(p, 7), "ui.dark") == 0);
    assert(strcmp(fdk_prefs_key_at(p, 8), "Window.x") == 0);
    fdk_prefs_destroy(p);

    /* CRLF throughout parses identically. */
    write_file("crlf.prefs",
               "[w]\r\na = 1\r\nb = two\r\n");
    use_file("crlf.prefs");
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FILE);
    assert(fdk_prefs_count(p) == 2);
    assert(fdk_prefs_get_int(p, "w.a", 0) == 1);
    assert(strcmp(fdk_prefs_get(p, "w.b", "d"), "two") == 0);
    fdk_prefs_destroy(p);

    /* Comments-only and blank-only files are legal (FILE, empty). */
    write_file("comments.prefs", "# nothing but comments\n\n// end\n");
    use_file("comments.prefs");
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FILE);
    assert(fdk_prefs_count(p) == 0);
    fdk_prefs_destroy(p);

    /* Empty file: documented first-run-with-residue (FRESH). */
    write_file("empty.prefs", "");
    use_file("empty.prefs");
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FRESH);
    assert(fdk_prefs_count(p) == 0);
    fdk_prefs_destroy(p);

    /* The missing file is the clean first run. */
    use_file("never-written.prefs");
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FRESH);
    assert(fdk_prefs_count(p) == 0);
    assert(fdk_prefs_get_int(p, "a.b", 42) == 42);
    fdk_prefs_destroy(p);

    printf("[ok] prefs: parser acceptance (comments, blanks, CRLF, "
           "empty values, UTF-8, interleaved sections, file order)\n");
}

/* ---- group: parser rejection (the resilience rule) ---------------------
 * Every rejection: FDK_OK from open (never kills the app), source
 * RECOVERED, an EMPTY store, defaults from every getter. */

static void expect_rejected(const char *name, const char *body) {
    fdk_prefs *p = NULL;
    write_file(name, body);
    use_file(name);
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_RECOVERED);
    assert(fdk_prefs_count(p) == 0);
    assert(fdk_prefs_get_int(p, "a.b", 42) == 42);
    assert(fdk_prefs_get_bool(p, "a.b", true) == true);
    assert(fdk_prefs_get_double(p, "a.b", 0.25) == 0.25);
    assert(strcmp(fdk_prefs_get(p, "a.b", "dflt"), "dflt") == 0);
    fdk_prefs_destroy(p);
}

static void test_parse_rejection(void) {
    env_reset();
    expect_rejected("r1.prefs", "key = before any section\n");
    expect_rejected("r2.prefs", "[a]\nk = 1\nk = 2\n");
    expect_rejected("r3.prefs", "[a]\nk = 1\n[b]\nk = 2\n[a]\nk = 3\n");
    expect_rejected("r4.prefs", "[a]\nbad key! = 1\n");
    expect_rejected("r5.prefs", "[a]\n= 1\n");
    expect_rejected("r6.prefs", "[a]\nk\n"); /* no '=' at all */
    expect_rejected("r7.prefs", "[a]\nk = v\x01x\n"); /* control char */
    expect_rejected("r8.prefs", "[unclosed\nk = 1\n");
    expect_rejected("r9.prefs", "[a] trailing junk\nk = 1\n");
    expect_rejected("r10.prefs", "[]\nk = 1\n"); /* empty section */
    expect_rejected("r11.prefs", "[a.]\nk = 1\n"); /* dot in section */
    expect_rejected("r12.prefs", "[a]\nk = \xc3\n"); /* truncated UTF-8 */
    expect_rejected("r13.prefs",
                    "[a]\nk = \xe6\x97\n"); /* 日 cut mid-run */

    /* Over-long pieces. */
    char longname[512], longsec[512], longval[600], line[1200];
    memset(longname, 'n', 65);
    longname[65] = '\0';
    snprintf(line, sizeof line, "[a]\n%s = 1\n", longname);
    expect_rejected("r14.prefs", line);
    memset(longsec, 's', 65);
    longsec[65] = '\0';
    snprintf(line, sizeof line, "[%s]\nk = 1\n", longsec);
    expect_rejected("r15.prefs", line);
    memset(longval, 'v', 257);
    longval[257] = '\0';
    snprintf(line, sizeof line, "[a]\nk = %s\n", longval);
    expect_rejected("r16.prefs", line);
    /* A 1025-byte line (1024 is the cap, plus the newline). */
    memset(line, 'x', 1025);
    line[1025] = '\0';
    char body[2200];
    snprintf(body, sizeof body, "[a]\n%s\n", line);
    expect_rejected("r17.prefs", body);

    /* The 1 MiB file bound: a rejected (not crashed) store. */
    {
        FILE *f = fopen("/dev/null", "w");
        (void)f;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", g_dir, "r18.prefs");
        f = fopen(path, "wb");
        assert(f != NULL);
        char chunk[4096];
        memset(chunk, 'v', sizeof chunk);
        for (int i = 0; i < 300; i++) { /* 300 * 4096 > 1 MiB */
            assert(fwrite(chunk, 1, sizeof chunk, f) == sizeof chunk);
        }
        fclose(f);
        use_file("r18.prefs");
        fdk_prefs *p = NULL;
        assert(fdk_ok(fdk_prefs_open("app", &p)));
        assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_RECOVERED);
        assert(fdk_prefs_count(p) == 0);
        fdk_prefs_destroy(p);
    }

    printf("[ok] prefs: parser rejection matrix (18 corrupt files, "
           "each -> empty store + defaults; the app survives)\n");
}

/* ---- group: typed access + warn-once ------------------------------------ */

static void test_typed_access(void) {
    env_reset();
    use_file("typed.prefs");
    write_file("typed.prefs",
               "[i]\npos = 42\nneg = -7\nplus = +9\n"
               "space = 12\nhex = 0x10\njunk = 12x\n"
               "alpha = x12\nemptyv =\nhuge = 99999999999999999999\n"
               "[b]\ny = true\nn = false\none = 1\nzero = 0\n"
               "yes = yes\nupper = TRUE\n"
               "[d]\nhalf = 0.5\nneg = -0.25\nexp = 1e3\n"
               "nan-ish = abc\n"
               "[s]\nplain = hello\n");
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("app", &p)));

    /* The warn-once contract FIRST (any earlier read of an
     * unparsable value arms the flag silently — order matters):
     * ONE DEBUG line on FIRST read, silence after (a getter on a
     * render-loop path must not spam). */
    log_capture_on();
    assert(fdk_prefs_get_int(p, "i.junk", -1) == -1);
    assert(fdk_prefs_get_int(p, "i.junk", -1) == -1);
    assert(fdk_prefs_get_int(p, "i.junk", -1) == -1);
    assert(g_log_n == 1);
    assert(strstr(g_log_lines[0], "i.junk") != NULL);
    assert(strstr(g_log_lines[0], "int") != NULL);
    /* The bool and double flavors each log their own single line. */
    assert(fdk_prefs_get_bool(p, "b.yes", true) == true);
    assert(fdk_prefs_get_bool(p, "b.yes", true) == true);
    assert(fdk_prefs_get_double(p, "d.nan-ish", -1) == -1);
    assert(fdk_prefs_get_double(p, "d.nan-ish", -1) == -1);
    assert(g_log_n == 3);

    /* int: exact strtol semantics (capture still on: clean parses
     * and absent keys must never log). */
    assert(fdk_prefs_get_int(p, "i.pos", -1) == 42);
    assert(fdk_prefs_get_int(p, "i.neg", -1) == -7);
    assert(fdk_prefs_get_int(p, "i.plus", -1) == 9);
    assert(fdk_prefs_get_int(p, "i.space", -1) == 12); /* strtol skips */
    assert(fdk_prefs_get_int(p, "i.hex", -1) == -1); /* strtol base 10 stops at 'x' */
    assert(fdk_prefs_get_int(p, "i.junk", -1) == -1);
    assert(fdk_prefs_get_int(p, "i.alpha", -1) == -1);
    assert(fdk_prefs_get_int(p, "i.emptyv", -1) == -1);
    assert(fdk_prefs_get_int(p, "i.huge", -1) == -1); /* ERANGE */
    assert(fdk_prefs_get_int(p, "i.missing", -1) == -1);

    /* bool: exactly the four legal spellings. */
    assert(fdk_prefs_get_bool(p, "b.y", false) == true);
    assert(fdk_prefs_get_bool(p, "b.n", true) == false);
    assert(fdk_prefs_get_bool(p, "b.one", false) == true);
    assert(fdk_prefs_get_bool(p, "b.zero", true) == false);
    assert(fdk_prefs_get_bool(p, "b.yes", true) == true);
    assert(fdk_prefs_get_bool(p, "b.upper", false) == false);

    /* double: whatever strtod accepts, whole value. */
    assert(fdk_prefs_get_double(p, "d.half", -1) == 0.5);
    assert(fdk_prefs_get_double(p, "d.neg", -1) == -0.25);
    assert(fdk_prefs_get_double(p, "d.exp", -1) == 1000.0);
    assert(fdk_prefs_get_double(p, "d.nan-ish", -1) == -1);
    assert(fdk_prefs_get_double(p, "d.missing", -1) == -1);

    /* The capture stayed on through the semantics reads above, so
     * the final count proves warn-once across the WHOLE group:
     * seven distinct unparsable values (hex, junk, alpha, huge,
     * yes, upper, nan-ish), several of them read three or four
     * times — exactly seven DEBUG lines, one per key, never one
     * per call. Clean parses (i.pos, d.half, ...) and absent keys
     * (i.missing, d.missing) never log at all. */
    assert(fdk_prefs_get_int(p, "i.missing", 5) == 5);
    assert(fdk_prefs_get_int(p, "i.pos", 0) == 42);
    assert(g_log_n == 7);
    log_capture_off();

    fdk_prefs_destroy(p);
    printf("[ok] prefs: typed access (strtol/strtod/bool-table "
           "semantics) + warn-once (7 unparsable keys, 15+ reads, "
           "exactly 7 DEBUG lines)\n");
}

/* ---- group: setters + the save round trip ------------------------------- */

static void test_setters_round_trip(void) {
    env_reset();
    use_file("roundtrip.prefs");
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("rt", &p)));

    /* 256 bytes is the exact value bound. */
    char v256[300], v257[300];
    memset(v256, 'a', 256);
    v256[256] = '\0';
    memset(v257, 'b', 257);
    v257[257] = '\0';
    assert(fdk_ok(fdk_prefs_set(p, "a.max", v256)));
    assert(fdk_prefs_set(p, "a.over", v257) == FDK_ERR_LIMIT);

    /* Typed setters format exactly as the getters parse. */
    assert(fdk_ok(fdk_prefs_set_int(p, "win.width", 880)));
    assert(fdk_ok(fdk_prefs_set_int(p, "win.x", -12)));
    assert(fdk_ok(fdk_prefs_set_bool(p, "ui.dark", true)));
    assert(fdk_ok(fdk_prefs_set_bool(p, "ui.hidden", false)));
    assert(fdk_ok(fdk_prefs_set_double(p, "ui.scale", 1.25)));
    assert(fdk_ok(fdk_prefs_set_double(p, "ui.zeta", -0.0625)));
    assert(fdk_ok(fdk_prefs_set(p, "ui.name", "persistent é value")));
    assert(fdk_ok(fdk_prefs_set(p, "ui.blank", "")));
    assert(fdk_ok(fdk_prefs_save(p)));
    fdk_prefs_destroy(p);

    /* Reopen: bit-for-bit. */
    assert(fdk_ok(fdk_prefs_open("rt", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FILE);
    assert(fdk_prefs_get_int(p, "win.width", 0) == 880);
    assert(fdk_prefs_get_int(p, "win.x", 0) == -12);
    assert(fdk_prefs_get_bool(p, "ui.dark", false) == true);
    assert(fdk_prefs_get_bool(p, "ui.hidden", true) == false);
    assert(fdk_prefs_get_double(p, "ui.scale", 0.0) == 1.25);
    assert(fdk_prefs_get_double(p, "ui.zeta", 0.0) == -0.0625);
    assert(strcmp(fdk_prefs_get(p, "ui.name", "d"),
                  "persistent é value") == 0);
    assert(strcmp(fdk_prefs_get(p, "ui.blank", "SET"), "") == 0);
    assert(fdk_prefs_count(p) == 9); /* a.max + 8 typed/typed-set keys */

    /* Replacement: set over an existing key keeps position. */
    assert(fdk_ok(fdk_prefs_set_int(p, "win.width", 1024)));
    assert(strcmp(fdk_prefs_key_at(p, 1), "win.width") == 0);
    assert(fdk_prefs_count(p) == 9);

    /* A second save + reopen keeps the replacement. */
    assert(fdk_ok(fdk_prefs_save(p)));
    fdk_prefs_destroy(p);
    assert(fdk_ok(fdk_prefs_open("rt", &p)));
    assert(fdk_prefs_get_int(p, "win.width", 0) == 1024);
    assert(fdk_prefs_count(p) == 9);
    fdk_prefs_destroy(p);
    printf("[ok] prefs: setter formatting + save/reopen round trip "
           "(bit-for-bit, order stable, 256/257 value bound)\n");
}

/* ---- group: exact save bytes (the golden file) --------------------------- */

static void test_save_golden(void) {
    env_reset();
    use_file("golden.prefs");
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("gold", &p)));
    assert(fdk_ok(fdk_prefs_set_int(p, "win.width", 880)));
    assert(fdk_ok(fdk_prefs_set_int(p, "win.height", 540)));
    assert(fdk_ok(fdk_prefs_set_bool(p, "ui.dark", true)));
    assert(fdk_ok(fdk_prefs_set(p, "ui.path", "/tmp")));
    assert(fdk_ok(fdk_prefs_save(p)));
    fdk_prefs_destroy(p);

    char path[512];
    snprintf(path, sizeof path, "%s/golden.prefs", g_dir);
    size_t len = 0;
    char *got = slurp(path, &len);
    assert(got != NULL);
    static const char want[] =
        "# FDK preferences v1 — gold\n"
        "# format: docs/fdk-prefs-format.md (human-editable)\n"
        "\n"
        "[win]\n"
        "width = 880\n"
        "height = 540\n"
        "[ui]\n"
        "dark = true\n"
        "path = /tmp\n";
    assert(len == sizeof want - 1);
    assert(memcmp(got, want, len) == 0);
    free(got);
    assert(tmp_residue() == 0);
    printf("[ok] prefs: save() writes the exact golden bytes "
           "(header, sections in first-appearance order, no residue)\n");
}

/* ---- group: the exact dirty flag ------------------------------------------
 * rename(2) always mints a new inode; an untouched disk keeps the
 * old one. That identity is the proof that "unchanged save is a
 * no-op" REALLY skipped the write (mtime alone is too coarse). */

static void test_dirty_exactness(void) {
    env_reset();
    char path[512];

    /* A FRESH store never creates a file: nothing was ever set. */
    use_file("fresh.prefs");
    snprintf(path, sizeof path, "%s/fresh.prefs", g_dir);
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_ok(fdk_prefs_save(p)));
    assert(inode_of(path) == 0); /* no file, no litter */
    assert(fdk_ok(fdk_prefs_set(p, "a.b", "v")));
    assert(fdk_ok(fdk_prefs_save(p)));
    ino_t ino1 = inode_of(path);
    assert(ino1 != 0);
    fdk_prefs_destroy(p);

    /* Unchanged reopen + save: inode identical (no disk touch). */
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FILE);
    assert(fdk_ok(fdk_prefs_save(p)));
    assert(inode_of(path) == ino1);
    assert(tmp_residue() == 0);

    /* Set to the SAME value the file carries: still clean. */
    assert(fdk_ok(fdk_prefs_set(p, "a.b", "v")));
    assert(fdk_ok(fdk_prefs_save(p)));
    assert(inode_of(path) == ino1);

    /* Set away, then set BACK before any save: the row is clean
     * again (value == file_value), the save is a no-op, the inode
     * does not move. This is the documented exactness — an edit
     * reverted before a save leaves no disk trace. */
    assert(fdk_ok(fdk_prefs_set(p, "a.b", "w")));
    assert(fdk_ok(fdk_prefs_set(p, "a.b", "v")));
    assert(fdk_ok(fdk_prefs_save(p)));
    assert(inode_of(path) == ino1);
    assert(tmp_residue() == 0);

    /* A real change mints the new inode. */
    assert(fdk_ok(fdk_prefs_set(p, "a.b", "w")));
    assert(fdk_ok(fdk_prefs_save(p)));
    ino_t ino2 = inode_of(path);
    assert(ino2 != 0 && ino2 != ino1);
    assert(tmp_residue() == 0);

    /* After that save the file holds "w": setting back to "v" is a
     * genuine change against the CURRENT file (not a no-op) and
     * saves a third inode. */
    assert(fdk_ok(fdk_prefs_set(p, "a.b", "v")));
    assert(fdk_ok(fdk_prefs_save(p)));
    ino_t ino3 = inode_of(path);
    assert(ino3 != 0 && ino3 != ino2);
    assert(tmp_residue() == 0);
    fdk_prefs_destroy(p);

    /* A second store reading the SAME file sees the new value. */
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(strcmp(fdk_prefs_get(p, "a.b", "d"), "v") == 0);
    fdk_prefs_destroy(p);
    printf("[ok] prefs: exact dirty flag (unchanged save skips the "
           "disk — inode-proven; set-back-to-file-value is clean)\n");
}

/* ---- group: remove + iteration -------------------------------------------- */

static void test_remove_iteration(void) {
    env_reset();
    use_file("remove.prefs");
    write_file("remove.prefs",
               "[a]\none = 1\ntwo = 2\n[b]\nthree = 3\n");
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_count(p) == 3);

    /* Absent key: the quiet no-op. */
    assert(fdk_ok(fdk_prefs_remove(p, "a.nope")));
    assert(fdk_ok(fdk_prefs_remove(p, "zzz.nope")));
    assert(fdk_prefs_count(p) == 3);

    assert(fdk_ok(fdk_prefs_remove(p, "a.two")));
    assert(fdk_prefs_count(p) == 2);
    assert(fdk_prefs_get_int(p, "a.two", -1) == -1);
    assert(fdk_prefs_remove(p, "bad key") ==
           FDK_ERR_INVALID_ARGUMENT); /* malformed key: loud */
    assert(fdk_ok(fdk_prefs_save(p)));
    fdk_prefs_destroy(p);

    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_count(p) == 2);
    assert(fdk_prefs_get_int(p, "a.one", -1) == 1);
    assert(fdk_prefs_get_int(p, "b.three", -1) == 3);

    /* A removed-then-re-added key lands at the END (insertion
     * order — the honest documented semantics). */
    assert(fdk_ok(fdk_prefs_set_int(p, "a.two", 22)));
    assert(fdk_prefs_count(p) == 3);
    assert(strcmp(fdk_prefs_key_at(p, 2), "a.two") == 0);
    assert(fdk_ok(fdk_prefs_save(p)));
    fdk_prefs_destroy(p);

    /* Iteration surface: order, values, NULL past the end. */
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(strcmp(fdk_prefs_key_at(p, 0), "a.one") == 0);
    assert(strcmp(fdk_prefs_value_at(p, 0), "1") == 0);
    assert(strcmp(fdk_prefs_key_at(p, 1), "b.three") == 0);
    assert(strcmp(fdk_prefs_key_at(p, 2), "a.two") == 0);
    assert(strcmp(fdk_prefs_value_at(p, 2), "22") == 0);
    assert(fdk_prefs_key_at(p, 3) == NULL);
    assert(fdk_prefs_value_at(p, 99) == NULL);
    fdk_prefs_destroy(p);
    printf("[ok] prefs: remove (quiet no-op, persisted) + iteration "
           "order (re-add lands at the end, NULL past the end)\n");
}

/* ---- group: the 8192-key bound --------------------------------------------- */

static void test_key_limit(void) {
    env_reset();
    use_file("limit.prefs");
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    char key[80];
    for (int i = 0; i < 8192; i++) {
        snprintf(key, sizeof key, "k%d.v", i);
        assert(fdk_ok(fdk_prefs_set_int(p, key, i)));
    }
    assert(fdk_prefs_count(p) == 8192);
    snprintf(key, sizeof key, "k0.v");
    assert(fdk_prefs_get_int(p, key, -1) == 0);
    snprintf(key, sizeof key, "k8191.v");
    assert(fdk_prefs_get_int(p, key, -1) == 8191);
    /* One past the bound: refused, store unchanged. */
    snprintf(key, sizeof key, "k8192.v");
    assert(fdk_prefs_set_int(p, key, 1) == FDK_ERR_LIMIT);
    assert(fdk_prefs_count(p) == 8192);
    assert(fdk_prefs_get_int(p, key, -1) == -1);
    /* Removal frees budget again. */
    assert(fdk_ok(fdk_prefs_remove(p, "k0.v")));
    assert(fdk_ok(fdk_prefs_set_int(p, key, 1)));
    assert(fdk_prefs_get_int(p, key, -1) == 1);
    fdk_prefs_destroy(p);
    printf("[ok] prefs: the 8192-key bound (accepted to the edge, "
           "refused past it, removal frees budget)\n");
}

/* ---- group: CRLF normalization + corrupt-file recovery rewrite ------------- */

static void test_rewrite_recover(void) {
    env_reset();
    use_file("norm.prefs");
    write_file("norm.prefs", "[w]\r\na = 1\r\n");
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_ok(fdk_prefs_set_int(p, "w.b", 2))); /* dirties */
    assert(fdk_ok(fdk_prefs_save(p)));
    fdk_prefs_destroy(p);

    char path[512];
    snprintf(path, sizeof path, "%s/norm.prefs", g_dir);
    size_t len = 0;
    char *got = slurp(path, &len);
    assert(got != NULL);
    assert(memchr(got, '\r', len) == NULL); /* pure LF out */
    assert(strstr(got, "a = 1\n") != NULL);
    assert(strstr(got, "b = 2\n") != NULL);
    free(got);

    /* A corrupt file's recovery: defaults now, clean rewrite on the
     * next save — the resilience rule end-to-end. */
    write_file("corrupt.prefs", "[a\nk = 1\n"); /* unclosed section */
    use_file("corrupt.prefs");
    snprintf(path, sizeof path, "%s/corrupt.prefs", g_dir);
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_RECOVERED);
    assert(fdk_prefs_get_int(p, "a.k", -1) == -1);
    assert(fdk_ok(fdk_prefs_set_int(p, "a.k", 7)));
    assert(fdk_ok(fdk_prefs_save(p)));
    fdk_prefs_destroy(p);
    assert(fdk_ok(fdk_prefs_open("app", &p)));
    assert(fdk_prefs_source(p) == FDK_PREFS_SOURCE_FILE);
    assert(fdk_prefs_get_int(p, "a.k", -1) == 7);
    fdk_prefs_destroy(p);
    printf("[ok] prefs: CRLF normalized on rewrite + corrupt file "
           "recovered to a clean rewrite on next save\n");
}

/* ---- main ------------------------------------------------------------------ */

int main(void) {
    /* Sandbox scratch space + env originals. */
    snprintf(g_dir, sizeof g_dir, "/tmp/fdk-prefs-test.XXXXXX");
    assert(mkdtemp(g_dir) != NULL);
    g_home = getenv("HOME");
    g_xdg = getenv("XDG_CONFIG_HOME");
    g_override = getenv("FDK_PREFS_FILE");
    g_home = g_home != NULL ? strdup(g_home) : NULL;
    g_xdg = g_xdg != NULL ? strdup(g_xdg) : NULL;
    g_override = g_override != NULL ? strdup(g_override) : NULL;

    env_reset();
    test_argument_safety();
    test_key_grammar();
    test_path_resolution();
    test_parse_acceptance();
    test_parse_rejection();
    test_typed_access();
    test_setters_round_trip();
    test_save_golden();
    test_dirty_exactness();
    test_remove_iteration();
    test_key_limit();
    test_rewrite_recover();

    /* Restore the ambient environment for the battery's sake. */
    if (g_home != NULL) {
        setenv("HOME", g_home, 1);
    } else {
        unsetenv("HOME");
    }
    if (g_xdg != NULL) {
        setenv("XDG_CONFIG_HOME", g_xdg, 1);
    } else {
        unsetenv("XDG_CONFIG_HOME");
    }
    if (g_override != NULL) {
        setenv("FDK_PREFS_FILE", g_override, 1);
    } else {
        unsetenv("FDK_PREFS_FILE");
    }
    free(g_home);
    free(g_xdg);
    free(g_override);

    /* Recursive scratch cleanup: the 8192-key group leaves one
     * large file; the groups leave ~30 small ones. */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    (void)system(cmd);

    printf("all prefs tests passed\n");
    return 0;
}
