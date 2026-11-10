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

    /* ---- the render-mode caps follow the resolved mode ------------------ */
    CHECK((fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_HYBRID) ||
           fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_GPU)),
          "HYBRID or GPU resolves (never both, never neither)");
    CHECK(!(fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_HYBRID) &&
            fdk_ext_has_capability(ctx, FDK_EXT_CAP_RENDER_GPU)),
          "HYBRID and GPU are exclusive");

    /* ---- the unstable-tier rule ----------------------------------------- */
    /* No unstable-protocol TYPE name may appear in a public header:
     * the bindings live in src/platform/wayland/generated/ and the
     * public surface speaks void* (fdk_wl_raw_bind). */
    {
        static const char *pub_headers[] = {
            "include/fdk/fdk_extend.h", "include/fdk/fdk_text.h",
            "include/fdk/fdk_window.h", "include/fdk/fdk_surface.h",
            "include/fdk/fdk_core.h",   NULL,
        };
        static const char *banned[] = {
            "zwp_", "zwlr_", "ext_session_lock", "xdg_activation_v1",
            "wl_registry", "wl_seat", NULL,
        };
        int clean = 1;
        for (int i = 0; pub_headers[i] != NULL; i++) {
            FILE *f = fopen(pub_headers[i], "r");
            if (f == NULL) {
                continue;
            }
            /* strip comments over the WHOLE file (block-comment state
             * survives line breaks), then scan what remains */
            char buf[256 * 1024];
            size_t len = fread(buf, 1, sizeof buf - 1, f);
            buf[len] = '\0';
            fclose(f);
            char *code = malloc(len + 1);
            size_t o = 0;
            int in_comment = 0, in_str = 0, in_chr = 0;
            for (size_t k = 0; k < len; k++) {
                char c = buf[k], n2 = k + 1 < len ? buf[k + 1] : '\0';
                if (in_comment) {
                    if (c == '*' && n2 == '/') {
                        in_comment = 0;
                        k++;
                    }
                    continue;
                }
                if (in_str) {
                    if (c == '\\') {
                        k++;
                    } else if (c == '"') {
                        in_str = 0;
                    }
                    code[o++] = ' ';
                    continue;
                }
                if (in_chr) {
                    if (c == '\\') {
                        k++;
                    } else if (c == '\'') {
                        in_chr = 0;
                    }
                    code[o++] = ' ';
                    continue;
                }
                if (c == '/' && n2 == '*') {
                    in_comment = 1;
                    k++;
                    code[o++] = ' ';
                    continue;
                }
                if (c == '/' && n2 == '/') {
                    break; /* the rest of the line is a comment */
                }
                if (c == '"') {
                    in_str = 1;
                } else if (c == '\'') {
                    in_chr = 1;
                }
                code[o++] = c;
            }
            code[o] = '\0';
            for (int b = 0; banned[b] != NULL; b++) {
                if (strstr(code, banned[b]) != NULL) {
                    printf("[FAIL] unstable-tier leak: %s declares '%s'\n",
                           pub_headers[i], banned[b]);
                    clean = 0;
                }
            }
            free(code);
        }
        CHECK(clean, "no unstable-protocol declarations in public "
                     "headers");
    }

    fdk_shutdown(ctx);
    printf(g_fail ? "MATRIX: %d FAILURES\n" : "MATRIX: all green\n",
           g_fail);
    return g_fail ? 1 : 0;
}
