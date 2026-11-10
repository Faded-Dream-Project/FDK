/* test_matrix.c — P4: the capability × backend × API test matrix.
 *
 * Asserts, per lane:
 *   - every capability flag (1-18) answers the platform truth,
 *   - every UNSUPPORTED path answers UNSUPPORTED (or NULL) exactly
 *     where the driver/compositor lacks the feature,
 *   - the unstable-tier rule: no Wayland unstable-protocol type name
 *     appears in any PUBLIC header (the raw-event surface exposes
 *     void* only — the bindings stay internal by policy),
 *   - the render-mode caps follow the context's resolved mode.
 */
#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#ifndef BUILD_DIR_STR
#define BUILD_DIR_STR "build"
#endif
#ifndef NL
#define NL "\n"
#endif
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond, name)                                                    \
    do {                                                                     \
        printf("[%s] %s\n", (cond) ? "ok" : "FAIL", name);                   \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

int main(int argc, char **argv) {
    const char *lane = argc > 1 ? argv[1] : "x11";
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("SKIP: no FDK context\n");
        return 77;
    }
    int is_wl = strcmp(lane, "wayland") == 0;

    /* ---- the caps that exist on BOTH lanes ---------------------------- */
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_SHM_RENDER),
          "SHM_RENDER (both backends)");
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_POPUP_GRAB),
          "POPUP_GRAB (both backends)");
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_CPU),
          "RENDER_CPU");
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_FRAME_PACING),
          "FRAME_PACING");

    /* ---- the lane-exclusive caps -------------------------------------- */
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_EWMH_DOCK) != is_wl,
          "EWMH_DOCK is X11-only truth");
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_GL) ==
              fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_GPU),
          "GL follows the lane's truth");
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_WL_RAW_EVENTS) == is_wl,
          "WL_RAW_EVENTS (17) is Wayland-only");
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_XDG_ACTIVATION),
          "XDG_ACTIVATION (11): both backends mint tokens");
    CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_X11_EVENT_FILTER) ==
              (!is_wl),
          "X11_EVENT_FILTER (18) is X11-only");

    /* ---- the compositor-negotiated P3 caps ----------------------------- */
    if (is_wl) {
        /* sway ships all seven; the cap says so */
        CHECK(fdk_ext_has_capability(ctx, FDK_EXT_CAP_IDLE_INHIBIT) &&
                  fdk_ext_has_capability(ctx, FDK_EXT_CAP_XDG_ACTIVATION) &&
                  fdk_ext_has_capability(ctx,
                                         FDK_EXT_CAP_FOREIGN_TOPLEVEL) &&
                  fdk_ext_has_capability(ctx,
                                         FDK_EXT_CAP_TEXT_INPUT_V3) &&
                  fdk_ext_has_capability(ctx, FDK_EXT_CAP_SESSION_LOCK) &&
                  fdk_ext_has_capability(ctx,
                                         FDK_EXT_CAP_INPUT_METHOD_V2) &&
                  fdk_ext_has_capability(ctx,
                                         FDK_EXT_CAP_VIRTUAL_KEYBOARD),
              "the seven P3 caps under sway");
    } else {
        CHECK(!fdk_ext_has_capability(ctx, FDK_EXT_CAP_IDLE_INHIBIT) &&
                  !fdk_ext_has_capability(ctx,
                                          FDK_EXT_CAP_FOREIGN_TOPLEVEL) &&
                  !fdk_ext_has_capability(ctx,
                                          FDK_EXT_CAP_TEXT_INPUT_V3) &&
                  !fdk_ext_has_capability(ctx, FDK_EXT_CAP_SESSION_LOCK) &&
                  !fdk_ext_has_capability(ctx,
                                          FDK_EXT_CAP_INPUT_METHOD_V2) &&
                  !fdk_ext_has_capability(ctx,
                                          FDK_EXT_CAP_VIRTUAL_KEYBOARD),
              "the six P3 caps honestly false on X11");
    }

    /* ---- the UNSUPPORTED paths ----------------------------------------- */
    if (is_wl) {
        CHECK(fdk_x11_set_event_filter(ctx, NULL, NULL) ==
                  FDK_ERR_UNSUPPORTED,
              "x11 event filter: UNSUPPORTED on Wayland");
        /* the UNSUPPORTED path for session-lock cannot fire under
         * sway (the manager exists); the P3 protocol test covers the
         * mint/verdict/unlock flow instead */
    } else {
        fdk_window *w = NULL;
        fdk_window_options o;
        memset(&o, 0, sizeof(o));
        o.render_mode = FDK_RENDER_CPU;
        o.width = 100;
        o.height = 60;
        o.title = "fdk-matrix";
        if (fdk_ok(fdk_window_create(ctx, &o, &w))) {
            CHECK(fdk_window_set_idle_inhibited(w, 1) ==
                      FDK_ERR_UNSUPPORTED,
                  "idle-inhibit: UNSUPPORTED on X11");
            /* X11 mints the SN-id token (the same contract, the
             * older mechanism) — it is SUPPORTED, not unsupported */
            char tok[128];
            memset(tok, 0, sizeof tok);
            CHECK(fdk_ok(fdk_window_activation_token(w, tok,
                                                     sizeof tok)) &&
                      tok[0] != '\0',
                  "activation token: the SN id mints on X11");
            fdk_window_destroy(w);
        }
    }
    /* the Wayland lane REALLY creates it — take the handle and
     * destroy it (the LeakSanitizer catch, 2026-11-11: the discarded
     * queue leaked its struct + the wrapped-display registry proxies) */
    {
        void *q = fdk_wl_queue_create(ctx, "x");
        CHECK(q != NULL || !is_wl,
              "wl queue create: non-NULL on Wayland, NULL off");
        if (q != NULL) {
            fdk_wl_queue_destroy(q);
        }
    }
    CHECK(fdk_wl_raw_bind(NULL, 0, NULL, 0) == NULL,
          "raw bind: NULL handles");

    /* ---- the D2 payload ceiling (the runtime echo of the static
     * assert in src/core/abi_check.c; a build that somehow skipped
     * that TU still fails the suite here) ------------------------------ */
    CHECK(sizeof(fdk_event_data) <= 128,
          "event payload ceiling (D2): sizeof(fdk_event_data) <= 128");
    CHECK(sizeof(fdk_event_data) >= sizeof(fdk_configure_event),
          "the union still holds the biggest payload");

    /* ---- the render-mode caps follow the resolved mode ------------------ */
    CHECK((fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_HYBRID) ||
           fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_GPU)),
          "HYBRID or GPU resolves (never both, never neither)");
    CHECK(!(fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_HYBRID) &&
            fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_GPU)),
          "HYBRID and GPU are exclusive");

    /* ---- the unstable-tier rule (D5: the BUILD gate) -------------------- */
    /* The gate is tools/header_gate.c, run by `make all` — a banned
     * declaration fails the BUILD, not merely the suite. This test
     * proves the gate itself: (1) the tool passes the real public
     * headers, and (2) the SAME tool rejects a synthetic bad header
     * (a zwlr_ declaration in code), while prose mentions in comments
     * and strings stay legal. */
    {
        const char *gate = getenv("FDK_HEADER_GATE");
        char gbuf[512];
        if (gate == NULL) {
            snprintf(gbuf, sizeof gbuf, "%s/header_gate", BUILD_DIR_STR);
            gate = gbuf;
        }
        if (access(gate, X_OK) != 0) {
            /* Self-build: `make all` runs the gate, but this test
             * must also pass from a narrow target invocation (the
             * sway step builds only named targets — the CI catch,
             * 2026-11-12). */
            (void)system("cc -O2 tools/header_gate.c -o "
                         BUILD_DIR_STR "/header_gate");
        }
        if (access(gate, X_OK) == 0) {
            int rc = 0;
            (void)system("mkdir -p " BUILD_DIR_STR "/tests/data");
            /* (1) the real headers: the gate's own `make all` run is
             * the enforcement; here assert the tool agrees. */
            rc = system("build/header_gate include/fdk/*.h "
                        "> /dev/null 2>&1");
            CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0,
                  "header gate: all real public headers clean");

            /* (2) the synthetic violation: a DECLARATION must fail. */
            const char *bad_path = BUILD_DIR_STR
                "/tests/data/bad_gate_header.h";
            FILE *bf = fopen(bad_path, "w");
            if (bf == NULL) {
                CHECK(0, "cannot write the bad-header fixture");
            } else {
                fputs("#include \"fdk/fdk.h\"" NL
                      "struct zwlr_layer_shell_v1 *bad_idea;" NL, bf);
                fclose(bf);
                char cmd[1024];
                snprintf(cmd, sizeof cmd, "%s %s > /dev/null 2>&1",
                         gate, bad_path);
                rc = system(cmd);
                CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) != 0,
                      "header gate: a banned DECLARATION fails");
            }

            /* (3) the legal shapes: prose in a comment, a token inside
             * a string — the stripping must keep them legal. */
            const char *ok_path = BUILD_DIR_STR
                "/tests/data/prose_gate_header.h";
            FILE *of = fopen(ok_path, "w");
            if (of == NULL) {
                CHECK(0, "cannot write the prose-header fixture");
            } else {
                fputs("/* prose mention: zwlr_ in a comment is legal */" NL
                      "static const char *s = "
                      "\"zwlr_ in a string too\";" NL, of);
                fclose(of);
                char cmd[1024];
                snprintf(cmd, sizeof cmd, "%s %s > /dev/null 2>&1",
                         gate, ok_path);
                rc = system(cmd);
                CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0,
                      "header gate: prose mentions stay legal");
            }
        }
    }

    fdk_shutdown(ctx);
    printf(g_fail ? "MATRIX: %d FAILURES\n" : "MATRIX: all green\n",
           g_fail);
    return g_fail ? 1 : 0;
}
