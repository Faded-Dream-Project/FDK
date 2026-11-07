#ifndef FDK_DIALOG_PORTAL_INTERNAL_H
#define FDK_DIALOG_PORTAL_INTERNAL_H

/* The xdg-desktop-portal FileChooser seam (1.1.0, C-1 / B-09).
 *
 * portal_file_chooser.c implements the portal client;
 * file_dialog.c owns the fallback. The two halves call each other
 * through exactly these three functions — declared here so neither
 * includes the other's private state. */

#include "fdk/fdk_dialog.h"

/* The portal half's entry (called by fdk_dialog_show_impl BEFORE it
 * builds the own-browser window). FDK_OK = the portal took the pick
 * (the caller returns immediately); FDK_ERR_UNSUPPORTED = the portal
 * is unavailable/disabled (the caller proceeds with the own
 * browser); other errors propagate as the dialog's failure. */
fdk_result fdk__dialog_portal_try(fdk_context *ctx,
                                  const fdk_file_dialog_options *options,
                                  fdk_file_dialog_done_fn on_done,
                                  void *user_data);

/* Drops any in-flight portal request (subscription + bus + argument
 * copies). Safe when nothing flies. Called by the portal half
 * itself at every terminal path; declared for completeness/tests. */
void fdk__dialog_portal_flight_clear(void);

/* The fallback entry (called by the portal half when the portal
 * fails at ANY point — no bus, no portal, refused call, timeout).
 * Re-issues the whole dialog through the toolkit's own browser with
 * the flight's SAVED options (the options struct the application
 * passed was borrowed and may be gone by then — the flight copied
 * everything it needs at fdk__dialog_portal_try time). Implemented
 * in file_dialog.c. */
void fdk__dialog_fallback_own_browser(void);

#endif /* FDK_DIALOG_PORTAL_INTERNAL_H */
