/*
 * shortcut_internal.h — the matcher seam between the parser
 * (src/core/shortcut.c) and its consumers (window shortcut table,
 * menu accelerators). The public face of this subsystem is
 * fdk_shortcut_parse / fdk_shortcut_format in fdk_event.h; the
 * window and menu layers call THIS to test a parsed pair against a
 * live key event.
 */

#ifndef FDK_SHORTCUT_INTERNAL_H
#define FDK_SHORTCUT_INTERNAL_H

#include "fdk/fdk_event.h"

/* Exact-match test of a parsed (modifiers, scancode) pair against a
 * key event. See shortcut.c for the equality semantics (extra held
 * modifiers block the match; is_repeat does NOT — repeat presses
 * fire like initial ones). */
bool fdk__shortcut_matches(fdk_u32 spec_modifiers, fdk_scancode spec_key,
                           const fdk_key_event *event);

#endif /* FDK_SHORTCUT_INTERNAL_H */
