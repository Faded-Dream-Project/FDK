#include "fdk/fdk_error.h"

const char *fdk_result_to_string(fdk_result result) {
    switch (result) {
        case FDK_OK:                     return "ok";

        case FDK_ERR_UNKNOWN:            return "unknown error";
        case FDK_ERR_INVALID_ARGUMENT:   return "invalid argument";
        case FDK_ERR_OUT_OF_MEMORY:      return "out of memory";
        case FDK_ERR_NOT_INITIALIZED:    return "not initialized";
        case FDK_ERR_ALREADY_INITIALIZED:return "already initialized";
        case FDK_ERR_UNSUPPORTED:        return "unsupported";
        case FDK_ERR_NOT_FOUND:          return "not found";

        case FDK_ERR_PLATFORM_INIT:      return "platform initialization failed";
        case FDK_ERR_NO_DISPLAY:         return "no display connection available";
        case FDK_ERR_WINDOW_CREATE:      return "window creation failed";

        case FDK_ERR_RENDER_INIT:        return "renderer initialization failed";
        case FDK_ERR_SURFACE_CREATE:     return "surface creation failed";

        case FDK_ERR_THEME_PARSE:        return "theme parse error";
        case FDK_ERR_THEME_IO:           return "theme I/O error";
        case FDK_ERR_THEME_VERSION:      return "unsupported theme version";

        case FDK_ERR_IO:                 return "I/O error";
        case FDK_ERR_NOT_A_FILE:         return "not a file";

        default:                         return "unknown error";
    }
}
