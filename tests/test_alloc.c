/* test_alloc.c — internal allocator tests.
 *
 * Reaches into src/core directly since fdk_alloc/fdk_free are internal,
 * not public API. Built and run alongside the public-API tests to
 * exercise the same allocation path fdk_init() itself uses. */

#include "core/alloc_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_alloc_zeroed(void) {
    unsigned char *p = fdk_alloc(64);
    assert(p != NULL);
    for (int i = 0; i < 64; i++) {
        assert(p[i] == 0);
    }
    fdk_free(p);
    printf("[ok] fdk_alloc zero-initializes\n");
}

static void test_alloc_zero_size(void) {
    void *p = fdk_alloc(0);
    assert(p == NULL);
    printf("[ok] fdk_alloc(0) == NULL\n");
}

static void test_alloc_array_overflow_rejected(void) {
    /* SIZE_MAX / 2 * 3-ish: pick values that overflow size_t multiply. */
    void *p = fdk_alloc_array(SIZE_MAX, SIZE_MAX);
    assert(p == NULL);
    printf("[ok] fdk_alloc_array overflow rejected\n");
}

static void test_alloc_array_normal(void) {
    int *p = fdk_alloc_array(10, sizeof(int));
    assert(p != NULL);
    for (int i = 0; i < 10; i++) {
        assert(p[i] == 0);
        p[i] = i;
    }
    for (int i = 0; i < 10; i++) {
        assert(p[i] == i);
    }
    fdk_free(p);
    printf("[ok] fdk_alloc_array normal path\n");
}

static void test_realloc_grow_preserves_data(void) {
    char *p = fdk_alloc(4);
    assert(p != NULL);
    memcpy(p, "abc", 4);

    char *p2 = fdk_realloc(p, 16);
    assert(p2 != NULL);
    assert(memcmp(p2, "abc", 4) == 0);
    fdk_free(p2);
    printf("[ok] fdk_realloc grow preserves data\n");
}

static void test_realloc_to_zero_frees(void) {
    void *p = fdk_alloc(8);
    assert(p != NULL);
    void *p2 = fdk_realloc(p, 0);
    assert(p2 == NULL); /* p has been freed internally */
    printf("[ok] fdk_realloc(ptr, 0) frees and returns NULL\n");
}

static void test_free_null_is_safe(void) {
    fdk_free(NULL); /* must not crash */
    printf("[ok] fdk_free(NULL) no-op\n");
}

int main(void) {
    test_alloc_zeroed();
    test_alloc_zero_size();
    test_alloc_array_overflow_rejected();
    test_alloc_array_normal();
    test_realloc_grow_preserves_data();
    test_realloc_to_zero_frees();
    test_free_null_is_safe();

    printf("\nall alloc tests passed\n");
    return 0;
}
