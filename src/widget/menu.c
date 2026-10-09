#define FDK_LOG_TAG "widgets"

/*
 * menu.c — Menu model + popup session + MenuBar (Phase 9 completion)
 *
 * Three cooperating pieces:
 *
 *   1. The MODEL (fdk_menu): an item list the application owns.
 *      Items are individually allocated so handles stay stable as
 *      the list grows.
 *
 *   2. The VIEW (fdk_menu_view_class_def): a widget that renders a
 *      model's items (rows, check/radio glyphs, submenu arrows,
 *      shortcut labels) and turns pointer/keyboard input into model
 *      actions. Views live inside popup windows' roots.
 *
 *   3. The SESSION (fdk_menu_session): the popup-window chain. It
 *      owns the toolkit popup windows (auto-painted, destroy-
 *      notified), routes "open/close/switch" between bar, submenus,
 *      and the keyboard, and tears the chain down on dismissal.
 *
 * Sessions are static slots (max 4 concurrent chains: one per
 * menubar/context/combo trigger — a fifth popup request closes the
 * oldest first, with a warning). The view and the bar hold borrowed
 * session pointers; the destroy-notify hook on every popup window
 * keeps those references from dangling across any destroy path
 * (app-driven, parent-window sweep, shutdown force-destroy).
 *
 * X11 vs Wayland grab notes: X11 grabs do not stack, so closing a
 * submenu re-asserts the parent popup's grab (fdk__window_regrab);
 * out-of-bounds MOTION under an X11 grab still arrives at the popup
 * (reported against the grab window), which the session uses to
 * keep bar hover-switching alive. Wayland's xdg_popup grab DOES
 * return to the parent automatically (protocol guarantee), and
 * nested popups MUST each grab ("the parent of a grabbing popup
 * must ... be another xdg_popup with an explicit grab") — both
 * behaviors live in the backend, not here.
 */

#include "widgets_internal.h"
#include "menu_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"

#include "core/alloc_internal.h"
#include "core/shortcut_internal.h"
#include "core/log_internal.h"

#include <string.h>
#include <stdio.h>

/* ---- tunables (layout constants; the row height minimum is the
 * themed FDK_TM_MENU_ITEM_HEIGHT metric) ---- */

#define MENU_PAD_X 8       /* row text left/right padding        */
#define MENU_GUTTER 22     /* check/radio gutter width           */
#define MENU_ARROW_GUTTER 18 /* submenu arrow column width       */
#define MENU_SEP_H 9       /* separator row height               */
#define MENU_MIN_W 48      /* never narrower than this           */
#define MENU_MAX_H 512     /* clamp: no scrolling in v1 (docs)   */
#define MENU_FONT_PAD 8    /* row height = font extent + this   */

/* Depth bound for every recursive model walk (submenu recursion in
 * the 1.3.3 accelerator scan and the cycle check in
 * fdk_menu_item_set_submenu). set_submenu refuses cycles outright;
 * the bound is defense-in-depth so a hand-corrupted model can never
 * hang a walk. */
#define MENU_ACCEL_MAX_DEPTH 16

#define BAR_PAD_X 10       /* menubar title padding              */
#define BAR_LEFT 6         /* menubar leading inset              */

/* =====================================================================
 * The model
 * ===================================================================== */

typedef enum fdk_menu_item_kind {
    FDK_MIK_NORMAL = 0,
    FDK_MIK_SEPARATOR = 1,
    FDK_MIK_CHECK = 2,
    FDK_MIK_RADIO = 3,
} fdk_menu_item_kind;

struct fdk_menu_item {
    char *text;       /* owned; NULL for separators; mnemonic
                      * markers ("&F") already stripped      */
    char *shortcut;   /* owned display-only label            */
    /* 1.3.6 — the mnemonic (underlined-letter Alt+letter
     * navigation): the lowercased marker letter (0 = none) and its
     * byte range in `text` (start inclusive, end exclusive — the
     * underline's x-extent is measured over exactly this span). */
    fdk_u32 mnemonic;
    size_t mn_start;
    size_t mn_end;
    fdk_menu_item_kind kind;
    bool enabled;
    bool checked;
    fdk_menu_activate_fn on_activate; /* per-item */
    void *on_activate_user;
    fdk_menu *submenu; /* borrowed */
    fdk_menu *owner;  /* the menu that appended us — set once
                      * at creation, never changed; lets the radio
                      * group rule run from the DIRECT setter
                      * (1.3.0), matching fdk_radio_set_checked. */
};

struct fdk_menu {
    fdk_font *font;  /* borrowed — shared by every view of it */
    fdk_menu_item **items;
    size_t count;
    size_t cap;
    fdk_menu_activate_fn on_activate; /* menu-wide fallback */
    void *on_activate_user;
    /* 1.3.2 — bars currently listing this model (fdk_menu_bar_append
     * registers; bar_destroy detaches; fdk_menu_destroy removes the
     * listing). Sessions are swept globally at destroy instead (the
     * session table is file-static). Without this registry a model
     * destroyed while a bar listed it left the bar's borrowed
     * pointer dangling for the NEXT title click (and any popup
     * showing it painting freed rows — the dormant half surfaced
     * when the Wayland first-configure EXPOSE began painting
     * quiescent popups). */
    struct fdk_menu_bar **bars;
    size_t bar_count;
    size_t bar_cap;
};

