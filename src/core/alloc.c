#define FDK_LOG_TAG "core"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *fdk_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    void *ptr = calloc(1, size);
    if (ptr == NULL) {
        FDK_ERROR("allocation of %zu bytes failed", size);
    }
    return ptr;
}

void *fdk_alloc_array(size_t count, size_t elem_size) {
    if (count == 0 || elem_size == 0) {
        return NULL;
    }

    /* Overflow check: reject if count * elem_size would wrap. */
    if (count > (SIZE_MAX / elem_size)) {
        FDK_ERROR("array allocation overflow: %zu * %zu", count, elem_size);
        return NULL;
    }

    void *ptr = calloc(count, elem_size);
    if (ptr == NULL) {
        FDK_ERROR("array allocation of %zu * %zu bytes failed", count, elem_size);
    }
    return ptr;
}

void fdk_free(void *ptr) {
    free(ptr);
}

void *fdk_realloc(void *ptr, size_t new_size) {
    if (new_size == 0) {
        fdk_free(ptr);
        return NULL;
    }

    void *new_ptr = realloc(ptr, new_size);
    if (new_ptr == NULL) {
        FDK_ERROR("reallocation to %zu bytes failed", new_size);
    }
    return new_ptr;
}
