/* window_gl.c — the public fdk_window_gl_* / fdk_gl_status routing
 * (include/fdk/fdk_gl.h). Thin by design: every real answer lives in
 * the backends' ops (window_gl_engage/status/info, appended 1.0.0);
 * this file owns argument validation, the reason-code mapping, and
 * the honest zero/NULL contracts the header documents. */

#define FDK_LOG_TAG "window"
#include "window_internal.h"
#include "fdk/fdk_gl.h"
#include "core/log_internal.h"
#include <string.h>

/* The platform-internal mirror's values MUST equal the public enum's
 * (platform_internal.h's comment pins why the mirror exists). This
 * translation table is the single place the two meet; the test pins
 * it (tests/test_gl_viewport.c includes both worlds). */
static fdk_gl_reason gl_reason_public(int platform_reason) {
    switch (platform_reason) {
    case FDK_PLATFORM_GL_REASON_OK: return FDK_GL_REASON_OK;
    case FDK_PLATFORM_GL_REASON_NOT_YET: return FDK_GL_REASON_NOT_YET;
    case FDK_PLATFORM_GL_REASON_SIZE_GATE:
        return FDK_GL_REASON_SIZE_GATE;
    case FDK_PLATFORM_GL_REASON_CPU_MODE: return FDK_GL_REASON_CPU_MODE;
    case FDK_PLATFORM_GL_REASON_DRIVER: return FDK_GL_REASON_DRIVER;
    case FDK_PLATFORM_GL_REASON_DOWNGRADED:
        return FDK_GL_REASON_DOWNGRADED;
    default: return FDK_GL_REASON_BACKEND;
    }
}

fdk_result fdk_window_gl_engage(fdk_window *window) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ops == NULL || window->ops->window_gl_engage == NULL) {
        FDK_INFO("fdk_window_gl_engage: backend has no GL lane — "
                 "engage refused (FDK_GL_REASON_BACKEND)");
        return FDK_ERR_UNSUPPORTED;
    }
    int reason = FDK_PLATFORM_GL_REASON_BACKEND;
    return window->ops->window_gl_engage(window->pwindow, &reason);
}

fdk_result fdk_gl_status(fdk_window *window,
                         fdk_gl_status_data *out_status) {
    if (out_status == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->reason = FDK_GL_REASON_BACKEND;
    if (window == NULL) {
        return FDK_OK; /* the documented NULL-window contract */
    }
    if (window->ops == NULL || window->ops->window_gl_status == NULL) {
        return FDK_OK;
    }
    int reason = FDK_PLATFORM_GL_REASON_BACKEND;
    int aux[5] = { 0, 0, 0, 0, 0 };
    window->ops->window_gl_status(window->pwindow, &reason, aux);
    out_status->reason = gl_reason_public(reason);
    out_status->pixel_area =
        (fdk_i64)((fdk_u64)(unsigned)aux[1] << 32 |
                  (fdk_u64)(unsigned)aux[0]);
    out_status->size_gate = (fdk_i64)aux[2];
    out_status->force_override = aux[3];
    out_status->cpu_mode = aux[4];
    return FDK_OK;
}

fdk_result fdk_window_gl_info(fdk_window *window, fdk_gl_info *out_info) {
    if (out_info == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->flavor[0] = 'n';
    out_info->flavor[1] = 'o';
    out_info->flavor[2] = 'n';
    out_info->flavor[3] = 'e';
    out_info->flavor[4] = '\0';
    out_info->vendor = "<unknown>";
    out_info->renderer = "<unknown>";
    out_info->version = "<unknown>";
    if (window == NULL || window->ops == NULL ||
        window->ops->window_gl_info == NULL) {
        return FDK_OK;
    }
    window->ops->window_gl_info(window->pwindow,
                                &out_info->version_major,
                                &out_info->version_minor,
                                out_info->flavor, &out_info->vendor,
                                &out_info->renderer, &out_info->version);
    return FDK_OK;
}
