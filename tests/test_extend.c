/* test_extend.c — extension API (1.1.0) headless tests.
 *
 * Platform-independent contract checks that need no display: the
 * API-version surface, and the argument-validation / NULL-safety
 * ladder of every fdk_ext_* entry point. The real surface machinery
 * (kinds, adoption, rendering, popups, fd sources firing) needs a
 * live backend and lives in tests/test_x11_integration.c's extension
 * section (run via `make test-x11`), exactly like every other
 * display-dependent subsystem — see docs/testing.md's split.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_ext_api_version(void) {
    /* The runtime number matches the compile-time one (a mismatch
     * here means the .so and the headers disagree — the exact bug
     * the add-on startup check exists to catch). */
    assert(fdk_ext_api_version() == FDK_EXT_API_VERSION);
    assert(fdk_ext_api_version() >= 1);
    printf("[ok] ext api version: %d == FDK_EXT_API_VERSION\n",
           fdk_ext_api_version());
}

static void test_argument_ladder(void) {
    /* Backend / capability queries. */
    assert(fdk_ext_active_backend(NULL) == FDK_EXT_BACKEND_NONE);
    assert(fdk_ext_has_capability(NULL, FDK_EXT_CAP_SHM_RENDER) == 0);
    assert(fdk_ext_has_capability(NULL, (fdk_ext_capability)999) == 0);

    /* Native display accessor. */
    fdk_ext_native_display nd;
    assert(fdk_ext_get_native_display(NULL, &nd) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Kind registration. */
    fdk_ext_surface_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    fdk_ext_surface_kind *kind = NULL;
    assert(fdk_ext_register_kind(NULL, "k", &ops, NULL, &kind) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_ext_kind_destroy(NULL) == FDK_OK); /* safe no-op */

    /* Surfaces. */
    fdk_ext_surface *surf = NULL;
    assert(fdk_ext_surface_create(NULL, &surf) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdk_ext_surface_destroy(NULL); /* safe no-op */
    assert(fdk_ext_surface_context(NULL) == NULL);
    fdk_ext_native_handle nh;
    assert(fdk_ext_surface_native(NULL, &nh) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdk_surface *render = NULL;
    assert(fdk_ext_surface_get_surface(NULL, &render) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdk_ext_surface_show(NULL);  /* safe no-ops */
    fdk_ext_surface_hide(NULL);
    fdk_size sz;
    assert(fdk_ext_surface_get_size(NULL, &sz) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_ext_surface_notify_configure(NULL, 10, 10) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdk_ext_surface_set_event_callback(NULL, NULL, NULL); /* no-op */

    /* Popups. */
    assert(fdk_ext_popup_create(NULL, 0, 0, 10, 10, &surf) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* fd sources. */
    assert(fdk_ext_loop_add_fd(NULL, 1, FDK_EXT_FD_IN, NULL, NULL) ==
           NULL);
    fdk_ext_loop_remove_fd(NULL); /* safe no-op */
    assert(fdk_ext_fd_source_fd(NULL) == -1);

    printf("[ok] ext argument/NULL ladder: every entry point\n");
}

int main(void) {
    test_ext_api_version();
    test_argument_ladder();
    printf("all extension headless tests passed\n");
    return 0;
}
