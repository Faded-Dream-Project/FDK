/*
 * fdk_log.h — Faded Dream ToolKit logging
 *
 * A minimal leveled logger. FDK writes internal diagnostics through
 * this API rather than printf directly, and applications may install
 * their own sink to route FDK's log output into their own logging
 * system instead of stderr.
 *
 * Thread safety: fdk_log_set_level() and fdk_log_set_sink() are NOT
 * thread-safe against concurrent fdk_log_* calls from other threads.
 * Set them once at startup, on the main thread, before spawning any
 * worker threads that might log. Once configured, the logging calls
 * themselves are safe to call from any thread.
 */

#ifndef FDK_LOG_H
#define FDK_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fdk_log_level {
    FDK_LOG_TRACE = 0,
    FDK_LOG_DEBUG = 1,
    FDK_LOG_INFO  = 2,
    FDK_LOG_WARN  = 3,
    FDK_LOG_ERROR = 4,
    FDK_LOG_NONE  = 5   /* passed to fdk_log_set_level() to silence all output */
} fdk_log_level;

/* A log sink receives one already-formatted line (no trailing newline)
 * per call, along with its level and the subsystem tag that produced
 * it (e.g. "core", "x11", "theme"). `user_data` is whatever was passed
 * to fdk_log_set_sink(). The default sink (used if none is set) writes
 * to stderr with a timestamp, level, and tag prefix. */
typedef void (*fdk_log_sink_fn)(fdk_log_level level,
                                 const char *tag,
                                 const char *message,
                                 void *user_data);

/* Sets the minimum level that will be passed to the sink; anything
 * below it is dropped before formatting (so disabled trace/debug
 * logging costs effectively nothing at call sites, aside from the
 * varargs already being evaluated by the caller). Default: FDK_LOG_INFO. */
void fdk_log_set_level(fdk_log_level level);

/* Installs a custom sink. Pass sink_fn = NULL to restore the default
 * stderr sink. `user_data` is passed through unchanged on every call. */
void fdk_log_set_sink(fdk_log_sink_fn sink_fn, void *user_data);

/* Logs a single line. `tag` should be a short static subsystem name
 * (e.g. "core", "x11", "wayland", "theme", "layout"). `fmt` is a
 * printf-style format string. Internal FDK code normally goes through
 * the FDK_LOG_* macros below rather than calling this directly. */
void fdk_log(fdk_log_level level, const char *tag, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif /* FDK_LOG_H */
