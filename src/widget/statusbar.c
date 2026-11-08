/*
 * statusbar.c — the context-scoped message bar (1.4.2).
 *
 * GTK's GtkStatusbar: the strip along a window's bottom edge where
 * an application reports transient state ("Row 3 of 12", "Sorting…",
 * "Ln 42, Col 7"). The API is the classic STACK WITH CONTEXTS:
 *
 *   ctx = fdk_statusbar_get_context_id(sb, "cursor position");
 *   id = fdk_statusbar_push(sb, ctx, "Ln 42, Col 7");
 *   ... later ...
 *   fdk_statusbar_pop(sb, ctx);
 *
 * Every push returns a message handle; the bar paints the message on
 * TOP of the stack. pop(ctx) removes the topmost message OF THAT
 * context (a context pops what it pushed, in LIFO order, regardless
 * of what other contexts pushed in between); remove(ctx, id) removes
 * one exact message wherever it sits. The id is what makes
 * "I'll take that back" precise: a loader that pushed "Loading…"
 * removes exactly that message when it finishes, not whatever a
 * tooltip context happened to push meanwhile.
 *
 * Paint: a flat surface one hair off the window background
 * (SIDEBAR_BACKGROUND — the "quiet zone" color) with a 1-px top rule
 * (ENTRY_BORDER), the current message in the plain text ink,
 * 10 px in from the left. No grip, no frames-within-frames — the
 * modern face. The height is the text line + 8 (floor 24), so a
 * fontless statusbar still reads as a strip.
 *
 * An indicator, not a control: no focus, no events, max_children 0.
 * The a11y face is the STATUS_BAR role with the current message as
 * the name (NAME_CHANGED fires whenever the top of the stack flips).
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"

#include <string.h>

#define STATUSBAR_PAD_X 10
#define STATUSBAR_MIN_H 24

/* Context-name table growth is geometric like every array here. */
typedef struct fdk_statusbar_ctx {
    char *name;      /* owned; the minting key */
    fdk_u32 id;      /* 1..n, never reused                     */
} fdk_statusbar_ctx;

/* The context table lives on the instance (freed at destroy). The
 * struct in widgets_internal.h keeps the message stack; the context
 * table is private to this file, so it rides the SAME allocation via
 * a trailing member — no, simpler: a plain pointer, grown like the
 * message stack. Declared here because widgets_internal.h has no
 * reason to know it. */
typedef struct fdk_statusbar_priv {
    fdk_statusbar_ctx *ctxs;
    size_t ctx_count;
    size_t ctx_capacity;
} fdk_statusbar_priv;

/* The priv rides the instance: appended after the public struct via
 * a private subclass struct with the same single-allocation
 * discipline (base first). */
typedef struct fdk_statusbar_full {
    fdk_statusbar pub;
    fdk_statusbar_priv priv;
} fdk_statusbar_full;

static fdk_statusbar_priv *sb_priv(fdk_widget *w) {
    return &((fdk_statusbar_full *)(void *)w)->priv;
}

static fdk_i32 statusbar_height(const fdk_statusbar *sb) {
    if (sb->font == NULL) {
        return STATUSBAR_MIN_H;
    }
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(sb->font, "Ag", &tw, &th);
    fdk_i32 h = th + 8;
    return (h < STATUSBAR_MIN_H) ? STATUSBAR_MIN_H : h;
}

static void statusbar_measure(fdk_widget *w, fdk_size *out) {
    fdk_statusbar *sb = statusbar_of(w);
    out->height = statusbar_height(sb);
    /* Width: the widest message ever pushed is transient state the
     * layout has no business chasing (GTK parity: the bar is
     * window-width in practice). A sane floor keeps a fresh bar
     * visible in hand-computed layouts. */
    out->width = 80;
}

