/* highlight.c — syntax highlighting through the range-attribute
 * engine: one pass per language filling (byte range, color) pairs.
 * Theme-aware: the colors come from the FDK palette so light and
 * dark both work without per-theme tables. */
#include "edit.h"

#include <ctype.h>
#include <string.h>

/* palette picks (theme-aware through the public theme API) */
static fdk_color col_keyword(void) {
    return fdk_theme_get_color(NULL, FDK_TK_ACCENT);
}
static fdk_color col_string(void) {
    /* a warm green-ish: derived from success with a nudge — use the
     * theme's success color for strings (readable both schemes)   */
    return fdk_theme_get_color(NULL, FDK_TK_SUCCESS);
}
static fdk_color col_comment(void) {
    return fdk_theme_get_color(NULL, FDK_TK_TEXT_DISABLED);
}
static fdk_color col_number(void) {
    return fdk_theme_get_color(NULL, FDK_TK_WARNING);
}

static void add(fdk_attr_list *a, size_t s, size_t e,
                fdk_attr_kind k, fdk_color c) {
    if (s >= e) {
        return;
    }
    (void)fdk_attr_list_add_range(a, s, e, k, 0, 0, c, NULL);
}

const char *ed_lang_name(ed_lang l) {
    switch (l) {
        case ED_LANG_C: return "C";
        case ED_LANG_SHELL: return "Shell";
        case ED_LANG_MARKDOWN: return "Markdown";
        case ED_LANG_JSON: return "JSON";
        case ED_LANG_MAKEFILE: return "Makefile";
        default: return "Plain";
    }
}

ed_lang ed_lang_of_path(const char *path) {
    if (path == NULL) {
        return ED_LANG_NONE;
    }
    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    const char *ext = (dot != NULL) ? dot + 1 : "";
    if (strcmp(base, "Makefile") == 0 ||
        strncmp(base, "Makefile", 8) == 0 ||
        strcmp(base, "GNUmakefile") == 0 ||
        strcmp(ext, "mk") == 0) {
        return ED_LANG_MAKEFILE;
    }
    if (strcmp(ext, "c") == 0 || strcmp(ext, "h") == 0 ||
        strcmp(ext, "cpp") == 0 || strcmp(ext, "hpp") == 0) {
        return ED_LANG_C;
    }
    if (strcmp(ext, "sh") == 0 || strcmp(ext, "bash") == 0 ||
        strcmp(ext, "zsh") == 0) {
        return ED_LANG_SHELL;
    }
    if (strcmp(ext, "md") == 0 || strcmp(ext, "markdown") == 0) {
        return ED_LANG_MARKDOWN;
    }
    if (strcmp(ext, "json") == 0) {
        return ED_LANG_JSON;
    }
    return ED_LANG_NONE;
}

/* ---- the shared scanner helpers ----------------------------------- */

static bool is_word_byte(char c) {
    return (unsigned char)c > 0x7F || isalnum((unsigned char)c) ||
           c == '_';
}

static size_t word_end(const char *t, size_t n, size_t i) {
    while (i < n && is_word_byte(t[i])) {
        i++;
    }
    return i;
}

/* C-family keywords + literals + comments + strings. */
static void hl_c(ed_doc *d, fdk_attr_list *a) {
    static const char *kw[] = {
        "auto", "break", "case", "char", "const", "continue",
        "default", "do", "double", "else", "enum", "extern", "float",
        "for", "goto", "if", "inline", "int", "long", "register",
        "restrict", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while", "bool", "true", "false", "NULL",
    };
    const char *t = d->text;
    size_t n = d->text_len;
    for (size_t i = 0; i < n;) {
        if (t[i] == '/' && i + 1 < n && t[i + 1] == '/') {
            size_t e = i;
            while (e < n && t[e] != '\n') {
                e++;
            }
            add(a, i, e, FDK_ATTR_BACKGROUND, col_comment());
            i = e;
        } else if (t[i] == '/' && i + 1 < n && t[i + 1] == '*') {
            size_t e = i + 2;
            while (e + 1 < n &&
                   !(t[e] == '*' && t[e + 1] == '/')) {
                e++;
            }
            e = (e + 1 < n) ? e + 2 : n;
            add(a, i, e, FDK_ATTR_BACKGROUND, col_comment());
            i = e;
        } else if (t[i] == '"') {
            size_t e = i + 1;
            while (e < n && t[e] != '"' && t[e] != '\n') {
                e += (t[e] == '\\' && e + 1 < n) ? 2 : 1;
            }
            e = (e < n) ? e + 1 : n;
            add(a, i, e, FDK_ATTR_BACKGROUND, col_string());
            i = e;
        } else if (t[i] == '\'') {
            size_t e = i + 1;
            while (e < n && t[e] != '\'' && t[e] != '\n') {
                e += (t[e] == '\\' && e + 1 < n) ? 2 : 1;
            }
            e = (e < n) ? e + 1 : n;
            add(a, i, e, FDK_ATTR_BACKGROUND, col_string());
            i = e;
        } else if (isdigit((unsigned char)t[i])) {
            size_t e = word_end(t, n, i);
            add(a, i, e, FDK_ATTR_BACKGROUND, col_number());
            i = e;
        } else if (is_word_byte(t[i])) {
            size_t s = i;
            size_t e = word_end(t, n, i);
            size_t len = e - s;
            for (size_t k = 0; k < sizeof kw / sizeof *kw; k++) {
                if (strlen(kw[k]) == len &&
                    strncmp(t + s, kw[k], len) == 0) {
                    add(a, s, e, FDK_ATTR_BACKGROUND, col_keyword());
                    break;
                }
            }
            i = e;
        } else {
            i++;
        }
    }
}

