#define FDK_LOG_TAG "widgets"

/*
 * picture.c — the image display widget (1.4.9)
 *
 * The GtkPicture-shaped hole: one owned fdk_surface (decoded from
 * a file or taken over from the app), painted into the widget's
 * bounds under four content-fit policies:
 *
 *   NONE     — top-left, 1:1, clipped (the raw look)
 *   CONTAIN  — scaled to fit INSIDE, aspect kept, centered (the
 *              letterbox; empty bands show the widget background)
 *   COVER    — scaled to FILL, aspect kept, centered, overflow
 *              clipped (the crop; nothing empty, something hidden)
 *   FILL     — stretched to the bounds, aspect broken (the
 *              distortion policy — sometimes exactly what a
 *              texture slot wants)
 *
 * Natural size = the image's pixel size (the parent may assign
 * anything; the fit adapts). Scaled paints ride
 * fdk_surface_blit_transformed — integer scale-ups take its
 * nearest-neighbor fast path (no blur), everything else the
 * antialiased affine walk. A picture is NOT focusable and takes no
 * input (the a11y ROLE_IMAGE carries the app's accessible name —
 * the alt text — via fdk_widget_set_accessible_name).
 */

#include "widgets_internal.h"
#include "../render/surface_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct fdk_picture {
    fdk_widget base;
    fdk_surface *image;   /* owned; NULL = empty              */
    fdk_i32 img_w;
    fdk_i32 img_h;
    fdk_picture_fit fit;
} fdk_picture;

static fdk_picture *pic_of(fdk_widget *w) {
    return (fdk_picture *)(void *)w;
}

extern const fdk_widget_class fdk_picture_class_def;

/* ---- geometry ---- */

/* The destination rect the fit policy paints the image into. */
static fdk_rect pic_dest_rect(const fdk_picture *p, fdk_rect bounds) {
    if (p->image == NULL || p->img_w <= 0 || p->img_h <= 0 ||
        bounds.width <= 0 || bounds.height <= 0) {
        return (fdk_rect){0, 0, 0, 0};
    }
    switch (p->fit) {
    case FDK_PICTURE_FIT_NONE:
        return (fdk_rect){bounds.x, bounds.y,
                          (p->img_w < bounds.width) ? p->img_w
                                                    : bounds.width,
                          (p->img_h < bounds.height) ? p->img_h
                                                     : bounds.height};
    case FDK_PICTURE_FIT_FILL:
        return bounds;
    case FDK_PICTURE_FIT_CONTAIN:
    case FDK_PICTURE_FIT_COVER:
    default: {
        fdk_f32 sx = (fdk_f32)(fdk_i64)bounds.width /
                     (fdk_f32)(fdk_i64)p->img_w;
        fdk_f32 sy = (fdk_f32)(fdk_i64)bounds.height /
                     (fdk_f32)(fdk_i64)p->img_h;
        fdk_f32 s = (p->fit == FDK_PICTURE_FIT_CONTAIN)
                        ? (sx < sy ? sx : sy)
                        : (sx > sy ? sx : sy);
        fdk_i32 dw = (fdk_i32)((fdk_f32)(fdk_i64)p->img_w * s + 0.5f);
        fdk_i32 dh = (fdk_i32)((fdk_f32)(fdk_i64)p->img_h * s + 0.5f);
        /* CENTER on the overflowing axis too (COVER crops evenly).*/
        fdk_i32 dx = bounds.x + (bounds.width - dw) / 2;
        fdk_i32 dy = bounds.y + (bounds.height - dh) / 2;
        return (fdk_rect){dx, dy, dw, dh};
    }
    }
}

static void pic_paint(fdk_widget *w, fdk_surface *surface,
                      fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_picture *p = pic_of(w);
    if (p->image == NULL) {
        return;
    }
    fdk_rect dest = pic_dest_rect(p, bounds);
    if (dest.width <= 0 || dest.height <= 0) {
        return;
    }
    if (p->fit == FDK_PICTURE_FIT_NONE) {
        /* 1:1 top-left: the plain blit (the clip stack trims). */
        (void)fdk_surface_blit_blend(surface, dest.x, dest.y,
                                     p->image,
                                     (fdk_rect){0, 0, p->img_w,
                                                p->img_h});
        return;
    }
    /* Scaled: the affine blit with scale+translate. Integer
     * scale-ups take the engine's nearest-neighbor fast path; the
     * rest walk the antialiased sampler. */
    fdk_f32 sx = (fdk_f32)dest.width / (fdk_f32)p->img_w;
    fdk_f32 sy = (fdk_f32)dest.height / (fdk_f32)p->img_h;
    fdk_matrix m = fdk_matrix_scale_xy(sx, sy);
    m.tx = (fdk_f32)dest.x;
    m.ty = (fdk_f32)dest.y;
    (void)fdk_surface_blit_transformed(surface, m, p->image);
}