static void statusbar_paint(fdk_widget *w, fdk_surface *surface,
                            fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_statusbar *sb = statusbar_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* The quiet zone + its top rule. */
    fdk_surface_fill_rect(surface, bounds, fdk__pal_sidebar());
    fdk_rect rule = {bounds.x, bounds.y, bounds.width, 1};
    fdk_surface_fill_rect(surface, rule, fdk__pal_entry_border());

    if (sb->count == 0 || sb->font == NULL) {
        return;
    }
    const char *text = sb->msgs[sb->count - 1].text;
    if (text == NULL) {
        return;
    }
    fdk_i32 baseline = fdk__center_baseline(sb->font, bounds.y,
                                            bounds.height);
    fdk__draw_text(surface, sb->font, text, fdk__pal_text(),
                   bounds.x + STATUSBAR_PAD_X, baseline);
}

/* Fires whenever the top of the stack may have changed (push, pop,
 * remove). The a11y name IS the top message. */
static void statusbar_top_changed(fdk_widget *w) {
    fdk_widget_invalidate(w);
    fdk__a11y_notify(w, FDK_A11Y_NAME_CHANGED, 0);
}

static void statusbar_destroy(fdk_widget *w) {
    fdk_statusbar *sb = statusbar_of(w);
    for (size_t i = 0; i < sb->count; i++) {
        fdk_free(sb->msgs[i].text);
    }
    fdk_free(sb->msgs);
    fdk_statusbar_priv *p = sb_priv(w);
    for (size_t i = 0; i < p->ctx_count; i++) {
        fdk_free(p->ctxs[i].name);
    }
    fdk_free(p->ctxs);
}

/* ---- a11y ---- */

static void statusbar_a11y_describe(const fdk_widget *w,
                                    fdk_a11y_info *out) {
    const fdk_statusbar *sb = (const fdk_statusbar *)(const void *)w;
    if (sb->count > 0 && sb->msgs[sb->count - 1].text != NULL) {
        out->name = fdk__strdup(sb->msgs[sb->count - 1].text);
    }
}

static const fdk_a11y_class statusbar_a11y = {
    .role = FDK_A11Y_ROLE_STATUS_BAR,
    .describe = statusbar_a11y_describe,
    .actions = NULL, /* an indicator, not a control */
    .perform = NULL,
};

const fdk_widget_class fdk_statusbar_class_def = {
    .size = sizeof(fdk_statusbar_full),
    .name = "statusbar",
    .handle_event = NULL,
    .paint = statusbar_paint,
    .measure = statusbar_measure,
    .arrange = NULL,
    .destroy = statusbar_destroy,
    .a11y = &statusbar_a11y,
    .max_children = 0,
};

