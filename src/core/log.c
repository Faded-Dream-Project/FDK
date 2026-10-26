#define FDK_LOG_TAG "log"
#include "core/log_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static fdk_log_level g_min_level = FDK_LOG_INFO;
static fdk_log_sink_fn g_sink = NULL;
static void *g_sink_user_data = NULL;
static fdk_log_level initial_level(void);

/* FDK_LOG_LEVEL (1.1.0-rc1): the environment knob for field debugging
 * — the level a live-desktop report wants before asking the user to
 * recompile anything. Read ONCE at first use (a mid-session change
 * would race the push/pop stack's saved levels); the explicit
 * fdk_log_set_level() call still wins when the app makes one.
 * Accepted: trace|debug|info|warn|error|none (case-insensitive,
 * first letter sufficient). */
static fdk_log_level env_level(void) {
    const char *v = getenv("FDK_LOG_LEVEL");
    if (v == NULL || v[0] == '\0') {
        return FDK_LOG_INFO;
    }
    switch (v[0] | 0x20) {
        case 't': return FDK_LOG_TRACE;
        case 'd': return FDK_LOG_DEBUG;
        case 'i': return FDK_LOG_INFO;
        case 'w': return FDK_LOG_WARN;
        case 'e': return FDK_LOG_ERROR;
        case 'n': return FDK_LOG_NONE;
        default:  return FDK_LOG_INFO;
    }
}

static fdk_log_level initial_level(void) {
    static int done = 0;
    static fdk_log_level from_env = FDK_LOG_INFO;
    if (!done) {
        done = 1;
        from_env = env_level();
    }
    return from_env;
}

/* Seeds the level before any log call or push/pop can observe it
 * (both would otherwise save/restore the pre-seed INFO default). Runs
 * at library load — before fdk_init, before any app main() code. */
__attribute__((constructor)) static void log_level_seed(void) {
    g_min_level = initial_level();
}

/* The push/pop level stack (log_internal.h). Depth-bounded; the
 * counter keeps counting past the bound so every pop has a push and
 * the pairs never desynchronize. */
#define LOG_LEVEL_STACK_MAX 8
static fdk_log_level g_level_stack[LOG_LEVEL_STACK_MAX];
static int g_level_stack_depth;

void fdk__log_level_push(fdk_log_level level) {
    if (g_level_stack_depth < LOG_LEVEL_STACK_MAX) {
        g_level_stack[g_level_stack_depth] = g_min_level;
    }
    g_level_stack_depth++;
    g_min_level = level;
}

void fdk__log_level_pop(void) {
    if (g_level_stack_depth == 0) {
        return; /* a pop without a push: ignored, never a crash */
    }
    g_level_stack_depth--;
    if (g_level_stack_depth < LOG_LEVEL_STACK_MAX) {
        g_min_level = g_level_stack[g_level_stack_depth];
    }
}

static const char *level_name(fdk_log_level level) {
    switch (level) {
        case FDK_LOG_TRACE: return "TRACE";
        case FDK_LOG_DEBUG: return "DEBUG";
        case FDK_LOG_INFO:  return "INFO";
        case FDK_LOG_WARN:  return "WARN";
        case FDK_LOG_ERROR: return "ERROR";
        default:            return "?";
    }
}

static void default_sink(fdk_log_level level, const char *tag,
                          const char *message, void *user_data) {
    (void)user_data;

    char timebuf[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_now);

    fprintf(stderr, "%s [%s] (%s) %s\n", timebuf, level_name(level), tag, message);
}

void fdk_log_set_level(fdk_log_level level) {
    g_min_level = level;
}

/* ---- startup trace (1.1.0-rc1, X1) --------------------------------------- */

static long long startup_t0_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

/* State: t0 + the running phase's begin time. Single-threaded UI
 * startup, no locking. The env var is read once (first use); a NULL
 * t0 with tracing enabled means "this call starts the clock". */
static int startup_trace_on = -1; /* -1 = not yet read */
static long long startup_t0 = -1;
static long long startup_phase_begin = -1;
static char startup_phase[64];

static int startup_trace_enabled(void) {
    if (startup_trace_on < 0) {
        const char *v = getenv("FDK_STARTUP_TRACE");
        startup_trace_on = (v != NULL && v[0] != '\0') ? 1 : 0;
    }
    return startup_trace_on;
}

void fdk__startup_trace_begin(const char *phase) {
    if (!startup_trace_enabled()) {
        return;
    }
    if (startup_t0 < 0) {
        startup_t0 = startup_t0_ms();
    }
    startup_phase_begin = startup_t0_ms();
    snprintf(startup_phase, sizeof(startup_phase), "%s", phase);
}

void fdk__startup_trace(const char *phase) {
    if (!startup_trace_enabled()) {
        return;
    }
    long long now = startup_t0_ms();
    if (startup_t0 < 0) {
        startup_t0 = now; /* a lone end without begin: it IS the begin */
    }
    long long total = now - startup_t0;
    long long own = (startup_phase_begin >= 0 &&
                   strcmp(startup_phase, phase) == 0)
                      ? now - startup_phase_begin
                      : -1;
    if (own >= 0) {
        FDK_INFO("[startup] %s: %lld ms (total %lld ms)",
                 phase, (long long)own, (long long)total);
    } else {
        FDK_INFO("[startup] %s (total %lld ms)",
                 phase, (long long)total);
    }
    startup_phase_begin = now;
    snprintf(startup_phase, sizeof(startup_phase), "%s", phase);
}

void fdk__startup_trace_reset(void) {
    startup_t0 = -1;
    startup_phase_begin = -1;
    startup_phase[0] = '\0';
}

void fdk_log_set_sink(fdk_log_sink_fn sink_fn, void *user_data) {
    g_sink = sink_fn;
    g_sink_user_data = user_data;
}

void fdk_log(fdk_log_level level, const char *tag, const char *fmt, ...) {
    if (level < g_min_level) {
        return;
    }
    if (tag == NULL) {
        tag = "fdk";
    }

    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (g_sink != NULL) {
        g_sink(level, tag, message, g_sink_user_data);
    } else {
        default_sink(level, tag, message, NULL);
    }
}
