/*
 * fdk.h — Faded Dream ToolKit
 *
 * Single umbrella header. Applications may include just this file to
 * get the full public API, or include individual fdk_*.h headers for
 * finer-grained dependencies.
 *
 * FDK is proprietary software — see LICENSE.
 */

#ifndef FDK_H
#define FDK_H

#include "fdk_version.h"
#include "fdk_types.h"
#include "fdk_error.h"
#include "fdk_log.h"
#include "fdk_core.h"
#include "fdk_window.h"
#include "fdk_event.h"
#include "fdk_surface.h"
#include "fdk_widget.h"

/* Widget, layout, and theme public headers are added here as their
 * respective phases land (see docs/roadmap.md). Phase 2 added window
 * lifecycle and the backend-neutral event model; the first slice of
 * Phase 3 (software rendering surfaces) added fdk_surface.h on top
 * of them; Phase 4 added fdk_widget.h — the retained-mode widget
 * foundation (hierarchy, state, focus, event routing, invalidation,
 * painting, subclassing). Layout and theme headers follow. */

#endif /* FDK_H */
