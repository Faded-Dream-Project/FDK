#include "fdk/fdk_version.h"

int fdk_get_version(void) {
    return FDK_VERSION;
}

const char *fdk_get_version_string(void) {
    return FDK_VERSION_STRING;
}
