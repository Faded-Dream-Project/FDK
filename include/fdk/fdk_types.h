/*
 * fdk_types.h — Faded Dream ToolKit core types
 *
 * Fundamental value types and opaque object handles shared across the
 * public API. No backend-specific (X11/Wayland) type ever appears here
 * or in any other public header — see docs/architecture.md, "Layering".
 */

#ifndef FDK_TYPES_H
#define FDK_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Fixed-width aliases used throughout the public API ---- */
typedef int32_t   fdk_i32;
typedef uint32_t  fdk_u32;
typedef int64_t   fdk_i64;
typedef uint64_t  fdk_u64;
typedef float      fdk_f32;
typedef double     fdk_f64;

/* ---- Geometry ---- */

typedef struct fdk_point {
    fdk_i32 x;
    fdk_i32 y;
} fdk_point;

typedef struct fdk_size {
    fdk_i32 width;
    fdk_i32 height;
} fdk_size;

typedef struct fdk_rect {
    fdk_i32 x;
    fdk_i32 y;
    fdk_i32 width;
    fdk_i32 height;
} fdk_rect;

/* Floating-point variants, used by the rendering layer where
 * high-DPI/fractional scaling requires sub-pixel precision. */
typedef struct fdk_pointf {
    fdk_f32 x;
    fdk_f32 y;
} fdk_pointf;

typedef struct fdk_sizef {
    fdk_f32 width;
    fdk_f32 height;
} fdk_sizef;

typedef struct fdk_rectf {
    fdk_f32 x;
    fdk_f32 y;
    fdk_f32 width;
    fdk_f32 height;
} fdk_rectf;

/* ---- Color ----
 * Straight (non-premultiplied) RGBA, components in [0.0, 1.0].
 * The renderer is responsible for premultiplying where its backend
 * requires it; the public API always deals in straight alpha. */
typedef struct fdk_color {
    fdk_f32 r;
    fdk_f32 g;
    fdk_f32 b;
    fdk_f32 a;
} fdk_color;

/* ---- Opaque object handles ----
 * All FDK objects are opaque pointers. Applications never see internal
 * struct layout — this is what makes ABI evolution possible pre-1.0
 * and keeps it stable post-1.0. See docs/abi-policy.md. */
typedef struct fdk_context   fdk_context;    /* global toolkit instance   */
typedef struct fdk_window    fdk_window;
typedef struct fdk_widget    fdk_widget;
typedef struct fdk_surface   fdk_surface;    /* renderable drawing target */
typedef struct fdk_theme     fdk_theme;
typedef struct fdk_event     fdk_event;
typedef struct fdk_timer     fdk_timer;

#ifdef __cplusplus
}
#endif

#endif /* FDK_TYPES_H */