/* Shell: comments + strings + the keyword set + $vars. */
static void hl_shell(ed_doc *d, fdk_attr_list *a) {
    static const char *kw[] = {
        "if", "then", "else", "elif", "fi", "for", "while", "do",
        "done", "case", "esac", "function", "in", "return", "local",
        "export", "echo", "exit", "set", "unset", "shift",
    };
    const char *t = d->text;
    size_t n = d->text_len;
    for (size_t i = 0; i < n;) {
        if (t[i] == '#') {
            size_t e = i;
            while (e < n && t[e] != '\n') {
                e++;
            }
            add(a, i, e, FDK_ATTR_BACKGROUND, col_comment());
            i = e;
        } else if (t[i] == '"') {
            size_t e = i + 1;
            while (e < n && t[e] != '"' && t[e] != '\n') {
                e += (t[e] == '\\' && e + 1 < n) ? 2 : 1;
            }
            e = (e < n) ? e + 1 : n;
            add(a, i, e, FDK_ATTR_BACKGROUND, col_string());
            i = e;
        } else if (t[i] == '\'') {
            size_t e = i + 1;
            while (e < n && t[e] != '\'') {
                e++;
            }
            e = (e < n) ? e + 1 : n;
            add(a, i, e, FDK_ATTR_BACKGROUND, col_string());
            i = e;
        } else if (t[i] == '$' && i + 1 < n &&
                   is_word_byte(t[i + 1])) {
            size_t e = word_end(t, n, i + 1);
            add(a, i, e, FDK_ATTR_BACKGROUND, col_number());
            i = e;
        } else if (is_word_byte(t[i])) {
            size_t s = i;
            size_t e = word_end(t, n, i);
            size_t len = e - s;
            for (size_t k = 0; k < sizeof kw / sizeof *kw; k++) {
                if (strlen(kw[k]) == len &&
                    strncmp(t + s, kw[k], len) == 0) {
                    add(a, s, e, FDK_ATTR_BACKGROUND,
                        col_keyword());
                    break;
                }
            }
            i = e;
        } else {
            i++;
        }
    }
}

/* JSON: keys (strings before ':') vs values + numbers + literals. */
static void hl_json(ed_doc *d, fdk_attr_list *a) {
    const char *t = d->text;
    size_t n = d->text_len;
    for (size_t i = 0; i < n;) {
        if (t[i] == '"') {
            size_t e = i + 1;
            while (e < n && t[e] != '"' && t[e] != '\n') {
                e += (t[e] == '\\' && e + 1 < n) ? 2 : 1;
            }
            e = (e < n) ? e + 1 : n;
            /* a key: the next non-space after the string is ':' */
            size_t j = e;
            while (j < n && (t[j] == ' ' || t[j] == '\t')) {
                j++;
            }
            add(a, i, e, FDK_ATTR_BACKGROUND,
                (j < n && t[j] == ':') ? col_keyword()
                                       : col_string());
            i = e;
        } else if (isdigit((unsigned char)t[i]) ||
                   (t[i] == '-' && i + 1 < n &&
                    isdigit((unsigned char)t[i + 1]))) {
            size_t e = word_end(t, n, i + 1);
            add(a, i, e, FDK_ATTR_BACKGROUND, col_number());
            i = e;
        } else if (is_word_byte(t[i])) {
            size_t s = i;
            size_t e = word_end(t, n, i);
            if (e - s == 4 && strncmp(t + s, "true", 4) == 0) {
                add(a, s, e, FDK_ATTR_BACKGROUND, col_keyword());
            } else if (e - s == 5 &&
                       strncmp(t + s, "false", 5) == 0) {
                add(a, s, e, FDK_ATTR_BACKGROUND, col_keyword());
            } else if (e - s == 4 &&
                       strncmp(t + s, "null", 4) == 0) {
                add(a, s, e, FDK_ATTR_BACKGROUND, col_keyword());
            }
            i = e;
        } else {
            i++;
        }
    }
}

