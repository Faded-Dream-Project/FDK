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
#include "fdk_layout.h"
#include "fdk_text.h"
#include "fdk_widgets.h"
#include "fdk_theme.h"
#include "fdk_clipboard.h"
#include "fdk_dialog.h"

/* Widget, layout, and theme public headers are added here as their
 * respective phases land (see docs/roadmap.md). Phase 2 added window
 * lifecycle and the backend-neutral event model; Phase 3 added
 * fdk_surface.h (software rendering); Phase 4 added fdk_widget.h (the
 * retained-mode widget foundation); Phase 5 added fdk_layout.h (the
 * box layout engine + window content integration); Phase 6 added
 * fdk_text.h (fonts, UTF-8 text measurement and rendering via the
 * vendored stb_truetype) and the widget catalog; Phase 7 added
 * fdk_theme.h (themes: the built-in default, .fdk parsing, runtime
 * switching); Phase 9 adds fdk_clipboard.h (text clipboard over the
 * ICCCM CLIPBOARD selection / wl_data_device). Further layout
 * containers follow. */

#endif /* FDK_H */
