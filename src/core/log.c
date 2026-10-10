#define FDK_LOG_TAG "log"
#include "core/log_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static fdk_log_level g_min_level = FDK_LOG_INFO;
static fdk_log_sink_fn g_sink = NULL;
static void *g_sink_user_data = NULL;

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