/* Markdown: headings, emphasis, code, links. */
static void hl_md(ed_doc *d, fdk_attr_list *a) {
    const char *t = d->text;
    size_t n = d->text_len;
    size_t line = 0;
    for (size_t i = 0; i < n;) {
        /* line start */
        size_t ls = i;
        size_t le = i;
        while (le < n && t[le] != '\n') {
            le++;
        }
        (void)line;
        if (t[ls] == '#') {
            size_t e = ls;
            while (e < le && t[e] == '#') {
                e++;
            }
            add(a, ls, le, FDK_ATTR_BACKGROUND, col_keyword());
        } else {
            /* inline: `code`, *em*, **strong**, [text](url) */
            for (size_t j = ls; j < le;) {
                if (t[j] == '`') {
                    size_t e = j + 1;
                    while (e < le && t[e] != '`') {
                        e++;
                    }
                    e = (e < le) ? e + 1 : le;
                    add(a, j, e, FDK_ATTR_BACKGROUND, col_string());
                    j = e;
                } else if (t[j] == '*' && j + 1 < le &&
                           t[j + 1] == '*') {
                    size_t e = j + 2;
                    while (e + 1 < le &&
                           !(t[e] == '*' && t[e + 1] == '*')) {
                        e++;
                    }
                    e = (e + 1 < le) ? e + 2 : le;
                    add(a, j, e, FDK_ATTR_BACKGROUND, col_keyword());
                    j = e;
                } else if (t[j] == '[') {
                    size_t e = j;
                    while (e < le && t[e] != ')') {
                        e++;
                    }
                    e = (e < le) ? e + 1 : le;
                    add(a, j, e, FDK_ATTR_BACKGROUND, col_number());
                    j = e;
                } else {
                    j++;
                }
            }
        }
        i = (le < n) ? le + 1 : n;
    }
}

/* Makefile: rules (target:) + $(vars) + comments + tabs-recipes. */
static void hl_mk(ed_doc *d, fdk_attr_list *a) {
    const char *t = d->text;
    size_t n = d->text_len;
    for (size_t i = 0; i < n;) {
        size_t ls = i;
        size_t le = i;
        while (le < n && t[le] != '\n') {
            le++;
        }
        if (t[ls] == '#') {
            add(a, ls, le, FDK_ATTR_BACKGROUND, col_comment());
        } else if (t[ls] != '\t' && ls < le) {
            /* a rule line: ends in ':' before any '=' */
            size_t c = ls;
            bool is_rule = false;
            while (c < le) {
                if (t[c] == ':') {
                    is_rule = true;
                    break;
                }
                if (t[c] == '=') {
                    break;
                }
                c++;
            }
            if (is_rule) {
                add(a, ls, c, FDK_ATTR_BACKGROUND, col_keyword());
            }
        }
        /* $(VAR) anywhere on the line */
        for (size_t j = ls; j + 1 < le; j++) {
            if (t[j] == '$' && t[j + 1] == '(') {
                size_t e = j + 2;
                while (e < le && t[e] != ')') {
                    e++;
                }
                e = (e < le) ? e + 1 : le;
                add(a, j, e, FDK_ATTR_BACKGROUND, col_number());
                j = e;
            }
        }
        i = (le < n) ? le + 1 : n;
    }
}

void ed_highlight(ed_doc *d) {
    if (d == NULL) {
        return;
    }
    fdk_attr_list_destroy(&eapp.attrs);
    eapp.attrs.items = NULL;
    eapp.attrs.count = 0;
    eapp.attrs.cap = 0;
    if (!eapp.highlight || d->lang == ED_LANG_NONE ||
        d->view == NULL) {
        (void)fdk_textview_set_attrs(d->view, NULL);
        return;
    }
    if (fdk_attr_list_init(&eapp.attrs) != FDK_OK) {
        return;
    }
    switch (d->lang) {
        case ED_LANG_C: hl_c(d, &eapp.attrs); break;
        case ED_LANG_SHELL: hl_shell(d, &eapp.attrs); break;
        case ED_LANG_JSON: hl_json(d, &eapp.attrs); break;
        case ED_LANG_MARKDOWN: hl_md(d, &eapp.attrs); break;
        case ED_LANG_MAKEFILE: hl_mk(d, &eapp.attrs); break;
        default: break;
    }
    (void)fdk_textview_set_attrs(d->view, &eapp.attrs);
}