fdk_result fdk_statusbar_create(fdk_widget *parent, fdk_font *font,
                                fdk_widget **out_statusbar) {
    if (out_statusbar == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_statusbar_class_def,
                                     (fdk_rect){0, 0, 80,
                                                STATUSBAR_MIN_H},
                                     &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_statusbar *sb = statusbar_of(w);
    sb->font = font;
    sb->msgs = NULL;
    sb->count = 0;
    sb->capacity = 0;
    sb->next_id = 1;
    fdk_statusbar_priv *p = sb_priv(w);
    p->ctxs = NULL;
    p->ctx_count = 0;
    p->ctx_capacity = 0;
    *out_statusbar = w;
    return FDK_OK;
}

fdk_u32 fdk_statusbar_get_context_id(fdk_widget *statusbar,
                                     const char *context) {
    if (statusbar == NULL ||
        statusbar->klass != &fdk_statusbar_class_def ||
        context == NULL || context[0] == '\0') {
        return 0; /* the invalid id: push/remove refuse it */
    }
    fdk_statusbar_priv *p = sb_priv(statusbar);
    for (size_t i = 0; i < p->ctx_count; i++) {
        if (strcmp(p->ctxs[i].name, context) == 0) {
            return p->ctxs[i].id;
        }
    }
    /* Mint: sequential per-instance ids starting at 1 (0 reserved). */
    if (p->ctx_count == p->ctx_capacity) {
        size_t cap = (p->ctx_capacity == 0) ? 4 : p->ctx_capacity * 2;
        if (cap < p->ctx_capacity ||
            cap > SIZE_MAX / sizeof(*p->ctxs)) {
            return 0;
        }
        fdk_statusbar_ctx *grown =
            fdk_realloc(p->ctxs, cap * sizeof(*grown));
        if (grown == NULL) {
            return 0; /* OOM: the id is refused, not faked */
        }
        p->ctxs = grown;
        p->ctx_capacity = cap;
    }
    char *copy = fdk__strdup(context);
    if (copy == NULL) {
        return 0;
    }
    p->ctxs[p->ctx_count].name = copy;
    p->ctxs[p->ctx_count].id = (fdk_u32)(p->ctx_count + 1);
    p->ctx_count++;
    return p->ctxs[p->ctx_count - 1].id;
}

fdk_u32 fdk_statusbar_push(fdk_widget *statusbar, fdk_u32 context_id,
                           const char *text) {
    if (statusbar == NULL ||
        statusbar->klass != &fdk_statusbar_class_def ||
        context_id == 0 || text == NULL) {
        return 0;
    }
    fdk_statusbar *sb = statusbar_of(statusbar);
    if (context_id > (fdk_u32)sb_priv(statusbar)->ctx_count) {
        return 0; /* an id this bar never minted */
    }
    if (sb->count == sb->capacity) {
        size_t cap = (sb->capacity == 0) ? 8 : sb->capacity * 2;
        if (cap < sb->capacity || cap > SIZE_MAX / sizeof(*sb->msgs)) {
            return 0;
        }
        fdk_statusbar_msg *grown =
            fdk_realloc(sb->msgs, cap * sizeof(*grown));
        if (grown == NULL) {
            return 0;
        }
        sb->msgs = grown;
        sb->capacity = cap;
    }
    char *copy = fdk__strdup(text);
    if (copy == NULL) {
        return 0;
    }
    sb->msgs[sb->count].context = context_id;
    sb->msgs[sb->count].id = sb->next_id++;
    sb->msgs[sb->count].text = copy;
    sb->count++;
    statusbar_top_changed(statusbar);
    return sb->msgs[sb->count - 1].id;
}

void fdk_statusbar_pop(fdk_widget *statusbar, fdk_u32 context_id) {
    if (statusbar == NULL ||
        statusbar->klass != &fdk_statusbar_class_def ||
        context_id == 0) {
        return;
    }
    fdk_statusbar *sb = statusbar_of(statusbar);
    /* The TOPMOST message of this context — LIFO per context, the
     * GTK rule (pop what you pushed, others' messages survive). */
    for (size_t i = sb->count; i-- > 0;) {
        if (sb->msgs[i].context == context_id) {
            fdk_free(sb->msgs[i].text);
            memmove(&sb->msgs[i], &sb->msgs[i + 1],
                    (sb->count - i - 1) * sizeof(*sb->msgs));
            sb->count--;
            statusbar_top_changed(statusbar);
            return;
        }
    }
    /* Nothing of that context on the stack: a no-op pop (GTK parity
     * — popping an empty context is legal and silent). */
}

void fdk_statusbar_remove(fdk_widget *statusbar, fdk_u32 context_id,
                          fdk_u32 message_id) {
    if (statusbar == NULL ||
        statusbar->klass != &fdk_statusbar_class_def ||
        context_id == 0 || message_id == 0) {
        return;
    }
    fdk_statusbar *sb = statusbar_of(statusbar);
    for (size_t i = 0; i < sb->count; i++) {
        if (sb->msgs[i].context == context_id &&
            sb->msgs[i].id == message_id) {
            fdk_free(sb->msgs[i].text);
            memmove(&sb->msgs[i], &sb->msgs[i + 1],
                    (sb->count - i - 1) * sizeof(*sb->msgs));
            sb->count--;
            statusbar_top_changed(statusbar);
            return;
        }
    }
}

const char *fdk_statusbar_get_text(fdk_widget *statusbar) {
    if (statusbar == NULL ||
        statusbar->klass != &fdk_statusbar_class_def) {
        return NULL;
    }
    fdk_statusbar *sb = statusbar_of(statusbar);
    return (sb->count > 0) ? sb->msgs[sb->count - 1].text : NULL;
}
