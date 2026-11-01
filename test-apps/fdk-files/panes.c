/* panes.c — see panes.h. The biggest UI module: the toolbar, the
 * places sidebar, the pane/tab structure, the breadcrumb bar, the
 * three view modes, the clipboard/DnD wiring, and the search. */
#include "panes.h"
#include "fsops.h"
#include "mime.h"
#include "menus.h"
#include "ops_dialogs.h"
#include "teardown.h"
#include "trash.h"

#include "fdk/fdk_dialog.h"
#include "fdk/fdk_layout.h"
#include "fdk/fdk_theme.h"

#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void pane_build_view(pane *p, tab *t);

/* forward decls (the build order below crosses them) */
static void mount_activated(fdk_widget *list, size_t row, void *user);
static void trash_open_clicked(fdk_widget *btn, void *user);
static const char *strcasestr_f(const char *hay, const char *needle);
static int tolower_f(int c);

/* ---- small helpers ------------------------------------------------- */

const char *fi_home(void) {
    const char *h = getenv("HOME");
    return (h != NULL && h[0] != '\0') ? h : "/";
}

void fi_fmt_size(off_t bytes, char *buf, size_t cap) {
    double v = (double)bytes;
    if (bytes < 1024) {
        snprintf(buf, cap, "%d B", (int)bytes);
    } else if (bytes < 1024LL * 1024) {
        snprintf(buf, cap, "%.1f KB", v / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        snprintf(buf, cap, "%.1f MB", v / (1024.0 * 1024.0));
    } else {
        snprintf(buf, cap, "%.1f GB",
                 v / (1024.0 * 1024.0 * 1024.0));
    }
}

void fi_fmt_mtime(time_t t, char *buf, size_t cap) {
    struct tm tm_buf;
    if (localtime_r(&t, &tm_buf) == NULL) {
        snprintf(buf, cap, "?");
        return;
    }
    strftime(buf, cap, "%Y-%m-%d %H:%M", &tm_buf);
}

void fi_set_status(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ta_status(&fapp.base, "%s", buf);
}

struct pane *active_pane(void) {
    return fapp.panes[fapp.active_pane];
}

tab *active_tab(pane *p) {
    return p->tabs[p->current];
}

/* ---- the breadcrumb ----------------------------------------------- */

typedef struct crumb {
    pane *pane;
    char path[FI_PATH_MAX];
} crumb;

/* The pane's crumb payloads (rebuilt on every navigation; the
 * widgets die with the bar's children, the payloads die HERE). */
static void pane_crumbs_free(pane *p) {
    for (size_t i = 0; i < p->crumb_count; i++) {
        free(p->crumbs[i]);
    }
    free(p->crumbs);
    p->crumbs = NULL;
    p->crumb_count = 0;
    p->crumb_cap = 0;
}

static void pane_crumb_add(pane *p, crumb *c) {
    if (p->crumb_count == p->crumb_cap) {
        p->crumb_cap = p->crumb_cap ? p->crumb_cap * 2 : 8;
        crumb **g = realloc(p->crumbs,
                            p->crumb_cap * sizeof *p->crumbs);
        if (g == NULL) {
            free(c);
            return;
        }
        p->crumbs = g;
    }
    p->crumbs[p->crumb_count++] = c;
}

static void crumb_clicked(fdk_widget *btn, void *user) {
    (void)btn;
    crumb *c = user;
    pane_navigate(c->pane, c->path);
}

static void path_activate(fdk_widget *entry, void *user) {
    pane *p = user;
    const char *text = fdk_entry_get_text(entry);
    char abs[FI_PATH_MAX];
    if (text[0] == '~' && (text[1] == '/' || text[1] == '\0')) {
        snprintf(abs, sizeof abs, "%.4000s%.60s", fi_home(),
                 text + 1);
    } else if (text[0] != '/') {
        snprintf(abs, sizeof abs, "%.4000s/%.60s",
                 active_tab(p)->dir, text);
    } else {
        snprintf(abs, sizeof abs, "%s", text);
    }
    /* resolve . and .. lexically */
    char clean[FI_PATH_MAX];
    char *out = clean;
    const char *s = abs;
    *out++ = '/';
    while (*s != '\0') {
        while (*s == '/') {
            s++;
        }
        const char *seg = s;
        while (*s != '/' && *s != '\0') {
            s++;
        }
        size_t n = (size_t)(s - seg);
        if (n == 1 && seg[0] == '.') {
            /* skip */
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (out > clean + 1 && *(out - 1) != '/') {
                out--;
            }
            if (out > clean + 1) {
                out--;
            }
        } else if (n > 0) {
            if (out != clean + 1) {
                *out++ = '/';
            }
            memcpy(out, seg, n);
            out += n;
        }
    }
    *out = '\0';

    struct stat st;
    if (stat(clean, &st) == 0 && S_ISDIR(st.st_mode)) {
        p->path_editing = false;
        pane_navigate(p, clean);
    } else {
        fi_set_status("Not a folder: %s", clean);
    }
}

static void path_entry_setup(pane *p, fdk_widget *entry) {
    fdk_widget_set_natural_size(entry, 0, 30);
    fdk_widget_set_expand(entry, true, false);
    (void)fdk_entry_set_icon(entry, FDK_ENTRY_SLOT_LEADING,
                             FDK_ENTRY_ICON_FOLDER);
    fdk_widget_set_accessible_name(entry, "Location");
    fdk_entry_set_on_activate(entry, path_activate, p);
}

void pane_refresh_breadcrumb(pane *p) {
    tab *t = active_tab(p);
    pane_crumbs_free(p); /* the old bar's payloads die with it */
    fdk_widget *host = t->breadcrumb_host;
    size_t children = fdk_widget_child_count(host);
    for (size_t i = children; i > 0; i--) {
        fdk_widget_destroy(fdk_widget_child_at(host, i - 1));
    }
    p->path_entry = NULL;
    if (p->path_editing) {
        fdk_widget *entry = NULL;
        (void)fdk_entry_create(host, fapp.base.font_ui, "", &entry);
        path_entry_setup(p, entry);
        (void)fdk_entry_set_text(entry, t->dir);
        fdk_entry_select_all(entry);
        p->path_entry = entry;
        return;
    }

    const char *dir = t->dir;
    char acc[FI_JOIN_MAX * 2] = "";
    if (dir[0] == '/') {
        crumb *c = calloc(1, sizeof *c);
        if (c != NULL) {
            c->pane = p;
            snprintf(c->path, sizeof c->path, "/");
            pane_crumb_add(p, c);
            fdk_widget *btn = NULL;
            if (fdk_ok(fdk_button_create(host, fapp.base.font_ui,
                                         "/", &btn))) {
                fdk_button_set_role(btn, FDK_BUTTON_ROLE_LINK);
                fdk_button_set_on_activate(btn, crumb_clicked, c);
                fdk_widget_set_accessible_name(btn,
                                               "Filesystem root");
            }
        }
    }
    size_t off = 1;
    for (;;) {
        size_t n = 0;
        while (dir[off + n] != '/' && dir[off + n] != '\0') {
            n++;
        }
        if (n > 0) {
            if (acc[0] == '\0') {
                snprintf(acc, sizeof acc, "/");
            } else {
                strncat(acc, "/", sizeof acc - strlen(acc) - 1);
            }
            strncat(acc, dir + off, n);
            char label[FI_NAME_MAX];
            snprintf(label, sizeof label, "%.*s",
                     (int)(n > 200 ? 200 : n), dir + off);
            crumb *c = calloc(1, sizeof *c);
            if (c != NULL) {
                c->pane = p;
                snprintf(c->path, sizeof c->path, "%.4000s", acc);
                pane_crumb_add(p, c);
                fdk_widget *btn = NULL;
                if (fdk_ok(fdk_button_create(
                        host, fapp.base.font_ui, label, &btn))) {
                    fdk_button_set_role(btn, FDK_BUTTON_ROLE_LINK);
                    fdk_button_set_on_activate(btn, crumb_clicked,
                                               c);
                    char a11y[FI_PATH_MAX + 16];
                    snprintf(a11y, sizeof a11y, "Go to %s",
                             c->path);
                    fdk_widget_set_accessible_name(btn, a11y);
                }
            }
        }
        if (dir[off + n] == '\0') {
            break;
        }
        off += n + 1;
    }
}

/* ---- the listing views --------------------------------------------- */

typedef struct view_ctx {
    pane *pane;
    tab *tab;
    fdk_widget *view; /* the list or iconview                  */
    bool is_list;
} view_ctx;

static void row_activated(fdk_widget *view, size_t row, void *user) {
    view_ctx *vc = user;
    const file_info *fi = NULL;
    if (vc->is_list) {
        fdk_u64 id = fdk_list_bound_row_id(view, row);
        fi = fmodel_info_of(id);
    } else {
        if (row < fmodel.info_count) {
            fi = fmodel.infos[row];
        }
    }
    if (fi == NULL) {
        return;
    }
    if (fi->is_dir) {
        pane_navigate(vc->pane, fi->path);
    } else {
        ops_open_with(vc->pane, fi);
    }
}

static bool view_event(fdk_widget *w, const fdk_widget_event *ev,
                       void *user) {
    (void)w;
    view_ctx *vc = user;
    if (ev->type == FDK_WIDGET_POINTER_DOWN &&
        ev->pointer.button == 3) {
        size_t sel = fdk_list_selected_count(vc->view);
        int flavor = (sel > 1) ? CTX_MULTI
                     : (sel == 1) ? CTX_FILE
                                  : CTX_EMPTY;
        menus_popup_context(vc->pane, flavor,
                            (fdk_i32)ev->pointer.position.x,
                            (fdk_i32)ev->pointer.position.y);
        return true;
    }
    /* the drag source: a left press on a selected row arms it; a
     * motion past the threshold starts the URI drag. FDK's DnD
     * begins from a pointer-press handler on the WINDOW, so we
     * record the press and let the drag start on motion. */
    if (ev->type == FDK_WIDGET_POINTER_DOWN &&
        ev->pointer.button == 1 && vc->is_list &&
        fdk_list_selected_count(vc->view) > 0) {
        vc->tab->drag_armed = true;
        vc->tab->drag_x = (fdk_i32)ev->pointer.position.x;
        vc->tab->drag_y = (fdk_i32)ev->pointer.position.y;
        return false; /* let the list keep the selection click */
    }
    if (ev->type == FDK_WIDGET_POINTER_MOTION && vc->tab->drag_armed) {
        fdk_i32 dx = (fdk_i32)ev->pointer.position.x - vc->tab->drag_x;
        fdk_i32 dy = (fdk_i32)ev->pointer.position.y - vc->tab->drag_y;
        if (dx * dx + dy * dy > 36) {
            vc->tab->drag_armed = false;
            pane_begin_drag(vc->pane);
            return true;
        }
    }
    if (ev->type == FDK_WIDGET_POINTER_UP) {
        vc->tab->drag_armed = false;
    }
    return false;
}

void pane_build_view(pane *p, tab *t) {
    fdk_widget *host = t->view_host;
    size_t children = fdk_widget_child_count(host);
    for (size_t i = children; i > 0; i--) {
        fdk_widget_destroy(fdk_widget_child_at(host, i - 1));
    }
    view_ctx *vc = calloc(1, sizeof *vc);
    if (vc == NULL) {
        return;
    }
    vc->pane = p;
    vc->tab = t;
    td_track(vc, TD_RAW); /* freed at teardown (td_sweep)         */

    if (t->view_mode == VIEW_ICONS) {
        fdk_widget *iv = NULL;
        (void)fdk_iconview_create(host, fapp.base.font_ui, &iv);
        fdk_widget_set_expand(iv, true, true);
        for (size_t i = 0; i < fmodel.info_count; i++) {
            const file_info *fi = fmodel.infos[i];
            if (fi == NULL) {
                continue;
            }
            fdk_row_icon glyph = fi->is_dir ? FDK_ROW_ICON_FOLDER
                                            : FDK_ROW_ICON_FILE;
            (void)fdk_iconview_append(iv, fi->name, glyph);
        }
        fdk_widget_set_accessible_name(iv, "Files");
        vc->view = iv;
        vc->is_list = false;
        t->list = NULL;
    } else {
        fdk_widget *list = NULL;
        (void)fdk_list_create(host, fapp.base.font_ui, &list);
        fdk_list_set_selection_mode(list,
                                    FDK_LIST_SELECTION_MULTIPLE);
        fdk_widget_set_expand(list, true, true);
        (void)fdk_list_bind_model(list, fmodel.view, FM_COL_NAME);
        fdk_list_set_on_row_activate(list, row_activated, vc);
        fdk_widget_set_accessible_name(list, "Files");
        vc->view = list;
        vc->is_list = true;
        t->list = list;
    }
    fdk_widget_set_event_callback(vc->view, view_event, vc);
    pane_report_status(p);
}

/* ---- navigation ---------------------------------------------------- */

void pane_reload(pane *p) {
    pane_navigate(p, active_tab(p)->dir);
}

static void pane_watch(pane *p, const char *dir) {
    if (p->watch_fd >= 0) {
        fs_watch_stop(p->watch_fd);
        p->watch_fd = -1;
    }
    p->watch_fd = fs_watch_start(dir);
}

void pane_navigate(pane *p, const char *abs_path) {
    tab *t = active_tab(p);
    if (t == NULL || abs_path == NULL) {
        return;
    }

    /* history (skip pushing a duplicate of the top) */
    if (p->back_count == 0 ||
        strcmp(p->back[p->back_count - 1], t->dir) != 0) {
        if (p->back_count == 64) {
            memmove(p->back[0], p->back[1],
                    sizeof p->back[0] * 63);
            p->back_count--;
        }
        char *slot = p->back[p->back_count];
        if (slot != NULL && p->back_count < 64) {
            snprintf(slot, FI_PATH_MAX, "%.4000s", t->dir);
            p->back_count++;
        }
    }
    p->fwd_count = 0;

    snprintf(t->dir, sizeof t->dir, "%.400s", abs_path);
    const char *slash = strrchr(t->dir, '/');
    const char *base = (slash != NULL) ? slash + 1 : t->dir;
    if (base[0] == '\0') {
        base = "/";
    }
    snprintf(t->title, sizeof t->title, "%.60s", base);

    long n = fmodel_load(t->dir, fapp.show_hidden);
    if (n < 0) {
        fi_set_status("Cannot open %s", t->dir);
        n = 0;
    }

    (void)fdk_notebook_set_page_label(p->notebook, p->current, t->title);
    pane_refresh_breadcrumb(p);
    pane_build_view(p, t);
    pane_watch(p, t->dir);
    pane_report_status(p);
}

void pane_back(pane *p) {
    if (p->back_count == 0) {
        return;
    }
    tab *t = active_tab(p);
    if (p->fwd_count < 64) {
        snprintf(p->fwd[p->fwd_count++], FI_PATH_MAX,
                 "%.4000s", t->dir);
    }
    char target[FI_PATH_MAX];
    snprintf(target, sizeof target, "%.4000s", p->back[--p->back_count]);
    snprintf(t->dir, sizeof t->dir, "%.4000s", target);
    long n = fmodel_load(t->dir, fapp.show_hidden);
    (void)n;
    (void)fdk_notebook_set_page_label(p->notebook, p->current, t->title);
    pane_refresh_breadcrumb(p);
    pane_build_view(p, t);
    pane_watch(p, t->dir);
}

void pane_forward(pane *p) {
    if (p->fwd_count == 0) {
        return;
    }
    tab *t = active_tab(p);
    if (p->back_count < 64) {
        snprintf(p->back[p->back_count++], FI_PATH_MAX,
                 "%.4000s", t->dir);
    }
    char target[FI_PATH_MAX];
    snprintf(target, sizeof target, "%.4000s", p->fwd[--p->fwd_count]);
    snprintf(t->dir, sizeof t->dir, "%.4000s", target);
    long n = fmodel_load(t->dir, fapp.show_hidden);
    (void)n;
    (void)fdk_notebook_set_page_label(p->notebook, p->current, t->title);
    pane_refresh_breadcrumb(p);
    pane_build_view(p, t);
    pane_watch(p, t->dir);
}

void pane_up(pane *p) {
    tab *t = active_tab(p);
    if (strcmp(t->dir, "/") == 0) {
        return;
    }
    char parent[FI_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", t->dir);
    char *slash = strrchr(parent, '/');
    if (slash == parent) {
        snprintf(parent + 1, sizeof parent - 1, "%s", "");
    } else if (slash != NULL) {
        *slash = '\0';
    }
    pane_navigate(p, parent);
}

/* ---- tabs ----------------------------------------------------------- */

void pane_new_tab(pane *p, const char *start_dir) {
    if (p->tab_count >= 16) {
        return;
    }
    tab *t = calloc(1, sizeof *t);
    if (t == NULL) {
        return;
    }
    snprintf(t->dir, sizeof t->dir, "%s",
             start_dir != NULL ? start_dir : fi_home());
    t->view_mode = VIEW_DETAIL;

    fdk_widget *page = NULL;
    (void)fdk_box_create(p->notebook_host, FDK_VERTICAL, &page);
    /* the page: [breadcrumb host][view host] */
    (void)fdk_box_create(page, FDK_HORIZONTAL, &t->breadcrumb_host);
    fdk_box_set_padding(t->breadcrumb_host, 6);
    fdk_box_set_spacing(t->breadcrumb_host, 2);
    fdk_widget_set_natural_size(t->breadcrumb_host, 0, 34);

    (void)fdk_box_create(page, FDK_VERTICAL, &t->view_host);
    fdk_widget_set_expand(t->view_host, true, true);

    t->page = page;
    p->tabs[p->tab_count] = t;
    (void)fdk_notebook_append_page(p->notebook, page, "Tab");
    p->current = p->tab_count;
    p->tab_count++;
    pane_navigate(p, t->dir);
}

void pane_close_tab(pane *p, size_t index) {
    if (p->tab_count <= 1 || index >= p->tab_count) {
        return;
    }
    free(p->tabs[index]);
    for (size_t i = index; i + 1 < p->tab_count; i++) {
        p->tabs[i] = p->tabs[i + 1];
    }
    p->tab_count--;
    (void)fdk_notebook_remove_page(p->notebook, index);
    if (p->current >= p->tab_count) {
        p->current = p->tab_count - 1;
    }
    (void)fdk_notebook_set_current_page(p->notebook, p->current);
}

/* ---- the view modes / selection ------------------------------------ */

void pane_set_view_mode(pane *p, int mode) {
    tab *t = active_tab(p);
    t->view_mode = mode;
    pane_build_view(p, t);
}

void pane_select_all(pane *p) {
    tab *t = active_tab(p);
    if (t->list != NULL) {
        /* model-bound multi-select: select every VIEW row */
        size_t n = fdk_list_row_count(t->list);
        fdk_list_clear_selection(t->list);
        for (size_t i = 0; i < n; i++) {
            (void)fdk_list_select(t->list, i);
        }
        pane_report_status(p);
    }
}

void pane_invert_selection(pane *p) {
    tab *t = active_tab(p);
    if (t->list == NULL) {
        return;
    }
    size_t n = fdk_list_row_count(t->list);
    for (size_t i = 0; i < n; i++) {
        if (fdk_list_is_selected(t->list, i)) {
            /* deselecting single rows in MULTIPLE mode needs the
             * toggle semantics FDK models via ctrl+click; the API
             * surface offers select + clear_selection only, so we
             * rebuild the set (recorded as an API friction note) */
        }
    }
    /* honest implementation: rebuild by complement */
    bool *want = calloc(n ? n : 1, sizeof *want);
    if (want == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        want[i] = !fdk_list_is_selected(t->list, i);
    }
    fdk_list_clear_selection(t->list);
    for (size_t i = 0; i < n; i++) {
        if (want[i]) {
            (void)fdk_list_select(t->list, i);
        }
    }
    free(want);
    pane_report_status(p);
}

/* ---- drag source ---------------------------------------------------- */

static void drag_done(fdk_drag_status status, void *user) {
    pane *p = user;
    fi_set_status("Drag %s",
                  status == FDK_DRAG_SUCCEEDED ? "delivered"
                                               : "cancelled");
    (void)p;
}

void pane_begin_drag(pane *p) {
    tab *t = active_tab(p);
    if (t->list == NULL) {
        return;
    }
    const file_info *sel[512];
    size_t n = fmodel_selected(p, sel, 512);
    if (n == 0) {
        return;
    }
    static const char *uris[512];
    static char buffers[512][FI_PATH_MAX];
    for (size_t i = 0; i < n; i++) {
        snprintf(buffers[i], sizeof buffers[i], "%s", sel[i]->path);
        uris[i] = buffers[i];
    }
    /* a small folder icon as the drag feedback */
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
    fdk_result r = fdk_drag_begin_ex(
        fapp.base.window, FDK_DRAG_FORMAT_URI_LIST, NULL, uris, n,
        (icon != NULL) ? pixels : NULL,
        (icon != NULL) ? 24 : 0, (icon != NULL) ? 24 : 0, drag_done,
        p);
    if (!fdk_ok(r)) {
        fi_set_status("Drag failed to start (backend refused)");
    }
}

/* ---- the drop target (widget-level) -------------------------------- */

static void pane_drop(fdk_widget *w, fdk_event_type type,
                      const fdk_drag_event *ev, void *user) {
    pane *p = user;
    tab *t = active_tab(p);
    if (type == FDK_EVENT_DRAG_DROP && ev->uris != NULL &&
        ev->uri_count > 0) {
        /* decode each URI to a path */
        static char paths[64][FI_PATH_MAX];
        size_t n = 0;
        for (size_t i = 0; i < ev->uri_count && n < 64; i++) {
            const char *u = ev->uris[i];
            if (strncmp(u, "file://", 7) == 0) {
                snprintf(paths[n], sizeof paths[n], "%s", u + 7);
            } else if (u[0] == '/') {
                snprintf(paths[n], sizeof paths[n], "%s", u);
            } else {
                continue;
            }
            n++;
        }
        if (n == 0) {
            return;
        }
        if (strcmp(t->dir, "/") == 0) {
            /* dropping onto the same directory: skip */
        }
        bool same_dir = true;
        for (size_t i = 0; i < n; i++) {
            char *slash = strrchr(paths[i], '/');
            char parent[FI_PATH_MAX];
            if (slash != NULL) {
                size_t plen = (size_t)(slash - paths[i]);
                if (plen == 0) {
                    plen = 1;
                }
                snprintf(parent, sizeof parent, "%.*s", (int)plen,
                         paths[i]);
            } else {
                snprintf(parent, sizeof parent, "%s", t->dir);
            }
            if (strcmp(parent, t->dir) != 0) {
                same_dir = false;
            }
        }
        if (same_dir) {
            fi_set_status("Dropped onto the source folder — skipped");
            return;
        }
        ops_begin_paste_into(p, (const char *const *)paths, n,
                             t->dir, (ev->modifiers & FDK_MOD_SHIFT)
                                         ? FI_OP_MOVE
                                         : FI_OP_COPY);
        (void)w;
    }
}

/* ---- the status line ------------------------------------------------ */

void pane_report_status(pane *p) {
    tab *t = active_tab(p);
    if (t->list != NULL) {
        size_t total = fdk_list_row_count(t->list);
        size_t sel = fdk_list_selected_count(t->list);
        off_t sel_bytes = 0;
        if (sel > 0) {
            fdk_u64 ids[512];
            size_t n = fdk_list_bound_selected_ids(t->list, ids,
                                                   512);
            for (size_t i = 0; i < n; i++) {
                const file_info *fi = fmodel_info_of(ids[i]);
                if (fi != NULL) {
                    sel_bytes += fi->size;
                }
            }
        }
        char sb[48];
        fi_fmt_size(sel_bytes, sb, sizeof sb);
        fi_set_status("%zu items%s%s — %s", total,
                      (sel > 0) ? ", " : "",
                      (sel > 0) ? "" : "", t->dir);
        if (sel > 0) {
            fi_set_status("%zu of %zu selected (%s) — %s", sel,
                          total, sb, t->dir);
        }
    } else {
        fi_set_status("%s", t->dir);
    }
}

/* ---- type-ahead find ------------------------------------------------ */

void pane_typeahead(pane *p, const char *ch) {
    tab *t = active_tab(p);
    if (t->list == NULL) {
        return;
    }
    static char query[64];
    static size_t query_len;
    size_t n = fdk_list_row_count(t->list);
    for (size_t i = 0; i < n; i++) {
        fdk_u64 id = fdk_list_bound_row_id(t->list, i);
        const file_info *fi = fmodel_info_of(id);
        if (fi != NULL &&
            strncasecmp(fi->name, query, query_len) == 0) {
            (void)fdk_list_select(t->list, i);
            return;
        }
    }
    (void)ch;
}

/* ---- the recursive search -------------------------------------------
 *
 * A timer walks directories breadth-first, a bounded number per
 * tick, appending matches to the filter's needle... no — the honest
 * shape: matches are collected and shown by navigating a "search
 * results" view. We keep the results in the model itself: the
 * search REPLACES the store with the matches (paths as names), and
 * stopping reloads the original dir. */

typedef struct search_state {
    pane *pane;
    char needle[128];
    char queue[2048][FI_PATH_MAX];
    size_t q_head, q_tail, q_count;
    size_t found;
    size_t budget_per_tick;
    bool done;
    fdk_timer *timer;
} search_state;

static search_state g_search;

static void search_tick(fdk_timer *timer, void *user) {
    (void)timer;
    search_state *s = user;
    if (s->done) {
        return;
    }
    size_t processed = 0;
    while (processed < s->budget_per_tick && s->q_count > 0) {
        char dir[FI_PATH_MAX];
        snprintf(dir, sizeof dir, "%.4000s", s->queue[s->q_head]);
        s->q_head = (s->q_head + 1) % 2048;
        s->q_count--;
        processed++;

        DIR *d = opendir(dir);
        if (d == NULL) {
            continue;
        }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') {
                continue;
            }
            if (strcasestr_f(de->d_name, s->needle) != NULL) {
                file_info *fi = calloc(1, sizeof *fi);
                if (fi != NULL) {
                    snprintf(fi->name, sizeof fi->name, "%.240s",
                             de->d_name);
                    snprintf(fi->path, sizeof fi->path, "%.3700s/%.190s",
                             dir, de->d_name);
                    fs_stat_entry(fi);
                    fdk_model_row row = fdk_model_row_append(
                        fmodel.store, FDK_MODEL_ROW_NONE);
                    (void)fdk_model_set_text(fmodel.store, row,
                                             FM_COL_NAME, fi->name);
                    (void)fdk_model_set_int(
                        fmodel.store, row, FM_COL_SIZE,
                        (fdk_i64)fi->size);
                    (void)fdk_model_set_int(
                        fmodel.store, row, FM_COL_MTIME,
                        (fdk_i64)fi->mtime);
                    (void)fdk_model_set_text(
                        fmodel.store, row, FM_COL_TYPE,
                        fi->is_dir ? "Folder"
                                   : mime_category(fi->mime));
                    (void)fdk_model_set_ptr(fmodel.store, row,
                                            FM_COL_PTR, fi);
                    s->found++;
                }
            }
            /* descend (stat: d_type is not POSIX) */
            struct stat dst_st;
            char full[FI_PATH_MAX];
            snprintf(full, sizeof full, "%.3800s/%.180s", dir, de->d_name);
            if (stat(full, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
                char sub[FI_PATH_MAX];
                snprintf(sub, sizeof sub, "%.4000s", full);
                if (s->q_count < 2048) {
                    snprintf(s->queue[s->q_tail], FI_PATH_MAX, "%s",
                             sub);
                    s->q_tail = (s->q_tail + 1) % 2048;
                    s->q_count++;
                }
            }
        }
        closedir(d);
    }
    fi_set_status("Searching for \"%s\": %zu found so far",
                  s->needle, s->found);
    if (s->q_count == 0) {
        s->done = true;
        fi_set_status("Search finished: %zu matches for \"%s\"",
                      s->found, s->needle);
        fdk_timer_remove(s->timer);
        s->timer = NULL;
    }
}

static const char *strcasestr_f(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) {
        return hay;
    }
    for (const char *s = hay; *s != '\0'; s++) {
        size_t i = 0;
        while (i < nl && s[i] != '\0' &&
               tolower_f((unsigned char)s[i]) ==
                   tolower_f((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) {
            return s;
        }
    }
    return NULL;
}

static int tolower_f(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

void pane_start_search(pane *p, const char *root, const char *needle) {
    pane_stop_search(p);
    tab *t = active_tab(p);
    fdk_model_clear(fmodel.store);
    for (size_t i = 0; i < fmodel.info_count; i++) {
        free(fmodel.infos[i]);
    }
    fmodel.info_count = 0;

    g_search.pane = p;
    snprintf(g_search.needle, sizeof g_search.needle, "%s", needle);
    snprintf(g_search.queue[0], FI_PATH_MAX, "%.4000s", root);
    g_search.q_head = 0;
    g_search.q_tail = 1;
    g_search.q_count = 1;
    g_search.found = 0;
    g_search.done = false;
    g_search.budget_per_tick = 8; /* dirs per 30ms tick */
    p->searching = true;
    snprintf(p->search_root, sizeof p->search_root, "%s", root);

    g_search.timer = fdk_timer_add(
        fapp.base.ctx, 30, true, search_tick, &g_search);
    pane_build_view(p, t);
    fi_set_status("Searching %s for \"%s\"…", root, needle);
}

void pane_stop_search(pane *p) {
    if (g_search.timer != NULL) {
        fdk_timer_remove(g_search.timer);
        g_search.timer = NULL;
    }
    if (p->searching) {
        p->searching = false;
        pane_reload(p);
    }
}

/* ---- the sidebar ---------------------------------------------------- */

typedef struct place {
    const char *label;
    const char *icon;
    fdk_row_icon glyph;
    char path[FI_PATH_MAX];
} place;

static place g_places[32];
static size_t g_place_count;

static void place_activated(fdk_widget *list, size_t row,
                            void *user) {
    (void)list;
    (void)user;
    if (row >= g_place_count) {
        return;
    }
    struct stat st;
    if (stat(g_places[row].path, &st) == 0 && S_ISDIR(st.st_mode)) {
        pane_navigate(active_pane(), g_places[row].path);
    } else {
        fi_set_status("Cannot open %s", g_places[row].path);
    }
}

static void build_places(void) {
    g_place_count = 0;
    struct {
        const char *label;
        const char *icon;
        fdk_row_icon glyph;
        const char *sub;
    } fixed[] = {
        {"Home", "home", FDK_ROW_ICON_HOME, NULL},
        {"Desktop", "home", FDK_ROW_ICON_HOME, "Desktop"},
        {"Documents", "doc", FDK_ROW_ICON_FOLDER, "Documents"},
        {"Downloads", "down", FDK_ROW_ICON_FOLDER, "Downloads"},
        {"Pictures", "image", FDK_ROW_ICON_FOLDER, "Pictures"},
        {"Music", "audio", FDK_ROW_ICON_FOLDER, "Music"},
        {"Videos", "video", FDK_ROW_ICON_FOLDER, "Videos"},
        {"Filesystem", "disk", FDK_ROW_ICON_DRIVE, NULL},
    };
    for (size_t i = 0;
         i < sizeof fixed / sizeof fixed[0] && g_place_count < 32;
         i++) {
        place *pl = &g_places[g_place_count];
        pl->label = fixed[i].label;
        pl->icon = fixed[i].icon;
        pl->glyph = fixed[i].glyph;
        if (fixed[i].sub != NULL) {
            snprintf(pl->path, sizeof pl->path, "%s/%s", fi_home(),
                     fixed[i].sub);
        } else if (i == 0) {
            snprintf(pl->path, sizeof pl->path, "%s", fi_home());
        } else {
            snprintf(pl->path, sizeof pl->path, "/");
        }
        g_place_count++;
    }
}

/* ---- build the whole UI --------------------------------------------- */

int panes_build(files_app *a) {
    build_places();

    fdk_widget *content = a->base.content;

    /* [toolbar][main = paned(sidebar, panes)] */
    fdk_widget *toolbar = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &toolbar);
    fdk_box_set_padding(toolbar, 4);
    fdk_box_set_spacing(toolbar, 2);
    a->toolbar = toolbar;

    fdk_widget *main_h = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &main_h);
    fdk_widget_set_expand(main_h, true, true);

    fdk_widget *paned = NULL;
    (void)fdk_paned_create(main_h, FDK_HORIZONTAL, &paned);
    fdk_widget_set_expand(paned, true, true);

    /* the sidebar */
    fdk_widget *sidebar_box = NULL;
    (void)fdk_box_create(paned, FDK_VERTICAL, &sidebar_box);
    fdk_box_set_padding(sidebar_box, 6);
    fdk_box_set_spacing(sidebar_box, 4);
    fdk_widget_set_natural_size(sidebar_box, 190, 0);
    fdk_widget_set_background_token(
        sidebar_box, FDK_TK_SIDEBAR_BACKGROUND);

    fdk_widget *places_label = NULL;
    (void)fdk_label_create(sidebar_box, a->base.font_ui, "Places",
                           &places_label);
    fdk_widget *places = NULL;
    (void)fdk_list_create(sidebar_box, a->base.font_ui, &places);
    fdk_list_set_selection_mode(places, FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_activate_on_single_click(places, true);
    fdk_widget_set_expand(places, false, true);
    fdk_widget_set_accessible_name(places, "Places sidebar");
    for (size_t i = 0; i < g_place_count; i++) {
        size_t idx = 0;
        (void)fdk_list_append(places, g_places[i].label, &idx);
        (void)fdk_list_row_set_icon(places, idx,
                                    g_places[i].glyph);
    }
    fdk_list_set_on_row_activate(places, place_activated, NULL);
    a->sidebar = places;

    /* the mounts section (live from /proc) */
    fdk_widget *mounts_label = NULL;
    (void)fdk_label_create(sidebar_box, a->base.font_ui, "Devices",
                           &mounts_label);
    fdk_widget *mounts_list = NULL;
    (void)fdk_list_create(sidebar_box, a->base.font_ui, &mounts_list);
    fdk_list_set_selection_mode(mounts_list,
                                FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_activate_on_single_click(mounts_list, true);
    fdk_widget_set_natural_size(mounts_list, 0, 120);
    mount_info *mi = NULL;
    size_t mn = fs_mounts(&mi);
    for (size_t i = 0; i < mn; i++) {
        char label[160];
        const char *base = strrchr(mi[i].dir, '/');
        base = (base != NULL && base[1] != '\0') ? base + 1
                                                 : mi[i].dir;
        snprintf(label, sizeof label, "%.100s (%.30s)", base, mi[i].fstype);
        size_t idx = 0;
        (void)fdk_list_append(mounts_list, label, &idx);
        (void)fdk_list_row_set_icon(mounts_list, idx,
                                    FDK_ROW_ICON_DRIVE);
    }
    fs_mounts_free(mi);
    fdk_list_set_on_row_activate(
        mounts_list,
        (fdk_list_row_activate_fn)mount_activated, NULL);
    fdk_widget_set_accessible_name(mounts_list, "Mounted devices");

    /* the trash row */
    fdk_widget *trash_btn = NULL;
    (void)fdk_button_create(sidebar_box, a->base.font_ui, "Trash",
                            &trash_btn);
    fdk_button_set_on_activate(trash_btn, trash_open_clicked, a);
    fdk_widget_set_accessible_name(trash_btn, "Open the trash");

    /* the right side: the pane stack (single pane first; the split
     * toggle clones the structure) */
    fdk_widget *panes_box = NULL;
    (void)fdk_box_create(paned, FDK_VERTICAL, &panes_box);
    fdk_widget_set_expand(panes_box, true, true);

    pane *p = calloc(1, sizeof *p);
    p->watch_fd = -1;
    fdk_widget *pn_box = NULL;
    (void)fdk_box_create(panes_box, FDK_VERTICAL, &pn_box);
    fdk_widget_set_expand(pn_box, true, true);
    p->notebook_host = pn_box;
    (void)fdk_notebook_create(pn_box, a->base.font_ui,
                              &p->notebook);
    fdk_widget_set_expand(p->notebook, true, true);

    fapp.panes[0] = p;
    fapp.pane_count = 1;
    fapp.active_pane = 0;

    pane_new_tab(p, fi_home());

    /* the drop target: the whole pane box accepts uri drops */
    (void)fdk_widget_set_drop_target(p->notebook_host,
                                     FDK_DRAG_FORMAT_URI_LIST);
    fdk_widget_set_drag_callback(p->notebook_host, pane_drop, p);
    (void)fdk_window_set_drop_formats(fapp.base.window,
                                      FDK_DRAG_FORMAT_URI_LIST);

    /* the filter box rides in the toolbar (built by menus.c with
     * the toolbar buttons; wired there) */
    return fapp.pane_count;
}

static void mount_activated(fdk_widget *list, size_t row,
                            void *user) {
    (void)list;
    (void)user;
    mount_info *mi = NULL;
    size_t mn = fs_mounts(&mi);
    if (row < mn) {
        pane_navigate(active_pane(), mi[row].dir);
    }
    fs_mounts_free(mi);
}

static void trash_open_clicked(fdk_widget *btn, void *user) {
    (void)btn;
    (void)user;
    ops_open_trash();
}

void panes_shutdown(void) {
    for (int i = 0; i < fapp.pane_count; i++) {
        pane *p = fapp.panes[i];
        if (p != NULL) {
            pane_crumbs_free(p);
            if (p->watch_fd >= 0) {
                fs_watch_stop(p->watch_fd);
                p->watch_fd = -1;
            }
            for (size_t t = 0; t < p->tab_count; t++) {
                free(p->tabs[t]);
            }
            free(p);
            fapp.panes[i] = NULL;
        }
    }
    fapp.pane_count = 0;
}
