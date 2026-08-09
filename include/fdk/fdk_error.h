/*
 * fdk_error.h — Faded Dream ToolKit error handling
 *
 * FDK does not use a global errno-style variable and does not throw
 * exceptions (this is C). Fallible functions return an fdk_result code
 * directly. Functions that also need to return a value do so through
 * an output pointer parameter, documented per-function in their header.
 *
 * Every function in the public API that can fail documents so in its
 * own doc comment, including which fdk_result codes it can produce.
 */

#ifndef FDK_ERROR_H
#define FDK_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fdk_result {
    FDK_OK = 0,

    /* Generic */
    FDK_ERR_UNKNOWN            = -1,
    FDK_ERR_INVALID_ARGUMENT   = -2,
    FDK_ERR_OUT_OF_MEMORY      = -3,
    FDK_ERR_NOT_INITIALIZED    = -4,
    FDK_ERR_ALREADY_INITIALIZED= -5,
    FDK_ERR_UNSUPPORTED        = -6,   /* valid request, backend can't do it */
    FDK_ERR_NOT_FOUND          = -7,

    /* Platform / windowing */
    FDK_ERR_PLATFORM_INIT      = -100,
    FDK_ERR_NO_DISPLAY         = -101, /* no X11/Wayland connection available */
    FDK_ERR_WINDOW_CREATE      = -102,

    /* Rendering */
    FDK_ERR_RENDER_INIT        = -200,
    FDK_ERR_SURFACE_CREATE     = -201,

    /* Theme / parsing */
    FDK_ERR_THEME_PARSE        = -300,
    FDK_ERR_THEME_IO           = -301,
    FDK_ERR_THEME_VERSION      = -302, /* .fdk file version unsupported */

    /* I/O */
    FDK_ERR_IO                 = -400,
    FDK_ERR_NOT_A_FILE         = -401,
} fdk_result;

/* Returns a short, static, human-readable, English description of a
 * result code, e.g. "out of memory". Never returns NULL — unrecognized
 * codes yield "unknown error". Intended for logs and diagnostics, not
 * for parsing or for direct display as localized UI text. */
const char *fdk_result_to_string(fdk_result result);

/* Convenience: nonzero (true) if `result` represents success. Equivalent
 * to `result == FDK_OK`, provided so call sites can write
 * `if (!fdk_ok(r))` without repeating the enum comparison. */
static inline int fdk_ok(fdk_result result) {
    return result == FDK_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* FDK_ERROR_H */
