/*
 * fdk_desktop.h — Faded Dream ToolKit: opening things on the desktop
 *
 * The one desktop service every real application eventually needs and
 * no toolkit wants to admit is just a spawn: "open this URI in
 * whatever the user configured". FDK's answer is the XDG standard
 * command (`xdg-open`, the freedesktop.org INTER-OPERABILITY spec
 * every conforming desktop ships) launched DETACHED — no library
 * dependency, no D-Bus requirement, nothing to hang the UI thread on.
 *
 * Honesty rules, same as the rest of FDK's desktop lane:
 *   - the command is probed on the PATH FIRST (X_OK along $PATH);
 *     absent command = FDK_ERR_UNSUPPORTED, never a silent fork that
 *     dies with 127 and looks like success;
 *   - the child is its own session (setsid), its stdio is pointed at
 *     /dev/null, and the parent never waits — a desktop opener that
 *     blocks the event loop would be a bug, not a feature;
 *   - URIs are VALIDATED before they reach a shell-free exec: a
 *     scheme ([A-Za-z][A-Za-z0-9+.-]*:) or an absolute path, no
 *     control characters, no leading '-' (the option-injection
 *     guard; "xdg-open --something" is not ours to run).
 *
 * Works on X11 and Wayland alike — it is a process launch, not a
 * window-system call.
 */

#ifndef FDK_DESKTOP_H
#define FDK_DESKTOP_H

#include "fdk_types.h"
#include "fdk_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens `uri` with the desktop's configured handler (xdg-open).
 *
 * Accepted URIs: anything with a scheme — http://, https://, mailto:,
 * file://, ftp://, ... — or an ABSOLUTE POSIX path (converted to a
 * file:// URI internally; spaces and non-ASCII are percent-encoded).
 * Rejected (FDK_ERR_INVALID_ARGUMENT): NULL/empty, strings with
 * control characters, strings starting with '-', relative paths with
 * no scheme.
 *
 * FDK_ERR_UNSUPPORTED: no xdg-open executable on the PATH (a
 * container or a non-XDG desktop; the application may offer its own
 * fallback or surface the error). FDK_ERR_PLATFORM: the launch fork
 * itself failed. FDK_OK: the handler was launched — NOT "the document
 * opened" (whether the handler succeeded is the desktop's business;
 * xdg-open reports its own failures to the user).
 *
 * Thread-safety: UI thread (it may paint a "launching..." state);
 * the launch itself is non-blocking. */
fdk_result fdk_desktop_open_uri(const char *uri);

/* The file-twin convenience: opens the PATH with the desktop's
 * handler (a file:// URI under the hood). Same contract as
 * fdk_desktop_open_uri, including the absolute-path requirement. */
fdk_result fdk_desktop_open_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* FDK_DESKTOP_H */
