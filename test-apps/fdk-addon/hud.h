/* fdk-hud.h — the fdk-hud add-on's public API.
 *
 * fdk-hud is a floating gauge HUD panel — the release-proof add-on
 * package for FDK's extension API. It deliberately lives by the
 * rules docs/EXTENDING.md states for every third-party package:
 *
 *   - it links ONLY the INSTALLED FDK's public headers
 *     (fdk/fdk.h, fdk/fdk_extend.h, fdk/fdk_text.h) and its own
 *     native dependency (Xlib, for the role it builds itself);
 *   - its shared library exports only the fdk_hud_* namespace
 *     (see fdk-hud.exports.map) — the fdk_* namespace belongs to
 *     FDK alone;
 *   - it is loaded with dlopen() by a host that linked the shared
 *     libfdk: both sides then resolve fdk_* from the ONE loaded
 *     libfdk.so.1 (the soname dedup), which is the whole reason a
 *     context pointer can cross the boundary safely.
 *
 * Threading: exactly like FDK — every call here is UI-thread-only
 * (docs/threading.md), from fdk_run's thread.
 */
#ifndef FDK_HUD_H
#define FDK_HUD_H

#include <fdk/fdk_error.h>
#include <fdk/fdk_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The add-on's own ABI version, independent of FDK's. A host checks
 * this after dlopen before touching anything else. */
#define FDK_HUD_API_VERSION 1

typedef struct fdk_hud fdk_hud;

/* What the host reads after a session: the counters that prove the
 * add-on's machinery really ran (also the smoke test's receipt). */
typedef struct fdk_hud_stats {
    int kind_creates;       /* kind create callback invocations      */
    int kind_destroys;      /* kind destroy callback invocations     */
    int exposes;            /* EXPOSE repaints served                */
    int commands;           /* command-pipe lines processed          */
    int menu_opens;         /* popup menu openings                   */
    int menu_picks;         /* menu rows the user activated          */
} fdk_hud_stats;

/* Options (zero-initialize for defaults). */
typedef struct fdk_hud_options {
    const char *label;   /* caption under the gauge; NULL = none    */
    float value;         /* initial gauge fill, 0..1                */
    fdk_i32 x, y;        /* panel position on the screen            */
} fdk_hud_options;

/* The extension API this add-on was compiled against (compile-time
 * pair of the run-time check fdk_ext_api_version() makes inside
 * fdk_hud_init). */
int fdk_hud_api_version(void);

/* Initializes the add-on on `ctx`: registers the surface kind
 * "org.fdk.hud.panel", creates the panel surface (hidden — call
 * fdk_hud_show), starts the animation timer and the command-pipe
 * fd source. Returns FDK_OK and writes *out, or a negative
 * fdk_result:
 *   FDK_ERR_UNSUPPORTED  - the connected backend is not X11 (this
 *                          build speaks only the X11 role; the
 *                          honest refusal, never a crash)
 *   FDK_ERR_INVALID_ARGUMENT / FDK_ERR_OUT_OF_MEMORY / others
 */
fdk_result fdk_hud_init(fdk_context *ctx, const fdk_hud_options *options,
                        fdk_hud **out);

/* The panel surface lifecycle. show/hide are the extension API's
 * map/unmap; shutdown tears everything down (surface, popup, fd
 * source, timer, fonts, kind) and leaves *the context alone* —
 * the host owns it. */
void fdk_hud_show(fdk_hud *hud);
void fdk_hud_hide(fdk_hud *hud);
void fdk_hud_shutdown(fdk_hud *hud);

/* The gauge: value eases toward `v01` (clamped to 0..1) at the
 * animation timer's pace. */
void fdk_hud_set_value(fdk_hud *hud, float v01);

/* The caption under the gauge (copied). */
void fdk_hud_set_label(fdk_hud *hud, const char *text);

/* Opens the panel's popup menu programmatically — the same menu a
 * right-click opens, exposed so hosts (and tests) can drive it
 * from the keyboard or scripts. */
void fdk_hud_show_menu(fdk_hud *hud);

/* The WRITE end of the add-on's command pipe: write lines like
 * "value 0.42\n", "label disk\n", "hide\n", "show\n", "quit\n"
 * and the add-on's fd source (fdk_ext_loop_add_fd) processes them
 * on the UI thread. Never closed by the caller before shutdown. */
int fdk_hud_command_fd(fdk_hud *hud);

/* The counters. NULL asks for the FINAL counters of the last
 * shutdown on this process (the instance itself is freed; the
 * receipt outlives it). */
fdk_hud_stats fdk_hud_stats_read(fdk_hud *hud);

#ifdef __cplusplus
}
#endif

#endif /* FDK_HUD_H */
