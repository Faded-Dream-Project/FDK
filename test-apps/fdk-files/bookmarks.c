/* bookmarks.c — see bookmarks.h. */
#include "bookmarks.h"
#include "../common/ta_buf.h"
#include "menus.h"
#include "teardown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static fi_bookmark g_marks[FI_BOOKMARKS_MAX];
static size_t g_mark_count;
static fdk_widget *g_list;

/* ---- persistence ----------------------------------------------------- */

static void bookmarks_path(char *out, size_t cap) {
    const char *xc = getenv("XDG_CONFIG_HOME");
    if (xc != NULL && xc[0] == '/') {
        ta_fmt(out, cap, "%.2000s/fdk-files/bookmarks", xc);
    } else {
        const char *home = getenv("HOME");
        if (home == NULL) {
            home = ".";
        }
        ta_fmt(out, cap, "%.2000s/.config/fdk-files/bookmarks", home);
    }
}

void bookmarks_load(void) {
    char path[FI_PATH_MAX];
    bookmarks_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return; /* the empty list */
    }
    char line[FI_PATH_MAX];
    while (g_mark_count < FI_BOOKMARKS_MAX &&
           fgets(line, sizeof line, f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        ta_copy(g_marks[g_mark_count].path,
                sizeof g_marks[g_mark_count].path, line);
        /* the label: the basename */
        const char *slash = strrchr(line, '/');
        const char *base = (slash != NULL) ? slash + 1 : line;
        if (base[0] == '\0') {
            base = "/";
        }
        ta_copy(g_marks[g_mark_count].label,
                sizeof g_marks[g_mark_count].label, base);
        g_mark_count++;
    }
    fclose(f);
}

void bookmarks_save(void) {
    char path[FI_PATH_MAX];
    bookmarks_path(path, sizeof path);
    /* the config dir on demand */
    char dir[FI_PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (slash != NULL) {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof dir) {
            n = sizeof dir - 1;
        }
        memcpy(dir, path, n);
        dir[n] = '\0';
        (void)system(NULL); /* no mkdir -p dependency: use the C way */
        char step[FI_PATH_MAX];
        ta_copy(step, sizeof step, dir);
        for (char *s = step + 1; *s != '\0'; s++) {
            if (*s == '/') {
                *s = '\0';
                (void)mkdir(step, 0700);
                *s = '/';
            }
        }
        (void)mkdir(step, 0700);
    }
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return; /* read-only HOME: persistence lost, not a crash */
    }
    fprintf(f, "# fdk-files bookmarks (order = display order)\n");
    for (size_t i = 0; i < g_mark_count; i++) {
        fprintf(f, "%s\n", g_marks[i].path);
    }
    fclose(f);
}

/* ---- the in-memory list ------------------------------------------------ */

size_t bookmarks_count(void) {
    return g_mark_count;
}

const fi_bookmark *bookmarks_at(size_t i) {
    return (i < g_mark_count) ? &g_marks[i] : NULL;
}

int bookmarks_add(const char *path) {
    if (path == NULL || path[0] != '/') {
        return -1; /* absolute folders only (honest limit) */
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return -1;
    }
    for (size_t i = 0; i < g_mark_count; i++) {
        if (strcmp(g_marks[i].path, path) == 0) {
            return (int)i; /* duplicate: idempotent */
        }
    }
    if (g_mark_count == FI_BOOKMARKS_MAX) {
        return -1;
    }
    fi_bookmark *b = &g_marks[g_mark_count];
    ta_copy(b->path, sizeof b->path, path);
    const char *slash = strrchr(path, '/');
    const char *base = (slash != NULL) ? slash + 1 : path;
    if (base[0] == '\0') {
        base = "/";
    }
    ta_copy(b->label, sizeof b->label, base);
    g_mark_count++;
    return (int)(g_mark_count - 1);
}

bool bookmarks_remove(const char *path) {
    for (size_t i = 0; i < g_mark_count; i++) {
        if (strcmp(g_marks[i].path, path) == 0) {
            memmove(&g_marks[i], &g_marks[i + 1],
                    (g_mark_count - i - 1) * sizeof g_marks[i]);
            g_mark_count--;
            return true;
        }
    }
    return false;
}

