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

#endif /* FDK_LOG_INTERNAL_H */
