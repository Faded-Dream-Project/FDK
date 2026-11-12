/* main.c — gpu_strict, the hardware-backend requirement app.
 *
 * The owner's ask: "an app that will close if Vulkan or OpenGL is
 * not used reporting an error about it — the app just refuses CPU
 * last fallback, not llvmpipe, pure OpenGL or Vulkan."
 *
 * The contract, exactly:
 *   - run on the Vulkan lane, or
 *   - run on the OpenGL lane (GLX on X11 / EGL on Wayland),
 *   - with a HARDWARE device behind it — the software rasterizers
 *     the lanes refuse by policy (llvmpipe on GL, lavapipe /
 *     SwiftShader on Vulkan) are NOT acceptable here either, even
 *     when FDK_GL_FORCE engaged them;
 *   - anything else (the CPU path, a software GPU) is REFUSED: a
 *     clear error on stderr naming what happened, exit code 1, no
 *     window shown beyond the probe.
 *
 * The forcing knobs work as in the library: FDK_GPU_BACKEND=vulkan /
 * =opengl / =auto (or the FDK_GL_VULKAN=1 / FDK_GL_OPENGL=1
 * aliases) — the app reports whichever lane actually carries it.
 *
 * How it probes: create a HYBRID window (the mode where lanes
 * engage whenever they can), paint + present one frame (a lane only
 * reports LIVE once it has actually carried a frame — the query
 * reads the backend's live state), ask
 * fdk_window_get_render_backend() + fdk_window_render_device(), and
 * apply the contract to the answer.
 *
 * Everything on the public API only, like every test-app here.
 *
 * Exit codes: 0 = a hardware Vulkan or OpenGL lane carried the
 * frame; 1 = refused (with the reason on stderr); 2 = FDK itself
 * failed before the question could be asked.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_window.h"

#include <stdio.h>
#include <stdlib.h> /* getenv, atoi — HOLD_MS */
#include <string.h>

/* The software-renderer hall of shame (the lanes' own policy list,
 * mirrored here so the app can refuse a FORCED software lane too). */
static int device_is_software(const char *d) {
    static const char *const names[] = {
        "llvmpipe", "softpipe", "swrast",
        "lavapipe", "SwiftShader", "swiftshader", "SOFTWARE"
    };
    if (d == NULL) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strstr(d, names[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

static const char *backend_name(fdk_render_backend b) {
    switch (b) {
    case FDK_RENDER_BACKEND_VULKAN:
        return "vulkan";
    case FDK_RENDER_BACKEND_OPENGL:
        return "opengl";
    default:
        return "cpu";
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "gpu_strict: fdk_init failed — no display?\n");
        return 2;
    }

    fdk_window_options wopts;
    memset(&wopts, 0, sizeof(wopts));
    wopts.title = "fdk-gpu-strict";
    wopts.width = 480;
    wopts.height = 320;
    /* HYBRID (not GPU): the probe wants the lane that WOULD carry
     * the frame; GPU mode's refusal semantics would collapse the
     * "no lane" case into a create failure and lose the report. */
    wopts.render_mode = FDK_RENDER_HYBRID;

    fdk_window *win = NULL;
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &win))) {
        fprintf(stderr, "gpu_strict: window create failed\n");
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_window_show(win);

    /* Carry ONE frame — the lane reports live only after it has.
     * The present result is CHECKED, not assumed: a lane that
     * answers "vulkan" while its presents silently fail would pass
     * the old probe (the owner's 2026-11-12 report: "the window
     * opened and closed instantly, that doesn't prove vulkan
     * works"). fdk_surface_present returning OK is the frame
     * actually committed to the lane's queue. */
    fdk_result present_r = FDK_ERR_INVALID_STATE;
    fdk_surface *s = NULL;
    if (fdk_ok(fdk_window_get_surface(win, &s)) && s != NULL) {
        fdk_surface_fill(s, (fdk_color){ 0.08f, 0.10f, 0.14f, 1.0f });
        fdk_surface_fill_rect(s, (fdk_rect){ 40, 40, 400, 240 },
                              (fdk_color){ 0.95f, 0.35f, 0.15f, 1.0f });
        present_r = fdk_surface_present(s);
    }
    for (int i = 0; i < 8; i++) {
        fdk_pump_events(ctx, 16);
    }

    /* Optional hold (gpu_smoke's rig knob, mirrored here): HOLD_MS=n
     * keeps the window alive repainting for n milliseconds so a
     * screen probe (grim / a phone camera on real hardware) can
     * capture a live frame — the visual proof the logs can't give.
     * Repaints every ~33ms so damage tracking stays exercised. */
    {
        const char *hold = getenv("HOLD_MS");
        int ms = hold != NULL ? atoi(hold) : 0;
        for (int t = 0; ms > 0 && t < ms; t += 33) {
            if (s != NULL) {
                (void)fdk_surface_present(s);
            }
            fdk_pump_events(ctx, 33);
        }
    }

    fdk_render_backend backend = FDK_RENDER_BACKEND_CPU;
    (void)fdk_window_get_render_backend(win, &backend);
    const char *device = fdk_window_render_device(win);

    fdk_window_destroy(win);
    fdk_shutdown(ctx);

    if (!fdk_ok(present_r)) {
        fprintf(stderr,
                "gpu_strict: REFUSED — the frame never committed "
                "(fdk_surface_present: %s); a backend answer without "
                "a carried frame proves nothing.\n",
                fdk_result_to_string(present_r));
        return 1;
    }
    if (backend == FDK_RENDER_BACKEND_CPU) {
        fprintf(stderr,
                "gpu_strict: REFUSED — FDK fell back to the CPU "
                "present path (no Vulkan or OpenGL lane engaged; "
                "check the driver, or force with "
                "FDK_GPU_BACKEND=vulkan|opengl). This app requires "
                "a hardware GPU backend.\n");
        return 1;
    }
    if (device_is_software(device)) {
        fprintf(stderr,
                "gpu_strict: REFUSED — the engaged %s lane runs on a "
                "SOFTWARE renderer (\"%s\"); this app requires "
                "hardware Vulkan or OpenGL, not llvmpipe/lavapipe.\n",
                backend_name(backend), device);
        return 1;
    }

    printf("gpu_strict: OK — backend=%s device=\"%s\" (hardware, "
           "frame committed)\n",
           backend_name(backend), device);
    return 0;
}