bool bookmarks_move(size_t from, size_t to) {
    if (from >= g_mark_count || to >= g_mark_count || from == to) {
        return false;
    }
    fi_bookmark b = g_marks[from];
    if (from < to) {
        memmove(&g_marks[from], &g_marks[from + 1],
                (to - from) * sizeof g_marks[0]);
    } else {
        memmove(&g_marks[to + 1], &g_marks[to],
                (from - to) * sizeof g_marks[0]);
    }
    g_marks[to] = b;
    return true;
}

/* ---- the sidebar section ------------------------------------------------ */

static void bookmark_activated(fdk_widget *list, size_t row,
                               void *user) {
    (void)list;
    (void)user;
    if (row < g_mark_count) {
        pane_navigate(active_pane(), g_marks[row].path);
    }
}

/* the row index at a WINDOW-LOCAL y (public-API math: the widget's
 * bounds + the list's row height) */
static size_t bookmark_row_at(fdk_pointf pos) {
    if (g_list == NULL) {
        return 0;
    }
    fdk_rect abs = fdk_widget_get_absolute_bounds(g_list);
    fdk_i32 rh = fdk_list_get_row_height(g_list);
    if (rh <= 0) {
        return 0;
    }
    fdk_i32 y = (fdk_i32)pos.y - abs.y;
    if (y < 0) {
        return 0;
    }
    size_t row = (size_t)(y / rh);
    if (row > g_mark_count) {
        row = g_mark_count; /* below the last row: append position */
    }
    return row;
}

static void bookmark_drag(fdk_widget *w, fdk_event_type type,
                          const fdk_drag_event *ev, void *user) {
    (void)w;
    (void)user;
    if (type != FDK_EVENT_DRAG_DROP) {
        return; /* ENTER/MOTION/LEAVE: no visual feedback (§24 shape) */
    }
    if (ev->uris == NULL || ev->uri_count == 0) {
        return;
    }
    /* decode the first URI to a path (the file-uri form) */
    char path[FI_PATH_MAX];
    const char *u = ev->uris[0];
    if (strncmp(u, "file://", 7) == 0) {
        u += 7;
    }
    ta_copy(path, sizeof path, u);

    size_t row = bookmark_row_at(ev->position);

    /* a URI that IS one of ours = REORDER (move to row) */
    for (size_t i = 0; i < g_mark_count; i++) {
        if (strcmp(g_marks[i].path, path) == 0) {
            size_t to = (row > g_mark_count) ? g_mark_count - 1 : row;
            if (to > 0 && i < to) {
                to--; /* dropping after ourselves = stay */
            }
            if (bookmarks_move(i, to)) {
                bookmarks_refresh();
                bookmarks_save();
                fi_set_status("Bookmark moved");
            }
            return;
        }
    }
    /* else: an add at the drop position */
    int added = bookmarks_add(path);
    if (added >= 0) {
        size_t from = (size_t)added;
        size_t to = (row > g_mark_count) ? g_mark_count - 1 : row;
        if (to > g_mark_count - 1) {
            to = g_mark_count - 1;
        }
        if (from != to) {
            (void)bookmarks_move(from, to);
        }
        bookmarks_refresh();
        bookmarks_save();
        fi_set_status("Bookmark added: %s", path);
    } else {
        fi_set_status("Not a folder — bookmarks are folders only");
    }
}

/* the row context menu: remove */
static void bookmark_menu_remove(fdk_menu_item *item, void *user) {
    (void)item;
    size_t row = (size_t)(intptr_t)user;
    if (row < g_mark_count) {
        fi_set_status("Bookmark removed: %s", g_marks[row].label);
        (void)bookmarks_remove(g_marks[row].path);
        bookmarks_refresh();
        bookmarks_save();
    }
}

static size_t g_bookmark_menu_row;

static void bookmark_menu_show(void) {
    fdk_menu *m = NULL;
    if (!fdk_ok(fdk_menu_create(fapp.base.font_ui, &m))) {
        return;
    }
    fdk_menu_item *it = NULL;
    (void)fdk_menu_append(m, "Remove bookmark", &it);
    fdk_menu_item_set_on_activate(
        it, bookmark_menu_remove,
        (void *)(intptr_t)g_bookmark_menu_row);
    menus_ctx_popup(m, g_list, -1, -1);
}