static void pic_measure(fdk_widget *w, fdk_size *out) {
    fdk_picture *p = pic_of(w);
    out->width = p->img_w;
    out->height = p->img_h;
}

static void pic_destroy(fdk_widget *w) {
    fdk_picture *p = pic_of(w);
    if (p->image != NULL) {
        fdk_surface_destroy(p->image);
        p->image = NULL;
    }
}

/* a11y: the IMAGE role; the accessible NAME (set by the app) is the
 * alt text, the value text reports the pixel size. */
static void pic_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_picture *p = (const fdk_picture *)(const void *)w;
    if (p->image == NULL) {
        return;
    }
    char buf[48];
    (void)snprintf(buf, sizeof(buf), "%d x %d pixels", p->img_w,
                   p->img_h);
    out->value_text = fdk__strdup(buf);
    out->has_value = true;
}

static const fdk_a11y_class pic_a11y = {
    .role = FDK_A11Y_ROLE_IMAGE,
    .describe = pic_a11y_describe,
};

const fdk_widget_class fdk_picture_class_def = {
    .size = sizeof(fdk_picture),
    .name = "picture",
    .handle_event = NULL, /* not interactive */
    .paint = pic_paint,
    .measure = pic_measure,
    .arrange = NULL,
    .destroy = pic_destroy,
    .a11y = &pic_a11y,
};

/* ---- public API ---- */

fdk_result fdk_picture_create(fdk_widget *parent,
                              fdk_widget **out_picture) {
    if (out_picture == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_picture_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_picture *p = pic_of(w);
    p->image = NULL;
    p->img_w = 0;
    p->img_h = 0;
    p->fit = FDK_PICTURE_FIT_CONTAIN;
    fdk_widget_child_layout_changed(w->parent);
    *out_picture = w;
    return FDK_OK;
}

/* The shared content-replace core. */
static void pic_set_image(fdk_widget *w, fdk_surface *image,
                          fdk_i32 iw, fdk_i32 ih) {
    fdk_picture *p = pic_of(w);
    if (p->image != NULL) {
        fdk_surface_destroy(p->image);
    }
    p->image = image;
    p->img_w = iw;
    p->img_h = ih;
    fdk_widget_child_layout_changed(w->parent);
    fdk_widget_invalidate(w);
    fdk__a11y_notify(w, FDK_A11Y_VALUE_CHANGED, 0);
}

fdk_result fdk_picture_set_from_file(fdk_widget *picture,
                                     const char *path) {
    if (picture == NULL || picture->klass != &fdk_picture_class_def ||
        path == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_surface *image = NULL;
    fdk_result r = fdk_surface_create_from_image(path, &image);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(image, &info))) {
        fdk_surface_destroy(image);
        return FDK_ERR_PLATFORM;
    }
    pic_set_image(picture, image, info.width, info.height);
    return FDK_OK;
}

void fdk_picture_set_surface(fdk_widget *picture, fdk_surface *surface) {
    if (picture == NULL || picture->klass != &fdk_picture_class_def) {
        /* Refuse politely: a surface we will not own must not leak
         * silently — destroy it here so the transfer contract stays
         * single-owner even on a type mismatch. */
        if (surface != NULL) {
            fdk_surface_destroy(surface);
        }
        return;
    }
    if (surface == NULL) {
        pic_set_image(picture, NULL, 0, 0);
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surface, &info))) {
        fdk_surface_destroy(surface);
        return;
    }
    pic_set_image(picture, surface, info.width, info.height);
}

void fdk_picture_set_fit(fdk_widget *picture, fdk_picture_fit fit) {
    if (picture == NULL || picture->klass != &fdk_picture_class_def) {
        return;
    }
    fdk_picture *p = pic_of(picture);
    if (p->fit == fit) {
        return;
    }
    p->fit = fit;
    fdk_widget_invalidate(picture);
}

fdk_picture_fit fdk_picture_get_fit(fdk_widget *picture) {
    if (picture == NULL || picture->klass != &fdk_picture_class_def) {
        return FDK_PICTURE_FIT_CONTAIN;
    }
    return pic_of(picture)->fit;
}

fdk_i32 fdk_picture_image_width(fdk_widget *picture) {
    if (picture == NULL || picture->klass != &fdk_picture_class_def) {
        return 0;
    }
    return pic_of(picture)->img_w;
}

fdk_i32 fdk_picture_image_height(fdk_widget *picture) {
    if (picture == NULL || picture->klass != &fdk_picture_class_def) {
        return 0;
    }
    return pic_of(picture)->img_h;
}
