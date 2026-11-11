/* header_gate.c — the D5 BUILD gate (PRE-1.0-DECISIONS).
 *
 * A banned unstable-tier declaration in a public header must fail the
 * BUILD, not merely the test suite (a distro build that skips tests
 * would otherwise ship the violation silently). This tool IS the
 * gate: it scans the public headers with the same comment/string
 * stripping the old in-test scanner used (prose mentions are legal —
 * a DECLARATION is not), and exits nonzero on any hit.
 *
 * Usage: header_gate <header> [<header> ...]
 * Exit:  0 = all clean; 1 = a banned declaration (named, per file);
 *        2 = usage/IO error.
 *
 * The banned list is the unstable-tier rule exactly as the test suite
 * stated it: no zwp_ or zwlr_ type-name prefixes, the session-lock or
 * activation protocol names, or raw libwayland shim types
 * (wl_registry and wl_seat — the public surface speaks void*; see
 * fdk_wl_raw_bind). The bindings live in
 * src/platform/wayland/generated/ and never in include/fdk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *banned[] = {
    "zwp_", "zwlr_", "ext_session_lock", "xdg_activation_v1",
    "wl_registry", "wl_seat", NULL,
};

#define BUF_CAP (256 * 1024)

/* The scanner: strip block/line comments, string and char literals
 * (prose is legal; a declaration is not), then strstr for each banned
 * token. Returns the first banned token found, or NULL. */
/* The include-structure pass (item: no core public header may reach
 * the ext doors): strip comments but KEEP string contents (the old
 * scanner blanks strings, which would erase the include path), then
 * flag any line whose #include pulls in fdk/ext/. The ext headers
 * are ADD-ON-side native doors (wayland.h / x11.h); a core header
 * including one would put native types into the core surface. */
static int scan_includes(const char *buf, size_t len) {
    char *code = malloc(len + 1);
    if (code == NULL) {
        return 0;
    }
    size_t o = 0;
    int in_comment = 0, in_line_comment = 0, in_str = 0, in_chr = 0;
    for (size_t k = 0; k < len; k++) {
        char c = buf[k], n2 = k + 1 < len ? buf[k + 1] : '\0';
        if (in_comment) {
            if (c == '*' && n2 == '/') {
                in_comment = 0;
                k++;
            }
            continue;
        }
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = 0;
                code[o++] = c;
            }
            continue;
        }
        if (in_str) {
            code[o++] = c;
            if (c == '\\') {
                k++;
                if (k < len) {
                    code[o++] = buf[k];
                }
            } else if (c == '"') {
                in_str = 0;
            }
            continue;
        }
        if (in_chr) {
            code[o++] = c;
            if (c == '\\') {
                k++;
                if (k < len) {
                    code[o++] = buf[k];
                }
            } else if (c == '\'') {
                in_chr = 0;
            }
            continue;
        }
        if (c == '/' && n2 == '*') {
            in_comment = 1;
            k++;
            continue;
        }
        if (c == '/' && n2 == '/') {
            in_line_comment = 1;
            k++;
            continue;
        }
        if (c == '"') {
            in_str = 1;
        } else if (c == '\'') {
            in_chr = 1;
        }
        code[o++] = c;
    }
    code[o] = '\0';
    int bad = 0;
    for (size_t k = 0; k < o; k++) {
        if (code[k] == '#' && strncmp(&code[k], "#include", 8) == 0) {
            const char *line_end = strchr(&code[k], '\n');
            size_t llen = (line_end ? (size_t)(line_end - &code[k]) : o - k);
            char line[512];
            if (llen >= sizeof line) {
                llen = sizeof line - 1;
            }
            memcpy(line, &code[k], llen);
            line[llen] = '\0';
            if (strstr(line, "fdk/ext/") != NULL) {
                printf("#include of an ext door in a core public "
                       "header: %.256s\n", line);
                bad++;
            }
        }
    }
    free(code);
    return bad;
}

static const char *scan(const char *buf, size_t len) {
    char *code = malloc(len + 1);
    if (code == NULL) {
        return NULL;
    }
    size_t o = 0;
    int in_comment = 0, in_str = 0, in_chr = 0;
    for (size_t k = 0; k < len; k++) {
        char c = buf[k], n2 = k + 1 < len ? buf[k + 1] : '\0';
        if (in_comment) {
            if (c == '*' && n2 == '/') {
                in_comment = 0;
                k++;
            }
            continue;
        }
        if (in_str) {
            if (c == '\\') {
                k++;
            } else if (c == '"') {
                in_str = 0;
            }
            code[o++] = ' ';
            continue;
        }
        if (in_chr) {
            if (c == '\\') {
                k++;
            } else if (c == '\'') {
                in_chr = 0;
            }
            code[o++] = ' ';
            continue;
        }
        if (c == '/' && n2 == '*') {
            in_comment = 1;
            k++;
            code[o++] = ' ';
            continue;
        }
        if (c == '/' && n2 == '/') {
            break; /* the rest of the line is a comment */
        }
        if (c == '"') {
            in_str = 1;
        } else if (c == '\'') {
            in_chr = 1;
        }
        code[o++] = c;
    }
    code[o] = '\0';
    const char *hit = NULL;
    for (int b = 0; banned[b] != NULL && hit == NULL; b++) {
        if (strstr(code, banned[b]) != NULL) {
            hit = banned[b];
        }
    }
    free(code);
    return hit;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <header> [<header> ...]\n", argv[0]);
        return 2;
    }
    char *buf = malloc(BUF_CAP);
    if (buf == NULL) {
        return 2;
    }
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (f == NULL) {
            fprintf(stderr, "header_gate: cannot open %s\n", argv[i]);
            return 2;
        }
        size_t len = fread(buf, 1, BUF_CAP - 1, f);
        buf[len] = '\0';
        fclose(f);
        bad += scan_includes(buf, len);
        const char *hit = scan(buf, len);
        if (hit != NULL) {
            /* The #error-shaped message: the gate IS a build failure. */
            fprintf(stderr,
                    "#error %s: banned unstable-tier declaration '%s' "
                    "in a public header (bindings live in "
                    "src/platform/wayland/generated; the public surface "
                    "speaks void*)\n",
                    argv[i], hit);
            bad++;
        }
    }
    free(buf);
    if (bad > 0) {
        fprintf(stderr, "header_gate: %d violating file(s)\n", bad);
        return 1;
    }
    return 0;
}