static void bookmark_drag_out(size_t row);

static bool bookmark_event(fdk_widget *w, const fdk_widget_event *ev,
                           void *user) {
    (void)w;
    (void)user;
    if (ev->type == FDK_WIDGET_POINTER_DOWN &&
        ev->pointer.button == 3) {
        g_bookmark_menu_row = bookmark_row_at(ev->pointer.position);
        if (g_bookmark_menu_row < g_mark_count) {
            bookmark_menu_show();
            return true;
        }
    }
    /* §29/G-10: the drag-out moved to FDK's widget-level drag
     * source (registered below) — the old immediate drag on press
     * was wrong-shaped (a real drag is press-then-move) and starved
     * by the row's own press consumption anyway. */
    return false;
}

/* the drag-source begin fn: the press (which selected the row)
 * moved past the slop — drag the bookmarked folder itself. */
static void *bookmark_drag_source_begin(fdk_widget *source,
                                        void *user) {
    (void)source;
    (void)user;
    if (g_list == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < g_mark_count; i++) {
        if (fdk_list_is_selected(g_list, i)) {
            bookmark_drag_out(i);
            return NULL;
        }
    }
    return NULL;
}

/* the drag SOURCE: a left press on a row starts a URI drag of the
 * bookmarked folder (the file-manager gesture). */
static void bookmark_drag_out(size_t row) {
    if (row >= g_mark_count) {
        return;
    }
    /* the URI drag of the folder itself */
    const char *uris[1] = {g_marks[row].path};
    fdk_surface *icon = icons_get("folder", 24, 1.0f);
    fdk_u32 pixels[24 * 24];
    if (icon != NULL) {
        fdk_surface_info info;
        if (fdk_ok(fdk_surface_get_info(icon, &info)) &&
            info.pixels != NULL) {
            for (int y = 0; y < 24 && y < info.height; y++) {
                for (int x = 0; x < 24 && x < info.width; x++) {
                    pixels[y * 24 + x] =
                        info.pixels[(size_t)y * (size_t)info.stride +
                                    (size_t)x];
                }
            }
        } else {
            icon = NULL;
        }
    }
    (void)fdk_drag_begin_ex(
        fapp.base.window, FDK_DRAG_FORMAT_URI_LIST, NULL, uris, 1,
        (icon != NULL) ? pixels : NULL,
        (icon != NULL) ? 24 : 0, (icon != NULL) ? 24 : 0, NULL, NULL);
}

void bookmarks_refresh(void) {
    if (g_list == NULL) {
        return;
    }
    fdk_list_begin_batch(g_list);
    fdk_list_clear(g_list);
    for (size_t i = 0; i < g_mark_count; i++) {
        size_t idx = 0;
        (void)fdk_list_append(g_list, g_marks[i].label, &idx);
        (void)fdk_list_row_set_icon(g_list, idx,
                                    FDK_ROW_ICON_FOLDER);
    }
    fdk_list_end_batch(g_list);
}

void bookmarks_build(fdk_widget *parent) {
    fdk_widget *label = NULL;
    (void)fdk_label_create(parent, fapp.base.font_ui, "Bookmarks",
                           &label);
    (void)fdk_list_create(parent, fapp.base.font_ui, &g_list);
    fdk_list_set_selection_mode(g_list, FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_activate_on_single_click(g_list, true);
    fdk_widget_set_expand(g_list, false, true);
    fdk_widget_set_accessible_name(g_list, "Bookmarks");
    fdk_list_set_on_row_activate(g_list, bookmark_activated, NULL);

    /* DnD: target (add/reorder) + source (drag a bookmark out) */
    (void)fdk_widget_set_drop_target(g_list,
                                     FDK_DRAG_FORMAT_URI_LIST);
    fdk_widget_set_drag_callback(g_list, bookmark_drag, NULL);
    fdk_widget_set_event_callback(g_list, bookmark_event, NULL);
    /* §29/G-10: press-then-move drag-out of a bookmark row */
    (void)fdk_widget_set_drag_source(g_list, 6,
                                     bookmark_drag_source_begin, NULL);

    bookmarks_refresh();
}

fdk_widget *bookmarks_widget(void) {
    return g_list;
}