fdk_result fdk_menu_create(fdk_font *font, fdk_menu **out_menu) {
    if (out_menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu *m = fdk_alloc(sizeof(fdk_menu));
    if (m == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    m->font = font;
    m->items = NULL;
    m->count = 0;
    m->cap = 0;
    m->on_activate = NULL;
    m->on_activate_user = NULL;
    m->bars = NULL;
    m->bar_count = 0;
    m->bar_cap = 0;
    *out_menu = m;
    return FDK_OK;
}

/* Defined after the bar type (the sweep touches bar internals). */
static void menu_detach_bars(fdk_menu *menu);
/* Defined after the session machinery (sweeps active sessions). */
static void menu_close_sessions_showing(fdk_menu *menu);

void fdk_menu_destroy(fdk_menu *menu) {
    if (menu == NULL) {
        return;
    }
    /* 1.3.2 — close everything still showing this model BEFORE the
     * memory goes: every active session level whose chain entry
     * borrowed it (popup windows die topmost-first inside the close,
     * taking their views with them), then every menu bar listing it
     * (the title is removed from the bar; the bar relayouts). This
     * is the GTK-shaped contract: destroying a model closes its
     * menus. The pre-1.3.2 code freed the rows and left the popups
     * painting freed memory the moment anything drove a paint (the
     * Wayland first-configure EXPOSE detonated it under ASan; X11's
     * map-Expose always could have). */
    menu_close_sessions_showing(menu);
    menu_detach_bars(menu);

    for (size_t i = 0; i < menu->count; i++) {
        fdk_free(menu->items[i]->text);
        fdk_free(menu->items[i]->shortcut);
        fdk_free(menu->items[i]);
    }
    fdk_free(menu->items);
    fdk_free(menu);
}

size_t fdk_menu_item_count(fdk_menu *menu) {
    return (menu != NULL) ? menu->count : 0;
}

/* Minimal UTF-8 decode for the mnemonic letter (the length was
 * derived from the lead byte just above; malformed tail bytes
 * decode to replacement-ish values that simply never match a key —
 * harmless). */
static fdk_u32 mn_decode(const char *p, size_t len) {
    const unsigned char *u = (const unsigned char *)p;
    if (len >= 1 && (u[0] & 0x80u) == 0u) {
        return (fdk_u32)u[0];
    }
    if (len >= 2 && (u[0] & 0xE0u) == 0xC0u) {
        return ((fdk_u32)(u[0] & 0x1Fu) << 6) | (fdk_u32)(u[1] & 0x3Fu);
    }
    if (len >= 3 && (u[0] & 0xF0u) == 0xE0u) {
        return ((fdk_u32)(u[0] & 0x0Fu) << 12) |
               ((fdk_u32)(u[1] & 0x3Fu) << 6) | (fdk_u32)(u[2] & 0x3Fu);
    }
    if (len >= 4 && (u[0] & 0xF8u) == 0xF0u) {
        return ((fdk_u32)(u[0] & 0x07u) << 18) |
               ((fdk_u32)(u[1] & 0x3Fu) << 12) |
               ((fdk_u32)(u[2] & 0x3Fu) << 6) | (fdk_u32)(u[3] & 0x3Fu);
    }
    return (fdk_u32)u[0];
}

/* ---- 1.3.6: mnemonic parsing ----------------------------------------
 *
 * Labels may carry "&X" markers: the NEXT character (a full UTF-8
 * codepoint) becomes the item's mnemonic — rendered underlined, and
 * reachable as Alt+X (bar titles: opens the menu; open menus: the
 * letter alone or with Alt activates the item). "&&" collapses to a
 * literal '&' (a menu called "Fish && Chips" needs one). The stored
 * text is the DISPLAY string (markers stripped); mn_start/mn_end
 * name the underlined codepoint's byte range inside it. Case folds
 * to lowercase — mnemonics are case-insensitive by convention.
 *
 * The first marker wins; later ones stay literal (a label can only
 * point at one letter — deterministic, documented, and what GTK/Qt
 * do). A '&' at the very end (dangling) is literal too: parsing must
 * never reject or alter a label the app considers valid. */
static char *mn_strip(const char *in, fdk_u32 *out_letter,
                      size_t *out_start, size_t *out_end) {
    *out_letter = 0;
    *out_start = 0;
    *out_end = 0;
    if (in == NULL) {
        return NULL;
    }
    size_t len = strlen(in);
    char *out = fdk_alloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t r = 0; /* read cursor */
    size_t w = 0; /* write cursor */
    bool taken = false;
    while (r < len) {
        if (in[r] == '&' && !taken && r + 1 < len) {
            if (in[r + 1] == '&') {
                out[w++] = '&';
                r += 2;
                continue;
            }
            /* The mnemonic codepoint starts at r+1. Measure its
             * UTF-8 length the same way the text layer does. */
            size_t clen = 1;
            unsigned char c0 = (unsigned char)in[r + 1];
            if ((c0 & 0xE0u) == 0xC0u) {
                clen = 2;
            } else if ((c0 & 0xF0u) == 0xE0u) {
                clen = 3;
            } else if ((c0 & 0xF8u) == 0xF0u) {
                clen = 4;
            }
            if (r + 1 + clen > len) {
                clen = 1; /* truncated sequence: literal fallback */
            }
            *out_start = w;
            *out_end = w + clen;
            memcpy(out + w, in + r + 1, clen);
            /* Lowercase ASCII for the case-insensitive match; the
             * underline still renders the ORIGINAL codepoint. */
            fdk_u32 cp = mn_decode(in + r + 1, clen);
            if (cp >= 'A' && cp <= 'Z') {
                cp += 32u;
            }
            *out_letter = cp;
            w += clen;
            r += 1 + clen;
            taken = true;
            continue;
        }
        out[w++] = in[r++];
    }
    out[w] = '\0';
    return out;
}

/* Forward declarations: the view's mnemonic Alt-fallback routes to
 * the bar machinery, which is defined further down this file (the
 * view code precedes the bar section). */
typedef struct fdk_menu_bar fdk_menu_bar;
static fdk_menu_bar *bar_of(fdk_widget *w);

/* Case-insensitive mnemonic/key match (both sides lowercased ASCII;
 * non-ASCII compares exact — the document-level rule). */
static bool mn_key_matches(const fdk_key_event *key, fdk_u32 mnemonic) {
    if (mnemonic == 0 || key == NULL || key->codepoint == 0) {
        return false;
    }
    fdk_u32 cp = key->codepoint;
    if (cp >= 'A' && cp <= 'Z') {
        cp += 32u;
    }
    return cp == mnemonic;
}

/* Draws the mnemonic underline under text drawn at (x, baseline):
 * the span [mn_start, mn_end) measured as two prefix advances. */
static void mn_paint_underline(fdk_surface *s, const fdk_font *font,
                               const char *text, fdk_u32 mnemonic,
                               size_t mn_start, size_t mn_end,
                               fdk_i32 x, fdk_i32 baseline, fdk_color col) {
    if (mnemonic == 0 || font == NULL || text == NULL) {
        return;
    }
    fdk_text_metrics a = {0, 0, 0};
    fdk_text_metrics b = {0, 0, 0};
    if (!fdk_ok(fdk_font_measure_utf8(font, text, mn_start, &a)) ||
        !fdk_ok(fdk_font_measure_utf8(font, text, mn_end, &b))) {
        return;
    }
    fdk_i32 uw = b.advance_width - a.advance_width;
    if (uw <= 0) {
        return;
    }
    /* Two pixels under the baseline reads as an underline at FDK's
     * typical menu sizes (14-16 px fonts); one pixel can vanish
     * against the row's background on low-contrast themes. */
    fdk_rect u = {x + a.advance_width, baseline + 2, uw, 1};
    fdk_surface_fill_rect(s, u, col);
}

static fdk_menu_item *menu_new_item(fdk_menu *menu, const char *text,
                                    fdk_menu_item_kind kind) {
    if (menu->count == menu->cap) {
        size_t ncap = menu->cap * 2 + 4;
        if (ncap < menu->cap || /* wrapped */
            ncap > SIZE_MAX / sizeof(fdk_menu_item *)) {
            return NULL; /* refuse absurd growth */
        }
        fdk_menu_item **ni =
            fdk_realloc(menu->items, ncap * sizeof(fdk_menu_item *));
        if (ni == NULL) {
            return NULL;
        }
        menu->items = ni;
        menu->cap = ncap;
    }
    fdk_menu_item *it = fdk_alloc(sizeof(fdk_menu_item));
    if (it == NULL) {
        return NULL;
    }
    it->mnemonic = 0;
    it->mn_start = 0;
    it->mn_end = 0;
    if (text != NULL) {
        it->text = mn_strip(text, &it->mnemonic, &it->mn_start,
                            &it->mn_end);
    } else {
        it->text = NULL;
    }
    if (text != NULL && it->text == NULL) {
        fdk_free(it);
        return NULL;
    }
    it->shortcut = NULL;
    it->kind = kind;
    it->enabled = true;
    it->checked = false;
    it->on_activate = NULL;
    it->on_activate_user = NULL;
    it->submenu = NULL;
    it->owner = menu;
    menu->items[menu->count++] = it;
    return it;
}

fdk_result fdk_menu_append(fdk_menu *menu, const char *text,
                           fdk_menu_item **out_item) {
    if (menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_item *it = menu_new_item(menu, text, FDK_MIK_NORMAL);
    if (it == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    if (out_item != NULL) {
        *out_item = it;
    }
    return FDK_OK;
}

fdk_result fdk_menu_append_separator(fdk_menu *menu) {
    if (menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (menu_new_item(menu, NULL, FDK_MIK_SEPARATOR) == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    return FDK_OK;
}

fdk_result fdk_menu_append_check(fdk_menu *menu, const char *text,
                                 bool checked, fdk_menu_item **out_item) {
    if (menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_item *it = menu_new_item(menu, text, FDK_MIK_CHECK);
    if (it == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    it->checked = checked;
    if (out_item != NULL) {
        *out_item = it;
    }
    return FDK_OK;
}

fdk_result fdk_menu_append_radio(fdk_menu *menu, const char *text,
                                 bool checked, fdk_menu_item **out_item) {
    if (menu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_item *it = menu_new_item(menu, text, FDK_MIK_RADIO);
    if (it == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    it->checked = checked;
    if (out_item != NULL) {
        *out_item = it;
    }
    return FDK_OK;
}

static fdk_menu_item *item_arg(fdk_menu_item *item) {
    if (item == NULL) {
        return NULL;
    }
    return item;
}

fdk_result fdk_menu_item_set_text(fdk_menu_item *item, const char *text) {
    item = item_arg(item);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_u32 letter = 0;
    size_t ms = 0, me = 0;
    char *copy = (text != NULL) ? mn_strip(text, &letter, &ms, &me)
                                : NULL;
    if (text != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(item->text);
    item->text = copy;
    item->mnemonic = letter;
    item->mn_start = ms;
    item->mn_end = me;
    return FDK_OK;
}

fdk_u32 fdk_menu_item_get_mnemonic(fdk_menu_item *item) {
    return (item_arg(item) != NULL) ? item_arg(item)->mnemonic : 0;
}

const char *fdk_menu_item_text(fdk_menu_item *item) {
    return (item_arg(item) != NULL) ? item->text : NULL;
}

const char *fdk_menu_item_get_shortcut(fdk_menu_item *item) {
    return (item_arg(item) != NULL) ? item->shortcut : NULL;
}

fdk_menu_item_type fdk_menu_item_get_type(fdk_menu_item *item) {
    if (item_arg(item) == NULL) {
        return FDK_MENU_ITEM_NORMAL;
    }
    switch (item->kind) {
    case FDK_MIK_SEPARATOR:
        return FDK_MENU_ITEM_SEPARATOR;
    case FDK_MIK_CHECK:
        return FDK_MENU_ITEM_CHECK;
    case FDK_MIK_RADIO:
        return FDK_MENU_ITEM_RADIO;
    default:
        return FDK_MENU_ITEM_NORMAL;
    }
}

void fdk_menu_item_set_enabled(fdk_menu_item *item, bool enabled) {
    if (item_arg(item) != NULL) {
        item->enabled = enabled;
    }
}

bool fdk_menu_item_is_enabled(fdk_menu_item *item) {
    return (item_arg(item) != NULL) ? item->enabled : false;
}

void fdk_menu_item_set_checked(fdk_menu_item *item, bool checked) {
    item = item_arg(item);
    if (item == NULL) {
        return;
    }
    if (item->kind == FDK_MIK_RADIO && checked) {
        /* All radios of the same menu are one group: checking one
         * unchecks its radio siblings — the same rule the activation
         * path enforces, now reachable from the DIRECT setter via
         * the item's owner back-pointer (1.3.0; before, the setter
         * could produce two checked radios in one group, a state the
         * keyboard/activation paths never create). */
        if (item->owner != NULL) {
            fdk_menu *m = item->owner;
            for (size_t i = 0; i < m->count; i++) {
                fdk_menu_item *sib = m->items[i];
                if (sib != item && sib->kind == FDK_MIK_RADIO &&
                    sib->checked) {
                    sib->checked = false;
                }
            }
        }
        item->checked = true;
    } else if (item->kind == FDK_MIK_CHECK || item->kind == FDK_MIK_RADIO) {
        item->checked = checked;
    }
}

bool fdk_menu_item_is_checked(fdk_menu_item *item) {
    return (item_arg(item) != NULL) ? item->checked : false;
}

fdk_result fdk_menu_item_set_shortcut(fdk_menu_item *item,
                                      const char *shortcut) {
    item = item_arg(item);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char *copy = (shortcut != NULL) ? fdk__strdup(shortcut) : NULL;
    if (shortcut != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(item->shortcut);
    item->shortcut = copy;
    return FDK_OK;
}

/* 1.3.3: is `target` reachable from `from` via item->submenu links?
 * Bounded depth — this walks developer-built models, and the refusal
 * below is what actually keeps the graph acyclic; the bound only
 * guarantees this walk terminates even on a pre-existing cycle. */
static bool menu_reaches(const fdk_menu *from, const fdk_menu *target,
                         int depth) {
    if (from == NULL || depth > MENU_ACCEL_MAX_DEPTH) {
        return false;
    }
    if (from == target) {
        return true;
    }
    for (size_t i = 0; i < from->count; i++) {
        if (menu_reaches(from->items[i]->submenu, target, depth + 1)) {
            return true;
        }
    }
    return false;
}

fdk_result fdk_menu_item_set_submenu(fdk_menu_item *item,
                                     fdk_menu *submenu) {
    item = item_arg(item);
    if (item == NULL || item->kind != FDK_MIK_NORMAL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* 1.3.3 — cycle refusal: wiring a submenu that already reaches
     * this item's own menu would build an infinite menu tree. Every
     * walker of that tree (the session's chain-open recursion, the
     * accelerator scan, a11y tree walkers) would hang or balloon;
     * refusing at the setter turns the class into a loud developer
     * error instead. NULL always clears (no cycle possible). */
    if (submenu != NULL && item->owner != NULL &&
        menu_reaches(submenu, item->owner, 0)) {
        FDK_WARN("menu: set_submenu refused — would create a cycle");
        return FDK_ERR_INVALID_ARGUMENT;
    }
    item->submenu = submenu;
    return FDK_OK;
}

fdk_menu *fdk_menu_item_get_submenu(fdk_menu_item *item) {
    item = item_arg(item);
    return (item != NULL) ? item->submenu : NULL;
}

void fdk_menu_item_set_on_activate(fdk_menu_item *item,
                                   fdk_menu_activate_fn on_activate,
                                   void *user_data) {
    item = item_arg(item);
    if (item == NULL) {
        return;
    }
    item->on_activate = on_activate;
    item->on_activate_user = user_data;
}

void fdk_menu_set_on_activate(fdk_menu *menu,
                              fdk_menu_activate_fn on_activate,
                              void *user_data) {
    if (menu != NULL) {
        menu->on_activate = on_activate;
        menu->on_activate_user = user_data;
    }
}

/* ---- model geometry (shared by view + session) ---- */

/* Internal: a model's row height for normal rows. */
fdk_i32 fdk__menu_row_height(const fdk_menu *m) {
    fdk_i32 metric = fdk_theme_get_metric(NULL, FDK_TM_MENU_ITEM_HEIGHT);
    fdk_i32 fh = 0;
    if (m != NULL && m->font != NULL) {
        fdk_i32 fw = 0;
        fdk__text_extent(m->font, "Mg", &fw, &fh);
    }
    fdk_i32 h = fh + MENU_FONT_PAD;
    return (h > metric) ? h : metric;
}

/* Internal: which gutters the model's rows need. */
static void menu_gutters(const fdk_menu *m, bool *check_gutter,
                         bool *arrow_gutter) {
    *check_gutter = false;
    *arrow_gutter = false;
    if (m == NULL) {
        return;
    }
    for (size_t i = 0; i < m->count; i++) {
        if (m->items[i]->kind == FDK_MIK_CHECK ||
            m->items[i]->kind == FDK_MIK_RADIO) {
            *check_gutter = true;
        }
        if (m->items[i]->submenu != NULL) {
            *arrow_gutter = true;
        }
    }
}

/* Internal: a model's natural popup size (width, height) — the
 * session creates popup windows with this. min_width widens
 * (combo dropdowns match their combo). */
void fdk__menu_measure(const fdk_menu *m, fdk_i32 min_width,
                       fdk_i32 *out_w, fdk_i32 *out_h) {
    fdk_i32 w = MENU_MIN_W;
    fdk_i32 h = 0;
    bool cg = false, ag = false;
    menu_gutters(m, &cg, &ag);
    fdk_i32 rh = fdk__menu_row_height(m);
    if (m != NULL) {
        for (size_t i = 0; i < m->count; i++) {
            fdk_menu_item *it = m->items[i];
            if (it->kind == FDK_MIK_SEPARATOR) {
                h += MENU_SEP_H;
                continue;
            }
            h += rh;
            if (m->font == NULL || it->text == NULL) {
                continue;
            }
            fdk_i32 tw = 0, sw = 0;
            fdk__text_extent(m->font, it->text, &tw, NULL);
            if (it->shortcut != NULL) {
                fdk__text_extent(m->font, it->shortcut, &sw, NULL);
            }
            fdk_i32 need = MENU_PAD_X + tw + MENU_PAD_X / 2;
            if (sw > 0) {
                need += sw + MENU_PAD_X;
            }
            if (cg) {
                need += MENU_GUTTER;
            }
            if (ag) {
                need += MENU_ARROW_GUTTER;
            }
            need += MENU_PAD_X;
            if (need > w) {
                w = need;
            }
        }
    }
    if (w < min_width) {
        w = min_width;
    }
    if (h > MENU_MAX_H) {
        h = MENU_MAX_H;
    }
    if (h < 1) {
        h = 1;
    }
    *out_w = w;
    *out_h = h;
}

/* =====================================================================
 * The session (struct + slots live up here: the view handlers
 * reference session fields before the implementation section)
 * ===================================================================== */

#define FDK_MENU_MAX_CHAIN 8
#define FDK_MENU_MAX_SESSIONS 4

struct fdk_menu_session {
    bool active;
    uint64_t id;       /* monotonic slot identity: a callback may
                         recycle the slot (close_all + session_new);
                         captured ids detect that and skip the
                         now-meaningless close */
    fdk_widget *bar;   /* borrowed MenuBar; NULL for context/combo */
    int bar_index;     /* open bar title index, -1 when none       */
    /* Optional one-shot notification when this session's chain
     * fully ends (any path — activation, dismissal, popup death):
     * combo dropdowns free their temporary model here. Fired after
     * the session is fully cleared (the callback may open a fresh
     * dropdown on a new slot). */
    void (*closed)(void *user);
    void *closed_user;
    struct {
        fdk_window *popup; /* owned (destroyed by the session)     */
        fdk_widget *view;  /* borrowed; dies with the popup's tree */
        fdk_menu *model;   /* borrowed                             */
    } chain[FDK_MENU_MAX_CHAIN];
    int depth;
};

static fdk_menu_session g_sessions[FDK_MENU_MAX_SESSIONS];
static uint64_t g_session_next_id = 1;

/* =====================================================================
 * The view widget
 * ===================================================================== */

typedef struct fdk_menu_view {
    fdk_widget base;
    fdk_menu *model;   /* borrowed */
    fdk_menu_session *session; /* borrowed; NULL in headless tests */
    int level;         /* chain depth (0 = top)                   */
    int highlight;     /* pointer-hover row, -1 none              */
    int key_row;       /* keyboard cursor, -1 none                */
    int open_row;      /* row whose submenu is open, -1 none      */
} fdk_menu_view;

/* The view's Alt-fallback routes Alt+letter naming ANOTHER bar
 * title while a chain is open (defined after the bar section). */
static bool mn_view_alt_bar_fallback(fdk_menu_view *v,
                                     const fdk_key_event *key);

static fdk_menu_view *view_of(fdk_widget *w) {
    return (fdk_menu_view *)(void *)w;
}

/* ---- a11y ---- */
/* Menu items are PAINTED ROWS, not widgets — the a11y tree exposes
 * the menu itself (role MENU) plus one VIRTUAL CHILD per row through
 * the class's virtual_* hooks (fdk_a11y_virtual_count/describe/
 * actions/perform). A bridge walks the menu view, then its virtual
 * children: role MENU_ITEM / CHECK_MENU_ITEM / RADIO_MENU_ITEM /
 * SEPARATOR, the item's text as the name, CHECKED for checked items,
 * ENABLED per the item's own flag, and ACTIVATE drives the exact
 * code path a pointer release drives (view_activate). Separators
 * have no name and no actions — they exist so the row indices a
 * bridge reports match what a sighted user counts. */

static fdk_i32 view_row_top(const fdk_menu_view *v, int row);
static void view_activate(fdk_menu_view *v, int row);

static size_t menu_view_virtual_count(const fdk_widget *w) {
    const fdk_menu_view *v = (const fdk_menu_view *)(const void *)w;
    return (v->model != NULL) ? v->model->count : 0;
}

static void menu_view_virtual_describe(const fdk_widget *w, size_t index,
                                       fdk_a11y_info *out) {
    const fdk_menu_view *v = (const fdk_menu_view *)(const void *)w;
    const fdk_menu *m = v->model;
    const fdk_menu_item *it = m->items[index];
    switch (it->kind) {
    case FDK_MIK_CHECK:
        out->role = FDK_A11Y_ROLE_CHECK_MENU_ITEM;
        break;
    case FDK_MIK_RADIO:
        out->role = FDK_A11Y_ROLE_RADIO_MENU_ITEM;
        break;
    case FDK_MIK_SEPARATOR:
        out->role = FDK_A11Y_ROLE_SEPARATOR;
        break;
    case FDK_MIK_NORMAL:
    default:
        out->role = FDK_A11Y_ROLE_MENU_ITEM;
        break;
    }
    if (it->text != NULL) {
        out->name = fdk__strdup(it->text);
    }
    if (it->shortcut != NULL) {
        out->description = fdk__strdup(it->shortcut);
    }
    out->states = FDK_A11Y_VISIBLE | FDK_A11Y_SHOWING;
    if (it->enabled) {
        out->states |= FDK_A11Y_ENABLED;
    }
    if (it->checked) {
        out->states |= FDK_A11Y_CHECKED;
    }
    if (it->submenu != NULL) {
        out->states |= FDK_A11Y_HAS_POPUP;
    }
    /* Bounds: the row's rect in the view's root-absolute space. */
    fdk_i32 rh = fdk__menu_row_height(m);
    fdk_i32 top = view_row_top(v, (int)index);
    fdk_i32 hh = (it->kind == FDK_MIK_SEPARATOR) ? MENU_SEP_H : rh;
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    out->bounds = (fdk_rect){abs.x, abs.y + top, abs.width, hh};
}

static fdk_a11y_action_set menu_view_virtual_actions(
    const fdk_widget *w, size_t index) {
    const fdk_menu_view *v = (const fdk_menu_view *)(const void *)w;
    const fdk_menu_item *it = v->model->items[index];
    if (it->kind == FDK_MIK_SEPARATOR || !it->enabled) {
        return 0;
    }
    return FDK_A11Y_ACTION_ACTIVATE;
}

static bool menu_view_virtual_perform(fdk_widget *w, size_t index,
                                      fdk_a11y_action action,
                                      double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    fdk_menu_view *v = (fdk_menu_view *)(void *)w;
    const fdk_menu_item *it = v->model->items[index];
    if (it->kind == FDK_MIK_SEPARATOR || !it->enabled) {
        return false;
    }
    view_activate(v, (int)index);
    return true;
}

static const fdk_a11y_class menu_view_a11y = {
    .role = FDK_A11Y_ROLE_MENU,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
    .virtual_count = menu_view_virtual_count,
    .virtual_describe = menu_view_virtual_describe,
    .virtual_actions = menu_view_virtual_actions,
    .virtual_perform = menu_view_virtual_perform,
};

const fdk_widget_class fdk_menu_view_class_def = {
    .size = sizeof(fdk_menu_view),
    .name = "menu-view",
    .handle_event = fdk__menu_view_handle_event,
    .paint = fdk__menu_view_paint,
    .measure = fdk__menu_view_measure,
    .arrange = NULL,
    .destroy = NULL,
    .a11y = &menu_view_a11y,
};

void fdk__menu_view_measure(fdk_widget *w, fdk_size *out) {
    fdk_menu_view *v = view_of(w);
    fdk_i32 width = 0, height = 0;
    fdk__menu_measure(v->model, 0, &width, &height);
    out->width = width;
    out->height = height;
}

/* Test-support/internal: bind a model to a standalone view (the
 * session does this at popup creation; headless tests create views
 * directly and need the same binding). Refuses non-views. */
fdk_result fdk__menu_view_bind(fdk_widget *w, fdk_menu *model) {
    if (w == NULL || w->klass != &fdk_menu_view_class_def ||
        model == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_view *v = view_of(w);
    v->model = model;
    v->highlight = -1;
    v->key_row = -1;
    v->open_row = -1;
    fdk_widget_invalidate(w);
    return FDK_OK;
}

/* Internal: row index at widget-local y (-1 outside rows). */
int fdk__menu_row_at(fdk_widget *w, fdk_f32 y) {
    fdk_menu_view *v = view_of(w);
    if (v->model == NULL) {
        return -1;
    }
    fdk_i32 rh = fdk__menu_row_height(v->model);
    fdk_i32 yy = 0;
    for (size_t i = 0; i < v->model->count; i++) {
        fdk_i32 hh = (v->model->items[i]->kind == FDK_MIK_SEPARATOR)
                         ? MENU_SEP_H
                         : rh;
        if ((fdk_i32)y >= yy && (fdk_i32)y < yy + hh) {
            return (int)i;
        }
        yy += hh;
    }
    return -1;
}

/* Internal: row i's top edge. */
static fdk_i32 view_row_top(const fdk_menu_view *v, int row) {
    fdk_i32 rh = fdk__menu_row_height(v->model);
    fdk_i32 yy = 0;
    for (int i = 0; i < row && (size_t)i < v->model->count; i++) {
        yy += (v->model->items[i]->kind == FDK_MIK_SEPARATOR) ? MENU_SEP_H
                                                              : rh;
    }
    return yy;
}

static void view_paint_check(fdk_surface *s, fdk_i32 cx, fdk_i32 cy,
                             fdk_color col) {
    fdk_surface_draw_line(s, cx - 4, cy, cx - 1, cy + 3, col);
    fdk_surface_draw_line(s, cx - 1, cy + 3, cx + 4, cy - 4, col);
}

static void view_paint_radio(fdk_surface *s, fdk_i32 cx, fdk_i32 cy,
                             fdk_color col) {
    /* Stroked diamond; a checked radio adds the inner cross so it
     * reads filled. Vector primitives only (font independence). */
    fdk_surface_draw_line(s, cx, cy - 4, cx + 4, cy, col);
    fdk_surface_draw_line(s, cx + 4, cy, cx, cy + 4, col);
    fdk_surface_draw_line(s, cx, cy + 4, cx - 4, cy, col);
    fdk_surface_draw_line(s, cx - 4, cy, cx, cy - 4, col);
}

static void view_paint_arrow(fdk_surface *s, fdk_i32 cx, fdk_i32 cy,
                             fdk_color col) {
    fdk_surface_draw_line(s, cx - 2, cy - 4, cx + 2, cy, col);
    fdk_surface_draw_line(s, cx + 2, cy, cx - 2, cy + 4, col);
}

void fdk__menu_view_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_menu_view *v = view_of(w);
    if (v->model == NULL || bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    bool cg = false, ag = false;
    menu_gutters(v->model, &cg, &ag);
    fdk_i32 rh = fdk__menu_row_height(v->model);

    /* Chrome: the menu surface + hairline border (1.4.0: the dedicated
     * MENU_BACKGROUND surface with rounded corners, one step above the
     * control fill — popups read as elevated layers, not as buttons).
     * Radius 0 degenerates to the plain square chrome. */
    fdk_i32 menu_r =
        fdk_theme_get_metric(NULL, FDK_TM_MENU_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect(surface, bounds, menu_r,
                                  fdk__pal_menu_bg());
    fdk_color border = fdk__pal_border();
    fdk_surface_draw_rounded_rect(surface, bounds, menu_r, border);

    fdk_i32 yy = bounds.y;
    for (size_t i = 0; i < v->model->count; i++) {
        fdk_menu_item *it = v->model->items[i];
        if (it->kind == FDK_MIK_SEPARATOR) {
            fdk_rect rule = {bounds.x + MENU_PAD_X / 2,
                             yy + MENU_SEP_H / 2,
                             bounds.width - MENU_PAD_X, 1};
            fdk_surface_fill_rect(surface, rule, border);
            yy += MENU_SEP_H;
            continue;
        }
        fdk_rect row = {bounds.x, yy, bounds.width, rh};
        bool lit = ((int)i == v->highlight || (int)i == v->key_row ||
                    (int)i == v->open_row) &&
                   it->enabled;
        if ((int)i == v->open_row && it->enabled) {
            /* The submenu-parent row stays lit while its child is
             * open — the selection fill so it reads "active", not
             * merely hovered (1.4.0: the token replaces the hardcoded
             * accent-alpha). */
            fdk_surface_fill_rect(surface, row, fdk__pal_selection());
        } else if (lit) {
            /* 1.4.0: the hover state is an inset rounded PILL, not a
             * full-width band — the modern menu-row look. The inset
             * keeps the pill inside the popup's rounded corners. */
            fdk_rect pill = {bounds.x + 3, yy + 1,
                             bounds.width - 6, rh - 2};
            if (pill.width > 0 && pill.height > 0) {
                fdk_i32 pill_r = menu_r > 3 ? menu_r - 3 : 2;
                fdk_surface_fill_rounded_rect(surface, pill, pill_r,
                                              fdk__pal_row_hover());
            }
        }

        fdk_i32 text_x = bounds.x + MENU_PAD_X;
        if (cg) {
            text_x += MENU_GUTTER;
            fdk_i32 cx = bounds.x + MENU_PAD_X + MENU_GUTTER / 2;
            fdk_i32 cy = yy + rh / 2;
            fdk_color col = it->enabled ? fdk__pal_accent()
                                        : fdk__pal_text_disabled();
            if (it->kind == FDK_MIK_CHECK && it->checked) {
                view_paint_check(surface, cx, cy, col);
            } else if (it->kind == FDK_MIK_RADIO) {
                view_paint_radio(surface, cx, cy, col);
                if (it->checked) {
                    view_paint_check(surface, cx, cy, col);
                }
            }
        }
        if (ag) {
            if (it->submenu != NULL) {
                fdk_color col = it->enabled ? fdk__pal_text()
                                            : fdk__pal_text_disabled();
                view_paint_arrow(surface,
                                 bounds.x + bounds.width -
                                     MENU_ARROW_GUTTER / 2 - MENU_PAD_X / 2,
                                 yy + rh / 2, col);
            }
        }
        if (v->model->font != NULL && it->text != NULL) {
            fdk_color col = it->enabled ? fdk__pal_text()
                                        : fdk__pal_text_disabled();
            fdk_i32 baseline =
                fdk__center_baseline(v->model->font, yy, rh);
            fdk__draw_text(surface, v->model->font, it->text, col,
                           text_x, baseline);
            mn_paint_underline(surface, v->model->font, it->text,
                               it->mnemonic, it->mn_start, it->mn_end,
                               text_x, baseline, col);
            if (it->shortcut != NULL) {
                fdk_i32 sw = 0;
                fdk__text_extent(v->model->font, it->shortcut, &sw, NULL);
                fdk_i32 sx = bounds.x + bounds.width - MENU_PAD_X - sw;
                if (ag) {
                    sx -= MENU_ARROW_GUTTER;
                }
                fdk__draw_text(surface, v->model->font, it->shortcut,
                               fdk__pal_text_disabled(), sx, baseline);
            }
        }
        yy += rh;
    }
}

/* ---- view-side session entry points (implemented below) ---- */

static void session_open_submenu(fdk_menu_session *s, fdk_menu_view *parent,
                                 int row);
static void session_close_above(fdk_menu_session *s, int level);

/* 1.3.2: close every active session level whose chain borrowed this
 * model (fdk_menu_destroy's first sweep — popups showing the model
 * die topmost-first, taking their views with them, while the model
 * is still whole). */
static void menu_close_sessions_showing(fdk_menu *menu) {
    for (int si = 0; si < FDK_MENU_MAX_SESSIONS; si++) {
        fdk_menu_session *s = &g_sessions[si];
        if (!s->active) {
            continue;
        }
        for (int lvl = 0; lvl < (int)s->depth; lvl++) {
            if (s->chain[lvl].model == menu) {
                session_close_above(s, lvl);
                break;
            }
        }
    }
}
static void session_switch_bar(fdk_menu_session *s, int dir);
static void session_bar_hover(fdk_menu_session *s, fdk_i32 x, fdk_i32 y);

/* Fires (once) and clears the session's closed hook — every path
 * that flips a session inactive ends here. */
static void menu_session_fire_closed(fdk_menu_session *s) {
    void (*fn)(void *) = s->closed;
    void *user = s->closed_user;
    s->closed = NULL;
    s->closed_user = NULL;
    if (fn != NULL) {
        fn(user);
    }
}

/* Fires the item's callback: per-item, else the model fallback. */
static void menu_fire(fdk_menu *m, fdk_menu_item *it) {
    if (it->on_activate != NULL) {
        it->on_activate(it, it->on_activate_user);
    } else if (m->on_activate != NULL) {
        m->on_activate(it, m->on_activate_user);
    }
}

/* Activates a row: flips check/radio state, fires the app callback,
 * THEN closes the chain. The callback runs first because closed
 * hooks may free the MODEL (combo dropdowns own theirs) — menu_fire
 * must not touch freed items. The post-callback close is id-guarded:
 * the callback may itself have closed/recycled the session (a
 * callback that opens another menu, destroys the bar, or kills the
 * window). */
static void view_activate(fdk_menu_view *v, int row) {
    fdk_menu *m = v->model;
    fdk_menu_item *it = m->items[row];
    if (it->kind == FDK_MIK_CHECK) {
        it->checked = !it->checked;
    } else if (it->kind == FDK_MIK_RADIO && !it->checked) {
        for (size_t i = 0; i < m->count; i++) {
            if (m->items[i]->kind == FDK_MIK_RADIO) {
                m->items[i]->checked = ((int)i == row);
            }
        }
    }
    fdk_menu_session *s = v->session;
    uint64_t sid = (s != NULL) ? s->id : 0;
    menu_fire(m, it);
    if (s != NULL && s->active && s->id == sid) {
        v->session = NULL; /* the view dies with the chain */
        fdk__menu_session_close_all(s);
    }
}

static int view_next_row(fdk_menu_view *v, int from, int dir) {
    if (v->model == NULL || v->model->count == 0) {
        return -1;
    }
    int i = from;
    for (size_t guard = 0; guard < v->model->count + 1; guard++) {
        i += dir;
        if (i < 0) {
            i = (int)v->model->count - 1;
        }
        if ((size_t)i >= v->model->count) {
            i = 0;
        }
        fdk_menu_item *it = v->model->items[i];
        if (it->kind != FDK_MIK_SEPARATOR && it->enabled) {
            return i;
        }
        if (i == from) {
            break;
        }
    }
    return -1;
}

bool fdk__menu_view_handle_event(fdk_widget *w,
                                 const fdk_widget_event *ev) {
    fdk_menu_view *v = view_of(w);
    if (v->model == NULL) {
        return false;
    }
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION: {
        int row = fdk__menu_row_at(w, ev->position.y);
        if (row == v->highlight) {
            return true;
        }
        v->highlight = row;
        /* Hovering a different row closes any submenu this view
         * opened (the classic menu behavior). */
        if (v->session != NULL && v->open_row != -1 && row != v->open_row) {
            session_close_above(v->session, v->level + 1);
            v->open_row = -1;
        }
        fdk_widget_invalidate(w);
        if (row >= 0 && v->session != NULL) {
            fdk_menu_item *it = v->model->items[row];
            if (it->enabled && it->submenu != NULL &&
                it->submenu->count > 0) {
                session_open_submenu(v->session, v, row);
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_DOWN: {
        int row = fdk__menu_row_at(w, ev->pointer.position.y);
        if (row < 0) {
            return true; /* inside the menu, between rows: swallow */
        }
        fdk_menu_item *it = v->model->items[row];
        if (it->kind == FDK_MIK_SEPARATOR || !it->enabled) {
            return true; /* separators and disabled rows swallow */
        }
        if (it->submenu != NULL) {
            if (it->submenu->count == 0) {
                return true;
            }
            if (v->open_row == row) {
                return true; /* already open (hover did it) */
            }
            if (v->session != NULL) {
                session_open_submenu(v->session, v, row);
            }
            return true;
        }
        view_activate(v, row);
        return true;
    }
    case FDK_WIDGET_POINTER_UP:
        return true; /* consumed: no click-through to anything under */
    case FDK_WIDGET_POINTER_LEAVE:
        if (v->highlight != -1) {
            v->highlight = -1;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_KEY_DOWN: {
        fdk_scancode key = ev->key.scancode;
        if (key == FDK_KEY_UP || key == FDK_KEY_DOWN) {
            int from = (v->key_row != -1) ? v->key_row
                                          : ((key == FDK_KEY_DOWN) ? -1 : 0);
            int next = view_next_row(
                v, from, (key == FDK_KEY_DOWN) ? 1 : -1);
            v->key_row = next;
            if (v->highlight != -1 && v->highlight != next) {
                v->highlight = -1;
            }
            fdk_widget_invalidate(w);
            return true;
        }
        if (key == FDK_KEY_HOME || key == FDK_KEY_END) {
            int next = view_next_row(
                v, (key == FDK_KEY_HOME) ? (int)v->model->count : -1,
                (key == FDK_KEY_HOME) ? 1 : -1);
            v->key_row = next;
            fdk_widget_invalidate(w);
            return true;
        }
        if (key == FDK_KEY_ENTER || key == FDK_KEY_SPACE) {
            if (v->key_row >= 0) {
                fdk_menu_item *it = v->model->items[v->key_row];
                if (it->enabled && it->submenu != NULL &&
                    it->submenu->count > 0 && v->session != NULL) {
                    session_open_submenu(v->session, v, v->key_row);
                    return true;
                }
                if (it->enabled) {
                    view_activate(v, v->key_row);
                }
            }
            return true;
        }
        /* 1.3.6 — mnemonics: a letter press (plain, or with Alt —
         * the classic both work; Ctrl/Super belong to accelerators)
         * activates the FIRST enabled item in THIS menu whose
         * mnemonic matches. First-match-wins is deterministic and
         * documented; duplicate letters in one menu are the app's
         * ambiguity to fix. Submenu items open their submenu, same
         * as Enter. */
        {
            fdk_u32 mods = ev->key.modifiers &
                           (FDK_MOD_CTRL | FDK_MOD_ALT | FDK_MOD_SHIFT |
                            FDK_MOD_SUPER);
            if ((mods & (FDK_MOD_CTRL | FDK_MOD_SUPER)) == 0 &&
                ev->key.codepoint != 0) {
                for (size_t mi = 0; mi < v->model->count; mi++) {
                    fdk_menu_item *it = v->model->items[mi];
                    if (it->kind == FDK_MIK_SEPARATOR || !it->enabled) {
                        continue;
                    }
                    if (mn_key_matches(&ev->key, it->mnemonic)) {
                        if (it->submenu != NULL && it->submenu->count > 0 &&
                            v->session != NULL) {
                            session_open_submenu(v->session, v, (int)mi);
                        } else {
                            view_activate(v, (int)mi);
                        }
                        return true;
                    }
                }
                /* Alt+letter that misses THIS menu may name another
                 * bar title — the classic jump-between-titles-while-
                 * open. The popup chain holds the keyboard grab, so
                 * the window-level mnemonic branch never sees the
                 * key; the VIEW is the only place to route it. */
                if ((mods & FDK_MOD_ALT) != 0 && v->session != NULL &&
                    v->session->bar != NULL &&
                    mn_view_alt_bar_fallback(v, &ev->key)) {
                    return true;
                }
            }
        }
        if (key == FDK_KEY_RIGHT) {
            if (v->key_row >= 0) {
                fdk_menu_item *it = v->model->items[v->key_row];
                if (it->submenu != NULL && it->submenu->count > 0 &&
                    v->session != NULL) {
                    session_open_submenu(v->session, v, v->key_row);
                    return true;
                }
            }
            if (v->level == 0 && v->session != NULL &&
                v->session->bar != NULL) {
                session_switch_bar(v->session, 1);
            }
            return true;
        }
        if (key == FDK_KEY_LEFT) {
            if (v->level > 0 && v->session != NULL) {
                session_close_above(v->session, v->level);
                return true;
            }
            if (v->level == 0 && v->session != NULL &&
                v->session->bar != NULL) {
                session_switch_bar(v->session, -1);
            }
            return true;
        }
        if (key == FDK_KEY_TAB) {
            return true; /* focus walk stays inside the menu chain */
        }
        return true; /* the menu grabbed the keyboard: it consumes */
    }
    default:
        return false;
    }
}

/* =====================================================================
 * The session (operations — the struct lives near the top)
 * ===================================================================== */

static fdk_menu_session *session_new(fdk_widget *bar) {
    fdk_menu_session *oldest = NULL;
    bool free_slot = false;
    for (int i = 0; i < FDK_MENU_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) {
            oldest = &g_sessions[i];
            free_slot = true;
            break;
        }
        if (oldest == NULL || g_sessions[i].depth < oldest->depth) {
            oldest = &g_sessions[i];
        }
    }
    if (!free_slot) {
        /* All slots busy: close the shallowest session (warned —
         * four simultaneous menu chains is already exotic). */
        FDK_WARN("menu: %d sessions active; closing one to fit another",
                 FDK_MENU_MAX_SESSIONS);
        fdk__menu_session_close_all(oldest);
    }
    oldest->active = true;
    oldest->id = g_session_next_id++;
    oldest->bar = bar;
    oldest->bar_index = -1;
    oldest->closed = NULL;
    oldest->closed_user = NULL;
    oldest->depth = 0;
    return oldest;
}

/* The popup windows' destroy-notify: a popup in our chain is being
 * destroyed by ANYONE (session teardown, parent-window sweep,
 * shutdown). Drop the reference; never destroy anything here. */
static void menu_popup_destroyed(fdk_window *window, void *user) {
    fdk_menu_session *s = user;
    if (!s->active) {
        return;
    }
    int at = -1;
    for (int i = 0; i < s->depth; i++) {
        if (s->chain[i].popup == window) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        return;
    }
    /* Everything at and above this level is going away with it (the
     * window-layer sweep destroys popup children topmost-first). */
    for (int i = at; i < s->depth; i++) {
        s->chain[i].popup = NULL;
        s->chain[i].view = NULL;
        s->chain[i].model = NULL;
    }
    s->depth = at;
    if (at == 0) {
        /* The whole chain is gone: end the session (unhighlight the
         * bar if we still can) and fire the one-shot closed hook. */
        fdk_widget *bar = s->bar;
        if (bar != NULL) {
            fdk__menu_bar_session_ended(bar, s);
        }
        s->bar = NULL;
        s->bar_index = -1;
        s->active = false;
        menu_session_fire_closed(s);
    }
}

/* The popup windows' event callback: CLOSE_REQUEST dismisses this
 * popup and everything above it (Escape in a submenu, a click
 * outside under a grab, the compositor's popup_done). Out-of-bounds
 * MOTION (X11 grabs report everything against the grab window)
 * keeps bar hover-switching alive: motion above the level-0 popup
 * is over the bar itself. */
static void menu_popup_event(fdk_window *window, const fdk_event_data *ev,
                             void *user) {
    fdk_menu_session *s = user;
    if (!s->active) {
        return;
    }
    int at = -1;
    for (int i = 0; i < s->depth; i++) {
        if (s->chain[i].popup == window) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        return;
    }
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        session_close_above(s, at);
        return;
    }
    if (ev->type == FDK_EVENT_POINTER_MOTION && at == 0 &&
        s->bar != NULL) {
        /* Popup-local coordinates; y < 0 = over the bar (the popup
         * hangs directly below it). */
        session_bar_hover(s, (fdk_i32)ev->pointer.position.x,
                          (fdk_i32)ev->pointer.position.y);
    }
    if (ev->type == FDK_EVENT_POINTER_MOTION && at > 0) {
        /* Hover left a submenu back onto its parent menu: the motion
         * reports against the (still-grabbed) submenu window with
         * out-of-bounds coordinates. Closing ourselves returns the
         * parent to normal hover. */
        if (ev->pointer.position.x < 0.0f &&
            ev->pointer.position.y >= 0.0f) {
            session_close_above(s, at);
        }
    }
}

/* Internal (menu_internal.h): closes a session's whole chain. Called
 * from the activation path (before the app's callback runs) and from
 * slot reuse. */
void fdk__menu_session_close_all(fdk_menu_session *s) {
    if (s == NULL || !s->active) {
        return;
    }
    session_close_above(s, 0);
}

static void session_close_above(fdk_menu_session *s, int level) {
    if (!s->active || level < 0) {
        return;
    }
    /* Destroy topmost-first (the Wayland xdg_popup rule: only the
     * topmost popup may be destroyed; X11 children die with parents
     * anyway — this order satisfies both). */
    for (int i = s->depth - 1; i >= level; i--) {
        fdk_window *pop = s->chain[i].popup;
        s->chain[i].popup = NULL;
        s->chain[i].view = NULL;
        s->chain[i].model = NULL;
        if (pop != NULL) {
            fdk_window_destroy(pop); /* destroy-notify sees NULLs */
        }
    }
    if ((int)s->depth > level) {
        s->depth = level;
    }
    if (level == 0) {
        if (s->bar != NULL) {
            fdk__menu_bar_session_ended(s->bar, s);
        }
        s->bar = NULL;
        s->bar_index = -1;
        s->active = false;
        menu_session_fire_closed(s);
        return;
    }
    /* The new topmost popup regains input: X11 needs an explicit
     * re-grab (grabs do not stack); Wayland returned it already. */
    if (s->depth > 0 && s->chain[s->depth - 1].popup != NULL) {
        fdk__window_regrab(s->chain[s->depth - 1].popup);
    }
    fdk_widget *view = (s->depth > 0) ? s->chain[s->depth - 1].view : NULL;
    if (view != NULL) {
        fdk_widget_focus(view);
        fdk_widget_invalidate(view);
    }
    /* Parent view's open_row must clear. */
    if (s->depth > 0 && s->chain[s->depth - 1].view != NULL) {
        fdk_menu_view *pv =
            (fdk_menu_view *)(void *)s->chain[s->depth - 1].view;
        if (pv->open_row != -1) {
            pv->open_row = -1;
            fdk_widget_invalidate(&pv->base);
        }
    }
}

/* Opens `model` as level `level` of the session, parented to
 * `parent_win` at its client-relative (x, y). */
static fdk_result session_open_level(fdk_menu_session *s, fdk_menu *model,
                                     fdk_window *parent_win, fdk_i32 x,
                                     fdk_i32 y, fdk_i32 min_width,
                                     int level) {
    if (model == NULL || model->count == 0 || parent_win == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (level >= FDK_MENU_MAX_CHAIN) {
        FDK_WARN("menu: chain depth cap (%d) reached", level);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_i32 w = 0, h = 0;
    fdk__menu_measure(model, min_width, &w, &h);

    fdk_window *pop = NULL;
    fdk_result r =
        fdk_window_create_popup(parent_win->ctx, parent_win, x, y, w, h,
                                &pop);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk__window_set_auto_paint(pop, true);
    fdk__window_set_destroy_notify(pop, menu_popup_destroyed, s);
    fdk_window_set_event_callback(pop, menu_popup_event, s);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(pop, &root);
    if (!fdk_ok(r)) {
        fdk_window_destroy(pop);
        return r;
    }
    fdk_widget *view_w = NULL;
    r = fdk_widget_create(root, &fdk_menu_view_class_def,
                          (fdk_rect){0, 0, w, h}, &view_w);
    if (!fdk_ok(r)) {
        fdk_window_destroy(pop);
        return r;
    }
    fdk_menu_view *v = view_of(view_w);
    v->model = model;
    v->session = s;
    v->level = level;
    v->highlight = -1;
    v->key_row = -1;
    v->open_row = -1;

    s->chain[level].popup = pop;
    s->chain[level].view = view_w;
    s->chain[level].model = model;
    if (level + 1 > s->depth) {
        s->depth = level + 1;
    }
    fdk_widget_set_can_focus(view_w, true);
    fdk_window_show(pop);
    fdk_widget_focus(view_w);
    return FDK_OK;
}

static void session_open_submenu(fdk_menu_session *s, fdk_menu_view *parent,
                                 int row) {
    fdk_menu_item *it = parent->model->items[row];
    if (it->submenu == NULL || it->submenu->count == 0) {
        return;
    }
    /* Close anything already open at deeper levels, then open. */
    if (s->depth > parent->level + 1) {
        session_close_above(s, parent->level + 1);
    }
    fdk_window *pw = s->chain[parent->level].popup;
    fdk_i32 w = 0, h = 0;
    fdk__menu_measure(it->submenu, 0, &w, &h);
    fdk_i32 row_top = view_row_top(parent, row);
    fdk_i32 rh = fdk__menu_row_height(parent->model);
    fdk_result r = session_open_level(
        s, it->submenu, pw, w - 6, row_top - 2, 0, parent->level + 1);
    if (!fdk_ok(r)) {
        return;
    }
    parent->open_row = row;
    fdk_widget_invalidate(&parent->base);
    /* Start the child's keyboard cursor on its first enabled row so
     * Right-then-Enter flows naturally. */
    fdk_menu_view *child =
        (fdk_menu_view *)(void *)s->chain[parent->level + 1].view;
    if (child != NULL) {
        child->key_row =
            view_next_row(child, -1, 1);
        (void)rh;
    }
}

static void session_switch_bar(fdk_menu_session *s, int dir) {
    if (s->bar == NULL) {
        return;
    }
    size_t n = fdk__menu_bar_count(s->bar);
    if (n == 0) {
        return;
    }
    /* Close the current chain but KEEP the session alive for the
     * switch. */
    int old = s->bar_index;
    for (int i = s->depth - 1; i >= 0; i--) {
        fdk_window *pop = s->chain[i].popup;
        s->chain[i].popup = NULL;
        s->chain[i].view = NULL;
        s->chain[i].model = NULL;
        if (pop != NULL) {
            fdk_window_destroy(pop);
        }
    }
    s->depth = 0;
    if (old < 0) {
        old = 0;
    }
    int next = (old + (int)dir) % (int)n;
    if (next < 0) {
        next += (int)n;
    }
    if (!fdk__menu_bar_open_index(s->bar, s, next)) {
        /* Open failed: end the session cleanly. */
        session_close_above(s, 0);
    }
}

/* Bar hover-switching from out-of-bounds motion under the X11 grab:
 * (mx, my) are popup-local; my < 0 means the pointer is above the
 * popup — i.e. over the bar. */
static void session_bar_hover(fdk_menu_session *s, fdk_i32 mx, fdk_i32 my) {
    if (s->bar == NULL || my >= 0) {
        return;
    }
    /* Popup-local (mx, my) → bar-local: the popup's top-left sits at
     * the open title's bottom-left. fdk__menu_bar_hit maps window
     * coordinates to a title index; the bar tracks that anchor. */
    fdk_i32 bx = 0, by = 0;
    if (!fdk__menu_bar_popup_anchor(s->bar, &bx, &by)) {
        return;
    }
    fdk_i32 wx = bx + mx;
    fdk_i32 wy = by + my;
    int hit = fdk__menu_bar_hit(s->bar, wx, wy);
    if (hit >= 0 && hit != s->bar_index) {
        session_switch_bar(s, (hit > s->bar_index) ? 1 : -1);
    }
}

/* ---- public context-menu entry point ---- */

fdk_result fdk__menu_popup_open_full(fdk_menu *menu, fdk_widget *anchor,
                                     fdk_i32 x, fdk_i32 y, fdk_i32 min_width,
                                     void (*on_closed)(void *),
                                     void *closed_user) {
    if (menu == NULL || anchor == NULL || menu->count == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_window *win =
        fdk__window_of_owner(fdk__widget_window_owner(anchor));
    if (win == NULL) {
        return FDK_ERR_INVALID_ARGUMENT; /* standalone tree: no window */
    }
    /* Window-relative anchor: the widget's absolute bounds + (x, y). */
    fdk_rect abs = fdk_widget_get_absolute_bounds(anchor);
    fdk_menu_session *s = session_new(NULL);
    s->closed = on_closed;
    s->closed_user = closed_user;
    return session_open_level(s, menu, win, abs.x + x, abs.y + y,
                              min_width, 0);
}

fdk_result fdk__menu_popup_open(fdk_menu *menu, fdk_widget *anchor,
                                fdk_i32 x, fdk_i32 y, fdk_i32 min_width) {
    return fdk__menu_popup_open_full(menu, anchor, x, y, min_width, NULL,
                                     NULL);
}

fdk_result fdk_menu_popup_at(fdk_menu *menu, fdk_widget *anchor,
                             fdk_i32 x, fdk_i32 y) {
    return fdk__menu_popup_open(menu, anchor, x, y, 0);
}

/* =====================================================================
 * The MenuBar widget
 * ===================================================================== */

typedef struct fdk_menu_bar_title {
    char *title;    /* owned; mnemonic markers stripped (1.3.6) */
    fdk_menu *menu; /* borrowed */
    fdk_rect rect;  /* bar-local layout slot */
    /* 1.3.6 — Alt+letter opens this title's menu. */
    fdk_u32 mnemonic;
    size_t mn_start;
    size_t mn_end;
} fdk_menu_bar_title;

typedef struct fdk_menu_bar {
    fdk_widget base;
    fdk_font *font; /* borrowed */
    fdk_menu_bar_title *titles;
    size_t count;
    size_t cap;
    int hover;        /* -1 none */
    int open_index;   /* -1 none; mirrors the session while open */
    int key_cursor;   /* keyboard title cursor, -1 none */
    fdk_menu_session *session; /* borrowed slot while a chain is open */
    fdk_i32 anchor_x; /* window x of the open popup's top-left   */
    fdk_i32 anchor_y; /* window y of the open popup's top-left   */
} fdk_menu_bar;

static fdk_menu_bar *bar_of(fdk_widget *w) {
    return (fdk_menu_bar *)(void *)w;
}


extern const fdk_widget_class fdk_menu_bar_class_def;

static void bar_layout(fdk_widget *w) {
    fdk_menu_bar *b = bar_of(w);
    fdk_i32 x = BAR_LEFT;
    for (size_t i = 0; i < b->count; i++) {
        fdk_i32 tw = 0;
        if (b->font != NULL && b->titles[i].title != NULL) {
            fdk__text_extent(b->font, b->titles[i].title, &tw, NULL);
        }
        if (tw < 8) {
            tw = 8;
        }
        b->titles[i].rect =
            (fdk_rect){x, 0, tw + BAR_PAD_X * 2, w->bounds.height};
        x += b->titles[i].rect.width;
    }
}
/* 1.3.2: remove this model's listing from every registered bar —
 * called by fdk_menu_destroy while the model is still whole. The
 * bar drops the title, relayouts, and repaints; the model's registry
 * entry is cleared by the caller's free of menu->bars. */
static void menu_detach_bars(fdk_menu *menu) {
    for (size_t bi = 0; bi < menu->bar_count; bi++) {
        fdk_menu_bar *b = menu->bars[bi];
        if (b == NULL) {
            continue;
        }
        size_t w = 0;
        for (size_t t = 0; t < b->count; t++) {
            if (b->titles[t].menu == menu) {
                fdk_free(b->titles[t].title);
                continue; /* drop the listing */
            }
            b->titles[w++] = b->titles[t];
        }
        b->count = w;
        bar_layout(&b->base); /* re-slot the remaining titles */
        fdk_widget_invalidate(&b->base);
    }
    fdk_free(menu->bars);
    menu->bars = NULL;
    menu->bar_count = 0;
    menu->bar_cap = 0;
}

/* 1.3.2: register a bar on the model it lists (fdk_menu_bar_append).
 * Returns false only on allocation failure — the append then fails
 * rather than leaving an unregistered dangling listing behind. */
static bool menu_attach_bar(fdk_menu *menu, fdk_menu_bar *bar) {
    for (size_t i = 0; i < menu->bar_count; i++) {
        if (menu->bars[i] == bar) {
            return true; /* already registered (multiple titles) */
        }
    }
    if (menu->bar_count == menu->bar_cap) {
        size_t ncap = menu->bar_cap * 2 + 2;
        if (ncap < menu->bar_cap ||
            ncap > SIZE_MAX / sizeof(fdk_menu_bar *)) {
            return false;
        }
        fdk_menu_bar **nb =
            fdk_realloc(menu->bars, ncap * sizeof(fdk_menu_bar *));
        if (nb == NULL) {
            return false;
        }
        menu->bars = nb;
        menu->bar_cap = ncap;
    }
    menu->bars[menu->bar_count++] = bar;
    return true;
}

/* 1.3.2: drop one bar from the model's registry (bar_destroy). */
static void menu_unattach_bar(fdk_menu *menu, fdk_menu_bar *bar) {
    for (size_t i = 0; i < menu->bar_count; i++) {
        if (menu->bars[i] == bar) {
            menu->bars[i] = menu->bars[menu->bar_count - 1];
            menu->bar_count--;
            return;
        }
    }
}


static void bar_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = 32;
    out->height = fdk__menu_row_height(NULL);
}

static void bar_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    bar_layout(w);
}

static void bar_paint(fdk_widget *w, fdk_surface *surface, fdk_rect bounds,
                      fdk_rect clip) {
    (void)clip;
    fdk_menu_bar *b = bar_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_surface_fill_rect(surface, bounds, fdk__pal_track());
    fdk_rect rule = {bounds.x, bounds.y + bounds.height - 1,
                     bounds.width, 1};
    fdk_surface_fill_rect(surface, rule, fdk__pal_border());
    if (b->font == NULL) {
        return;
    }
    for (size_t i = 0; i < b->count; i++) {
        fdk_rect r = b->titles[i].rect;
        r.x += bounds.x;
        r.y = bounds.y;
        bool lit = ((int)i == b->hover || (int)i == b->open_index ||
                    (int)i == b->key_cursor);
        if (lit) {
            fdk_surface_fill_rect(surface, r, fdk__pal_control_hover());
        }
        fdk_i32 tw = 0;
        fdk__text_extent(b->font, b->titles[i].title, &tw, NULL);
        fdk_i32 baseline =
            fdk__center_baseline(b->font, bounds.y, bounds.height);
        fdk_i32 tx = r.x + (r.width - tw) / 2;
        fdk__draw_text(surface, b->font, b->titles[i].title,
                       fdk__pal_text(), tx, baseline);
        mn_paint_underline(surface, b->font, b->titles[i].title,
                           b->titles[i].mnemonic, b->titles[i].mn_start,
                           b->titles[i].mn_end, tx, baseline,
                           fdk__pal_text());
    }
}

static void bar_destroy(fdk_widget *w) {
    fdk_menu_bar *b = bar_of(w);
    /* An open chain dies with the bar (popups are owned by the
     * session slot, which the destroy-notify unwinds). */
    if (b->session != NULL && b->session->active) {
        fdk_menu_session *s = b->session;
        b->session = NULL;
        session_close_above(s, 0);
    }
    for (size_t i = 0; i < b->count; i++) {
        /* 1.3.2: detach from the model's registry so a later
         * fdk_menu_destroy does not sweep a freed bar. */
        if (b->titles[i].menu != NULL) {
            menu_unattach_bar(b->titles[i].menu, b);
        }
        fdk_free(b->titles[i].title);
    }
    fdk_free(b->titles);
    b->titles = NULL;
    b->count = 0;
}

/* ---- a11y: bar titles as virtual children ----
 *
 * Each title is a virtual child with role MENU, the title text as
 * its name, HAS_POPUP + EXPANDED-while-open, and ACTIVATE that
 * replays the exact POINTER_DOWN semantics (toggle the open menu,
 * switch to another while a chain is open, or open fresh). */

static size_t bar_virtual_count(const fdk_widget *w) {
    const fdk_menu_bar *b = (const fdk_menu_bar *)(const void *)w;
    return b->count;
}

static void bar_virtual_describe(const fdk_widget *w, size_t index,
                                 fdk_a11y_info *out) {
    const fdk_menu_bar *b = (const fdk_menu_bar *)(const void *)w;
    out->role = FDK_A11Y_ROLE_MENU;
    if (b->titles[index].title != NULL) {
        out->name = fdk__strdup(b->titles[index].title);
    }
    out->states = FDK_A11Y_VISIBLE | FDK_A11Y_SHOWING |
                  FDK_A11Y_ENABLED | FDK_A11Y_HAS_POPUP;
    if (b->session != NULL && b->session->active &&
        b->open_index == (int)index) {
        out->states |= FDK_A11Y_EXPANDED;
    }
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_rect tr = b->titles[index].rect;
    out->bounds = (fdk_rect){abs.x + tr.x, abs.y + tr.y, tr.width,
                             tr.height};
}

static fdk_a11y_action_set bar_virtual_actions(const fdk_widget *w,
                                               size_t index) {
    const fdk_menu_bar *b = (const fdk_menu_bar *)(const void *)w;
    const fdk_menu *m = b->titles[index].menu;
    return (m != NULL && m->count > 0) ? FDK_A11Y_ACTION_ACTIVATE : 0;
}

static bool bar_virtual_perform(fdk_widget *w, size_t index,
                                fdk_a11y_action action, double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    fdk_menu_bar *b = bar_of(w);
    const fdk_menu *m = b->titles[index].menu;
    if (m == NULL || m->count == 0) {
        return false;
    }
    int hit = (int)index;
    if (b->session != NULL && b->session->active &&
        b->open_index == hit) {
        fdk_menu_session *s = b->session;
        session_close_above(s, 0);
    } else if (b->session != NULL && b->session->active) {
        session_switch_bar(b->session, (hit > b->open_index) ? 1 : -1);
    } else {
        fdk_menu_session *s = session_new(w);
        if (!fdk__menu_bar_open_index(w, s, hit)) {
            s->active = false;
            s->bar = NULL;
            return false;
        }
    }
    return true;
}

static const fdk_a11y_class menu_bar_a11y = {
    .role = FDK_A11Y_ROLE_MENU_BAR,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
    .virtual_count = bar_virtual_count,
    .virtual_describe = bar_virtual_describe,
    .virtual_actions = bar_virtual_actions,
    .virtual_perform = bar_virtual_perform,
};

const fdk_widget_class fdk_menu_bar_class_def = {
    .size = sizeof(fdk_menu_bar),
    .name = "menu-bar",
    .handle_event = fdk__menu_bar_handle_event,
    .paint = bar_paint,
    .measure = bar_measure,
    .arrange = bar_arrange,
    .destroy = bar_destroy,
    .a11y = &menu_bar_a11y,
};

/* Internal helpers shared with the session (menu_internal.h). */

size_t fdk__menu_bar_count(fdk_widget *w) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return 0;
    }
    return bar_of(w)->count;
}

/* Opens bar title `index` under session `s` (switching keeps the
 * session alive across a close). Returns success. */
bool fdk__menu_bar_open_index(fdk_widget *w, fdk_menu_session *s,
                              int index) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return false;
    }
    fdk_menu_bar *b = bar_of(w);
    if (index < 0 || (size_t)index >= b->count) {
        return false;
    }
    fdk_menu *m = b->titles[index].menu;
    if (m == NULL || m->count == 0) {
        return false;
    }
    fdk_window *win = fdk__window_of_owner(fdk__widget_window_owner(w));
    if (win == NULL) {
        return false; /* standalone tree: nothing to anchor to */
    }
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_rect tr = b->titles[index].rect;
    fdk_i32 x = abs.x + tr.x;
    fdk_i32 y = abs.y + w->bounds.height;
    fdk_result r = session_open_level(s, m, win, x, y, 0, 0);
    if (!fdk_ok(r)) {
        return false;
    }
    b->open_index = index;
    b->session = s;
    s->bar = w;
    s->bar_index = index;
    /* Remember the popup's anchor for bar hover-switching. */
    b->anchor_x = x;
    b->anchor_y = y;
    fdk_widget_invalidate(w);
    return true;
}

/* The session's chain fully closed (or switched away): unhighlight. */
void fdk__menu_bar_session_ended(fdk_widget *w, fdk_menu_session *s) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return;
    }
    fdk_menu_bar *b = bar_of(w);
    if (b->session == s) {
        b->session = NULL;
        b->open_index = -1;
        fdk_widget_invalidate(w);
    }
}

/* Window coordinates of the open popup's top-left (the hover-switch
 * mapping). False when no chain is open. */
bool fdk__menu_bar_popup_anchor(fdk_widget *w, fdk_i32 *out_x,
                                fdk_i32 *out_y) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return false;
    }
    fdk_menu_bar *b = bar_of(w);
    if (b->session == NULL || !b->session->active) {
        return false;
    }
    *out_x = b->anchor_x;
    *out_y = b->anchor_y;
    return true;
}

/* Window-coordinate hit test over titles. */
int fdk__menu_bar_hit(fdk_widget *w, fdk_i32 x, fdk_i32 y) {
    if (w == NULL || w->klass != &fdk_menu_bar_class_def) {
        return -1;
    }
    fdk_menu_bar *b = bar_of(w);
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_i32 lx = x - abs.x;
    fdk_i32 ly = y - abs.y;
    if (ly < 0 || ly >= w->bounds.height) {
        return -1;
    }
    for (size_t i = 0; i < b->count; i++) {
        fdk_rect r = b->titles[i].rect;
        if (lx >= r.x && lx < r.x + r.width) {
            return (int)i;
        }
    }
    return -1;
}

bool fdk__menu_bar_handle_event(fdk_widget *w,
                                const fdk_widget_event *ev) {
    fdk_menu_bar *b = bar_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION: {
        /* Motion positions are BAR-LOCAL; map to window coords for
         * the shared hit helper. */
        fdk_rect abs = fdk_widget_get_absolute_bounds(w);
        int hit = fdk__menu_bar_hit(w, abs.x + (fdk_i32)ev->position.x,
                                    abs.y + (fdk_i32)ev->position.y);
        if (hit != b->hover) {
            b->hover = hit;
            fdk_widget_invalidate(w);
        }
        return true;
    }
    case FDK_WIDGET_POINTER_DOWN: {
        fdk_rect abs = fdk_widget_get_absolute_bounds(w);
        int hit = fdk__menu_bar_hit(w, abs.x + (fdk_i32)ev->pointer.position.x,
                                    abs.y + (fdk_i32)ev->pointer.position.y);
        if (hit < 0) {
            return true; /* press on the bar's padding: swallow */
        }
        if (b->session != NULL && b->session->active &&
            b->open_index == hit) {
            /* Clicking the open title closes it (toggle). */
            fdk_menu_session *s = b->session;
            session_close_above(s, 0);
        } else if (b->session != NULL && b->session->active) {
            /* Another title while a chain is open: switch. */
            session_switch_bar(b->session,
                               (hit > b->open_index) ? 1 : -1);
        } else {
            fdk_menu_session *s = session_new(w);
            if (!fdk__menu_bar_open_index(w, s, hit)) {
                /* Nothing to open: end the fresh session. */
                s->active = false;
                s->bar = NULL;
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_LEAVE:
        if (b->hover != -1) {
            b->hover = -1;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_KEY_DOWN: {
        fdk_scancode key = ev->key.scancode;
        if (key == FDK_KEY_LEFT || key == FDK_KEY_RIGHT) {
            int n = (int)b->count;
            if (n == 0) {
                return true;
            }
            int cur = (b->key_cursor != -1) ? b->key_cursor : 0;
            cur += (key == FDK_KEY_RIGHT) ? 1 : -1;
            if (cur < 0) {
                cur = n - 1;
            }
            if (cur >= n) {
                cur = 0;
            }
            b->key_cursor = cur;
            fdk_widget_invalidate(w);
            return true;
        }
        if (key == FDK_KEY_DOWN || key == FDK_KEY_ENTER ||
            key == FDK_KEY_SPACE) {
            if (b->key_cursor >= 0 && b->session == NULL) {
                fdk_menu_session *s = session_new(w);
                if (!fdk__menu_bar_open_index(w, s, b->key_cursor)) {
                    s->active = false;
                    s->bar = NULL;
                }
            }
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

/* ---- MenuBar public API ---- */

fdk_result fdk_menu_bar_create(fdk_widget *parent, fdk_font *font,
                               fdk_widget **out_bar) {
    if (out_bar == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_menu_bar_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_menu_bar *b = bar_of(w);
    b->font = font;
    b->titles = NULL;
    b->count = 0;
    b->cap = 0;
    b->hover = -1;
    b->open_index = -1;
    b->key_cursor = -1;
    b->session = NULL;
    b->anchor_x = 0;
    b->anchor_y = 0;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_bar = w;
    return FDK_OK;
}

size_t fdk_menu_bar_count(fdk_widget *bar) {
    return fdk__menu_bar_count(bar);
}

fdk_result fdk_menu_bar_append(fdk_widget *bar, const char *title,
                               fdk_menu *menu) {
    if (bar == NULL || bar->klass != &fdk_menu_bar_class_def ||
        title == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_bar *b = bar_of(bar);
    if (b->count == b->cap) {
        size_t ncap = b->cap * 2 + 4;
        fdk_menu_bar_title *nt =
            fdk_realloc(b->titles, ncap * sizeof(fdk_menu_bar_title));
        if (nt == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        b->titles = nt;
        b->cap = ncap;
    }
    /* 1.3.2: register the bar on the model FIRST — if this fails
     * the listing is refused rather than left unregistered (an
     * unregistered listing would dangle at model destroy). */
    if (menu != NULL && !menu_attach_bar(menu, b)) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_u32 letter = 0;
    size_t ms = 0, me = 0;
    char *copy = mn_strip(title, &letter, &ms, &me);
    if (copy == NULL) {
        if (menu != NULL) {
            menu_unattach_bar(menu, b);
        }
        return FDK_ERR_OUT_OF_MEMORY;
    }
    b->titles[b->count].title = copy;
    b->titles[b->count].menu = menu;
    b->titles[b->count].rect = (fdk_rect){0, 0, 0, 0};
    b->titles[b->count].mnemonic = letter;
    b->titles[b->count].mn_start = ms;
    b->titles[b->count].mn_end = me;
    b->count++;
    bar_layout(bar);
    fdk_widget_invalidate(bar);
    /* Virtual children (the bar titles) changed. */
    fdk__a11y_notify(bar, FDK_A11Y_CHILDREN_CHANGED, 0);
    return FDK_OK;
}

fdk_result fdk_menu_bar_remove(fdk_widget *bar, size_t index) {
    if (bar == NULL || bar->klass != &fdk_menu_bar_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_bar *b = bar_of(bar);
    if (index >= b->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Removing the OPEN title closes the chain first. */
    if (b->session != NULL && b->session->active &&
        (int)index == b->open_index) {
        session_close_above(b->session, 0);
    }
    /* 1.3.2: detach the dropped listing from the model's registry —
     * the model must not sweep this bar at destroy for a title it no
     * longer carries (found live by test_menu's remove-then-destroy
     * ordering under ASan). */
    if (b->titles[index].menu != NULL) {
        menu_unattach_bar(b->titles[index].menu, b);
    }
    fdk_free(b->titles[index].title);
    memmove(&b->titles[index], &b->titles[index + 1],
            (b->count - index - 1) * sizeof(fdk_menu_bar_title));
    b->count--;
    if (b->key_cursor >= (int)b->count) {
        b->key_cursor = (int)b->count - 1;
    }
    bar_layout(bar);
    fdk_widget_invalidate(bar);
    /* Virtual children (the bar titles) changed. */
    fdk__a11y_notify(bar, FDK_A11Y_CHILDREN_CHANGED, 0);
    return FDK_OK;
}

void fdk_menu_bar_close(fdk_widget *bar) {
    if (bar == NULL || bar->klass != &fdk_menu_bar_class_def) {
        return;
    }
    fdk_menu_bar *b = bar_of(bar);
    if (b->session != NULL && b->session->active) {
        session_close_above(b->session, 0);
    }
}

/* =====================================================================
 * 1.3.3 — menu accelerators
 *
 * The window's KEY_DOWN dispatch calls fdk__menu_bar_accel_hit with
 * its root; the scan below is the whole feature. It is LIVE by
 * design: no registration, no cache, no invalidation protocol to
 * forget. A menu appended, retitled, destroyed, or re-shortcut
 * between keypresses is found (or not) exactly as the tree stands
 * at the moment of the press. The walk runs zero user code, so no
 * widget or model can die mid-scan; activation runs afterwards and
 * may destroy anything — the caller treats the returned item as
 * borrowed-instantly-consumed.
 *
 * Scan order is tree order (children in z-order), and within a bar,
 * title order, and within a menu, row order, recursing into
 * submenus. The first matching ENABLED item wins — deterministic
 * and documented. Disabled items do not fire (their rows swallow
 * clicks in the popup too).
 * ===================================================================== */

static bool accel_scan_model(const fdk_menu *m, const fdk_key_event *key,
                             int depth, fdk_menu_item **out_item) {
    if (m == NULL || depth > MENU_ACCEL_MAX_DEPTH) {
        return false;
    }
    for (size_t i = 0; i < m->count; i++) {
        fdk_menu_item *it = m->items[i];
        if (it->kind == FDK_MIK_SEPARATOR || !it->enabled) {
            continue;
        }
        if (it->shortcut != NULL) {
            fdk_u32 mods = 0;
            fdk_scancode code = 0;
            if (fdk_ok(fdk_shortcut_parse(it->shortcut, &mods, &code)) &&
                fdk__shortcut_matches(mods, code, key)) {
                *out_item = it;
                return true;
            }
        }
        if (it->submenu != NULL &&
            accel_scan_model(it->submenu, key, depth + 1, out_item)) {
            return true;
        }
    }
    return false;
}

static bool accel_scan_widget(fdk_widget *w, const fdk_key_event *key,
                              int depth, fdk_menu_item **out_item) {
    if (w == NULL || depth > MENU_ACCEL_MAX_DEPTH ||
        (w->flags & FDK_WF_DESTROYING) != 0) {
        return false;
    }
    if (w->klass == &fdk_menu_bar_class_def) {
        fdk_menu_bar *b = bar_of(w);
        for (size_t t = 0; t < b->count; t++) {
            if (accel_scan_model(b->titles[t].menu, key, 0, out_item)) {
                return true;
            }
        }
    }
    /* Descend in z-order (children array order — index 0 first). */
    for (size_t c = 0; c < w->child_count; c++) {
        if (accel_scan_widget(w->children[c], key, depth + 1, out_item)) {
            return true;
        }
    }
    return false;
}

/* 1.3.6 — Alt+letter with no chain open: open the first bar title
 * whose mnemonic matches (the keyboard twin of clicking the title).
 * Walks like the accelerator scan (tree order, first bar wins);
 * runs no user code; the open itself is the only thing that can
 * fail. The caller gates on the modifier state (exactly Alt, no
 * Ctrl/Super) and the popup-exemption. */
/* Opens bar title `t` — closing any chain already up on that bar
 * first (a mnemonic names its title directly; switch_bar steps ±1).
 * The close is the same path the open-title toggle takes. Returns
 * false only when the session/open allocation failed. */
static bool mn_bar_jump(fdk_widget *bar_w, int t) {
    fdk_menu_bar *b = bar_of(bar_w);
    if (b->session != NULL && b->session->active) {
        fdk_menu_session *s = b->session;
        b->session = NULL;
        session_close_above(s, 0);
    }
    fdk_menu_session *s = session_new(bar_w);
    if (s == NULL) {
        return false;
    }
    if (!fdk__menu_bar_open_index(bar_w, s, t)) {
        s->active = false;
        s->bar = NULL;
        return false;
    }
    return true;
}

/* The view's Alt-fallback body (defined here, after the bar
 * section, because it reads bar fields): the popup chain holds the
 * keyboard grab, so an Alt+letter naming ANOTHER title can only be
 * routed from the open view. Own title is skipped (already up). */
static bool mn_view_alt_bar_fallback(struct fdk_menu_view *v,
                                     const fdk_key_event *key) {
    fdk_widget *bar_w = v->session->bar;
    fdk_menu_bar *b = bar_of(bar_w);
    for (size_t t = 0; t < b->count; t++) {
        if (b->titles[t].menu == NULL ||
            b->titles[t].menu->count == 0 ||
            b->titles[t].menu == v->model) {
            continue; /* own title: already showing it */
        }
        if (mn_key_matches(key, b->titles[t].mnemonic)) {
            (void)mn_bar_jump(bar_w, (int)t);
            return true;
        }
    }
    return false;
}

static bool mn_open_scan(fdk_widget *w, const fdk_key_event *key,
                         int depth) {
    if (w == NULL || depth > MENU_ACCEL_MAX_DEPTH ||
        (w->flags & FDK_WF_DESTROYING) != 0) {
        return false;
    }
    if (w->klass == &fdk_menu_bar_class_def) {
        fdk_menu_bar *b = bar_of(w);
        for (size_t t = 0; t < b->count; t++) {
            if (b->titles[t].menu == NULL ||
                b->titles[t].menu->count == 0) {
                continue; /* nothing to open: not a mnemonic target */
            }
            if (mn_key_matches(key, b->titles[t].mnemonic)) {
                return mn_bar_jump(w, (int)t);
            }
        }
    }
    for (size_t c = 0; c < w->child_count; c++) {
        if (mn_open_scan(w->children[c], key, depth + 1)) {
            return true;
        }
    }
    return false;
}

bool fdk__menu_bar_mnemonic_open(fdk_widget *root,
                                 const fdk_key_event *key) {
    if (root == NULL || key == NULL) {
        return false;
    }
    return mn_open_scan(root, key, 0);
}

bool fdk__menu_bar_accel_hit(fdk_widget *root, const fdk_key_event *key,
                             fdk_menu_item **out_item) {
    if (out_item == NULL) {
        return false;
    }
    *out_item = NULL;
    if (root == NULL || key == NULL) {
        return false;
    }
    return accel_scan_widget(root, key, 0, out_item);
}

void fdk__menu_item_accel_activate(fdk_menu_item *item) {
    item = item_arg(item);
    if (item == NULL || !item->enabled ||
        item->kind == FDK_MIK_SEPARATOR) {
        return;
    }
    fdk_menu *m = item->owner;
    /* The session-less twin of view_activate's state flips: identical
     * check/radio semantics (radios uncheck their whole menu group),
     * identical callback order (per-item then model fallback). The
     * view machinery needs no repaint here — nothing is on screen;
     * if the model drives any on-screen state, the application's
     * callback invalidates it. After menu_fire returns, `item` and
     * `m` may be freed — nothing below touches them. */
    if (item->kind == FDK_MIK_CHECK) {
        item->checked = !item->checked;
    } else if (item->kind == FDK_MIK_RADIO && !item->checked) {
        for (size_t i = 0; i < m->count; i++) {
            if (m->items[i]->kind == FDK_MIK_RADIO) {
                m->items[i]->checked = (m->items[i] == item);
            }
        }
    }
    menu_fire(m, item);
}
