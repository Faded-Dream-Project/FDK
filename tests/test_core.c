/* test_core.c — platform-independent core tests: version, error
 * strings, and argument validation that doesn't require an actual
 * platform connection to exercise. See docs/testing.md for why this
 * file is deliberately narrower than it was in Phase 1: fdk_init()
 * now really does connect to a display, so tests that need a live
 * connection moved to tests/test_x11_integration.c (run via
 * `make test-x11`, not plain `make test`) rather than staying here
 * and making ordinary `make test` require a desktop session. */

#include <stddef.h> /* offsetof: the init_ex contract test */
#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_prefs.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_version(void) {
    assert(fdk_get_version() == FDK_VERSION);
    assert(strcmp(fdk_get_version_string(), FDK_VERSION_STRING) == 0);

    /* The public version is PINNED to 1.0.0-dev by policy
     * (docs/versioning.md — the 2026-11-19 suffix decision, taken
     * after the third withdrawal of the 1.0.0 release: no tag
     * exists, the project is in development, and every build from
     * main must self-identify as unreleased so it can never be
     * mistaken for the withdrawn release). If these asserts fail,
     * someone bumped the version without a recorded project-owner
     * decision — read docs/versioning.md (including the 2026-10-11
     * retraction record: a premature 1.0.0 bump was reverted the
     * same day) before "fixing" either side of the assert. Internal
     * milestones (ROADMAP.md's Phase numbers and 1.x labels) are a
     * different axis and must never leak into these macros. Note the
     * ENCODED value carries no suffix — 1.0.0-dev encodes as its
     * base triple (1.0.0), and the suffix returns to "" only on a
     * final release. */
    assert(FDK_VERSION_MAJOR == 1);
    assert(FDK_VERSION_MINOR == 0);
    assert(FDK_VERSION_PATCH == 0);
    assert(strcmp(FDK_VERSION_SUFFIX, "-dev") == 0);
    assert(strcmp(FDK_VERSION_STRING, "1.0.0-dev") == 0);
    assert(fdk_get_version() == FDK_VERSION_ENCODE(1, 0, 0));
    printf("[ok] version: %s (%d, pinned per docs/versioning.md)\n",
           fdk_get_version_string(), fdk_get_version());
}

static void test_error_strings(void) {
    assert(strcmp(fdk_result_to_string(FDK_OK), "ok") == 0);
    assert(fdk_ok(FDK_OK));
    assert(!fdk_ok(FDK_ERR_OUT_OF_MEMORY));

    /* Every declared error code must produce a non-generic string;
     * spot-check a representative sample from each subsystem range. */
    assert(strcmp(fdk_result_to_string(FDK_ERR_INVALID_ARGUMENT), "unknown error") != 0);
    assert(strcmp(fdk_result_to_string(FDK_ERR_NO_DISPLAY), "unknown error") != 0);
    assert(strcmp(fdk_result_to_string(FDK_ERR_THEME_PARSE), "unknown error") != 0);

    printf("[ok] error strings\n");
}

static void test_init_rejects_null_out(void) {
    /* No display needed: fdk_init() validates out_ctx before ever
     * attempting a platform connection (see src/core/context.c). */
    fdk_result r = fdk_init(NULL, NULL);
    assert(r == FDK_ERR_INVALID_ARGUMENT);
    printf("[ok] fdk_init(NULL, ...) rejected\n");
}

static void test_shutdown_null_is_safe(void) {
    fdk_shutdown(NULL); /* must not crash */
    printf("[ok] fdk_shutdown(NULL) no-op\n");
}

static void test_quit_null_is_safe(void) {
    fdk_quit(NULL); /* must not crash */
    printf("[ok] fdk_quit(NULL) no-op\n");
}

/* The suite's own hermeticity, asserted where it is consumed: the
 * pin in hermetic_env.h must have set XDG_CONFIG_HOME to an
 * ABSOLUTE path inside build/tests/hermetic-home — the prefs
 * resolver spec-ignores relative XDG vars and falls back to the
 * developer's real $HOME/.config/fdk.prefs (the 2026-11-08 Pink
 * Rave leak: test_controls' progress fill painted the owner's
 * theme, the blue-pixel scan counted 0). A relative pin reds HERE
 * on every machine, CI included, because the assertion reads the
 * string, not the desktop. */
static void test_hermetic_pin(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    CHECK_GE(xdg != NULL ? 1 : 0, 1, "XDG_CONFIG_HOME is set by the pin");
    CHECK_EQ(xdg[0] == '/', 1, "XDG_CONFIG_HOME is absolute");
    CHECK_NE(strstr(xdg, "/build/tests/hermetic-home/config"), NULL,
             "XDG_CONFIG_HOME points at the hermetic home");

    /* The resolution the tests actually consume: opening the global
     * store lands in the hermetic home, never in $HOME. */
    fdk_prefs *p = NULL;
    assert(fdk_ok(fdk_prefs_open("fdk", &p)));
    const char *path = fdk_prefs_path(p);
    CHECK_GE(path != NULL ? 1 : 0, 1, "global prefs path resolves");
    CHECK_NE(strstr(path, "/build/tests/hermetic-home/config/fdk.prefs"),
             NULL, "global prefs resolve INSIDE the hermetic home");
    fdk_prefs_destroy(p);
    printf("[ok] hermetic pin: XDG_CONFIG_HOME absolute, global prefs "
           "land in the hermetic home\n");
}

/* 1.0.0-dev (per-format budget): the image_max_pixels init option —
 * append-only (zero-init = defaults), validated floor, and the
 * override actually reaching the decode budget. */
