/*
 * log_internal.h — internal-only logging macros
 *
 * Not installed, not part of the public API. Every .c file in FDK
 * should define FDK_LOG_TAG to a short subsystem name before including
 * this header, e.g.:
 *
 *     #define FDK_LOG_TAG "core"
 *     #include "core/log_internal.h"
 */

#ifndef FDK_LOG_INTERNAL_H
#define FDK_LOG_INTERNAL_H

#include "fdk/fdk_log.h"

#ifndef FDK_LOG_TAG
#error "FDK_LOG_TAG must be #defined before including log_internal.h"
#endif

#define FDK_TRACE(...) fdk_log(FDK_LOG_TRACE, FDK_LOG_TAG, __VA_ARGS__)
#define FDK_DEBUG(...) fdk_log(FDK_LOG_DEBUG, FDK_LOG_TAG, __VA_ARGS__)
#define FDK_INFO(...)  fdk_log(FDK_LOG_INFO,  FDK_LOG_TAG, __VA_ARGS__)
#define FDK_WARN(...)  fdk_log(FDK_LOG_WARN,  FDK_LOG_TAG, __VA_ARGS__)
#define FDK_ERROR(...) fdk_log(FDK_LOG_ERROR, FDK_LOG_TAG, __VA_ARGS__)

/* Scoped minimum-level override (internal). Push a temporary level,
 * pop to restore; nesting to depth 8, deeper pushes are counted but
 * ignored (an honest degradation — the worst case is over-silencing
 * or over-chattiness inside a pathological nesting, never a crash
 * or a lost pop). The motivating user: theme discovery's internal-
 * name pass PROBES many .fdk files, and a file that fails to parse
 * mid-scan is a skip-with-warning, not an ERROR the user must see —
 * while the SAME file asked for BY NAME (the direct stem pass) must
 * keep its full loud diagnostics. The bracket scopes exactly that
 * difference without a level-getter racing a second thread. */
void fdk__log_level_push(fdk_log_level level);
void fdk__log_level_pop(void);

/* ---- startup trace (1.1.0-rc1, X1) --------------------------------------
 *
 * FDK_STARTUP_TRACE=1 logs one INFO line per startup phase with the
 * milliseconds elapsed since the first traced call — the field
 * diagnostic for "every FDK app starts slowly on my machine" (the
 * live report: X11 yes, Wayland no). Phases are traced at their
 * natural sites (backend connect legs, theme, first font scan, GL
 * context, self-check, first present); a phase's line reports BOTH
 * its own duration (when paired via fdk__startup_trace_begin) and
 * the total since t0. Without the env var the calls are two cheap
 * branches. */
void fdk__startup_trace_begin(const char *phase);
void fdk__startup_trace(const char *phase);
/* Test seam: forget t0 so a later suite re-traces from scratch. */
void fdk__startup_trace_reset(void);

#endif /* FDK_LOG_INTERNAL_H */
