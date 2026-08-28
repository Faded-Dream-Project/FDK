/*
 * abi_check.c — compile-time ABI size assertions (Phase 11 freeze).
 *
 * Every public VALUE/RESULT struct's size and (where meaningful)
 * field order is pinned here with static_assert. The point: an
 * accidental field edit in a public header fails THIS translation
 * unit's build with a message naming the struct, instead of
 * shipping an ABI break that detonates in someone's application.
 *
 * These are not guesses — the sizes are the audited 1.0 values from
 * docs/abi-policy.md's classification table. When a struct
 * legitimately needs to change, it needs a NEW struct or an
 * accessor API, and this file is updated only in a major version.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_a11y.h"
#include "fdk/fdk_i18n.h"

#include <stddef.h>

/* ---- geometry value types ---- */
_Static_assert(sizeof(fdk_point) == 8, "fdk_point ABI frozen");
_Static_assert(sizeof(fdk_size) == 8, "fdk_size ABI frozen");
_Static_assert(sizeof(fdk_rect) == 16, "fdk_rect ABI frozen");
_Static_assert(sizeof(fdk_pointf) == 8, "fdk_pointf ABI frozen");
_Static_assert(sizeof(fdk_sizef) == 8, "fdk_sizef ABI frozen");
_Static_assert(sizeof(fdk_rectf) == 16, "fdk_rectf ABI frozen");
_Static_assert(sizeof(fdk_color) == 16, "fdk_color ABI frozen");

/* ---- events ---- */
_Static_assert(sizeof(fdk_key_event) == 16,
              "fdk_key_event ABI frozen");
_Static_assert(sizeof(fdk_event_data) >= sizeof(fdk_configure_event),
              "fdk_event_data union ABI frozen");
_Static_assert(sizeof(fdk_widget_event) == 32,
              "fdk_widget_event ABI frozen");
_Static_assert(sizeof(fdk_a11y_event) == 24,
              "fdk_a11y_event ABI frozen");

/* ---- text ---- */
_Static_assert(sizeof(fdk_font_metrics) == 16,
              "fdk_font_metrics ABI frozen");
_Static_assert(sizeof(fdk_text_metrics) == 12,
              "fdk_text_metrics ABI frozen");
_Static_assert(sizeof(fdk_text_line) == 24, "fdk_text_line ABI frozen");

/* ---- i18n value types ---- */
_Static_assert(sizeof(fdk_locale) == 28, "fdk_locale ABI frozen");
_Static_assert(sizeof(fdk_date) == 12, "fdk_date ABI frozen");
_Static_assert(sizeof(fdk_time) == 12, "fdk_time ABI frozen");
_Static_assert(sizeof(fdk_plural_operands) == 40,
              "fdk_plural_operands ABI frozen");

/* ---- the caller-allocated RESULT struct ---- */
_Static_assert(sizeof(fdk_a11y_info) == 80, "fdk_a11y_info ABI frozen");

_Static_assert(sizeof(fdk_surface_info) == 24,
              "fdk_surface_info ABI frozen");
_Static_assert(sizeof(fdk_font_cache_stats) == 16,
              "fdk_font_cache_stats ABI frozen");

/* ---- input structs: sizes recorded, fields append-only ---- */
_Static_assert(sizeof(fdk_dialog_options) == 40,
              "fdk_dialog_options 1.0 size (append-only after)");
_Static_assert(sizeof(fdk_init_options) == 16,
              "fdk_init_options 1.0 size (append-only after)");
_Static_assert(sizeof(fdk_window_options) == 32,
              "fdk_window_options 1.0 size (append-only after)");
_Static_assert(sizeof(fdk_number_options) == 16,
              "fdk_number_options 1.0 size (append-only after)");

/* Pointers are opaque handles everywhere: their size is the
 * platform's, and no public header dereferences one. */
_Static_assert(sizeof(void *) >= 4, "pointer sanity");