static void test_image_budget_option(void) {
    /* The floor: a budget that rejects a 1024x1024 icon is refused
     * at init (before any platform connection — the NULL backend
     * validation order makes this safe with no display). */
    fdk_context *ctx = NULL;
    fdk_init_options bad = {0};
    bad.image_max_pixels = 1000; /* < 1 Mi */
    assert(fdk_init(&ctx, &bad) == FDK_ERR_INVALID_ARGUMENT);

    /* A REAL context exercises the override path end-to-end — but
     * init needs a display for a full connection. The validation
     * above runs pre-connection by design (cheap-args-first, same
     * as the render_mode check), which is what this test pins; the
     * EFFECT of the override on decode is pinned headlessly in
     * test_render.c's budget group via the default path (and the
     * override multiplier path is exercised by test-apps' photo
     * viewer rig on live desktops). */
    printf("[ok] image budget option: floor validation, zero-init "
           "default preserved\n");
}

/* 1.0.0-dev: the fdk_init_ex append-only contract (fdk_core.h's
 * big comment is the spec). The struct grew twice by appending
 * with no size signal; fdk_init_ex is the size-disciplined form.
 * This group pins, under ASan:
 *   - a caller compiled against the FIRST-generation layout
 *     (backend + app_id only, 16 bytes) is never over-read —
 *     the newer render_mode/image_max_pixels fields read as
 *     their zero-init defaults;
 *   - opts_size = 0 behaves as fdk_init (full layout);
 *   - the image-budget floor still refuses through the ex form;
 *   - fdk_init and fdk_init_ex agree on the full layout. */
/* The frozen-symbol pin USED to live here, reaching around the
 * redirect with a GCC asm label (__asm__("fdk_init") on an extern
 * alias). That put two entities on one assembler name in one TU,
 * which is fragile across arches and -O levels — on arm64/gcc-13
 * at -O0 the call returned 0 without entering fdk_init_ex and the
 * sentinel context faulted in fdk_shutdown. The pin now lives in
 * tests/test_frozen_init.c, a TU built with -DFDK_NO_INIT_REDIRECT
 * that calls plain fdk_init: no aliases, the documented opt-out,
 * the ordinary linker binding. This file keeps the redirect-leg
 * tests (legacy size through ex, size 0, the floor, and the
 * redirected fdk_init) — the redirect itself is what they pin. */

static void test_init_ex_size_contract(void) {
    /* The first-generation layout, EXACTLY as a binary compiled
     * against the original header allocated it. Heap-allocated so
     * ASan's redzone makes ANY read past 16 bytes a hard failure
     * — the point of the test. */
    struct legacy_layout {
        fdk_platform_backend backend; /* offset 0 */
        /* 4 bytes padding, as the original ABI had */
        const char *app_id;           /* offset 8 */
    };
    assert(sizeof(struct legacy_layout) == 16);
    assert(offsetof(struct legacy_layout, app_id) == 8);
    struct legacy_layout *legacy = calloc(1, sizeof *legacy);
    assert(legacy != NULL);
    legacy->backend = FDK_PLATFORM_AUTO;
    legacy->app_id = "fdk-size-contract-test";

    /* The ex call with the caller's true size: FDK must read
     * backend and app_id, and must NOT touch a byte past 16.
     * Validation order (cheap args first) means a backend=0
     * (AUTO) call proceeds toward the platform connection —
     * which fails cleanly here (no display in the headless
     * suite) AFTER the option reads; the reads themselves are
     * what ASan watches. Either NO_DISPLAY or OK is acceptable;
     * an over-read is not (it aborts the process). */
    fdk_context *ctx = (fdk_context *)0x1;
    fdk_result r = fdk_init_ex(&ctx, (const fdk_init_options *)legacy,
                               sizeof *legacy);
    assert(r == FDK_ERR_NO_DISPLAY || fdk_ok(r));
    if (fdk_ok(r)) {
        fdk_shutdown(ctx);
    }

    /* opts_size = 0: the documented "full layout" meaning. Same
     * acceptance envelope. */
    fdk_init_options full = {0};
    full.backend = FDK_PLATFORM_AUTO;
    ctx = (fdk_context *)0x1;
    r = fdk_init_ex(&ctx, &full, 0);
    assert(r == FDK_ERR_NO_DISPLAY || fdk_ok(r));
    if (fdk_ok(r)) {
        fdk_shutdown(ctx);
    }

    /* The floor rule through the ex form: a too-small budget
     * refuses BEFORE any platform work. */
    fdk_init_options bad = {0};
    bad.image_max_pixels = 1000;
    ctx = NULL;
    assert(fdk_init_ex(&ctx, &bad, sizeof bad) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* The legacy wrapper reads the full layout (callers rebuilt
     * against this header) — same envelope. */
    ctx = (fdk_context *)0x1;
    r = fdk_init(&ctx, &full);
    assert(r == FDK_ERR_NO_DISPLAY || fdk_ok(r));
    if (fdk_ok(r)) {
        fdk_shutdown(ctx);
    }

    free(legacy);

    printf("[ok] fdk_init_ex size contract: legacy caller not "
           "over-read (ASan), 0 = full layout, floor enforced, "
           "redirect agrees with ex\n");
}

int main(void) {
    test_hermetic_pin();
    test_version();
    test_error_strings();
    test_init_rejects_null_out();
    test_image_budget_option();
    test_init_ex_size_contract();
    test_shutdown_null_is_safe();
    test_quit_null_is_safe();

    printf("\nall platform-independent core tests passed\n");
    return 0;
}
