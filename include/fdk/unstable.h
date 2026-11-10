/*
 * fdk/unstable.h — the EXPLICITLY UNSTABLE tier (1.0.0).
 *
 * Every fdk_unstable_* symbol is exported from libfdk.so under the
 * FDK_UNSTABLE version node (a separate --version-script the build
 * generates next to the stable map) and is governed by ONE rule: it
 * may change or vanish in ANY release — no version bump, no SONAME
 * move, no apology. Add-ons include this header per source file and
 * check fdk_unstable_api_level() at runtime; the tier's number grows
 * as residents are added, and NOTHING here is covered by
 * docs/abi-policy.md.
 *
 * What belongs here: deeper access real add-ons need while a
 * first-class design settles — the proving ground. Graduates move
 * into the public headers (append-only, the ordinary discipline).
 * First candidates queued for the next rounds: per-glyph shaping
 * access, raw framebuffer acquisition, image-frame decode.
 */
#ifndef FDK_UNSTABLE_H
#define FDK_UNSTABLE_H

#include "fdk/fdk_error.h"
#include "fdk/fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The tier's own level: grows when residents are added. An add-on
 * using the tier pins this at compile time and checks it at runtime —
 * the extension-API discipline, applied to the unstable tier. */
#define FDK_UNSTABLE_API_LEVEL 1

int fdk_unstable_api_level(void);

/* The tier marker as a string ("FDK_UNSTABLE" — diagnostics/logging
 * convenience for add-ons that print their linkage). */
const char *fdk_unstable_tier_name(void);

#ifdef __cplusplus
}
#endif
#endif /* FDK_UNSTABLE_H */
