/* mime.h — shared-mime-info lookup for fdk-files.
 *
 * Implements the lookup over the INSTALLED database (when present):
 *   1. globs2 — "weight:mimetype:pattern[:flags]" lines; keep the
 *      highest-weight matches, then the longest pattern; the `cs`
 *      flag marks case-sensitive patterns (default is insensitive)
 *   2. magic — the "MIME-Magic\0\n" text format: [priority:type]
 *      blocks of ">indent =len value [&mask][~word][+range]"
 *      match lines, nested by indent (parent = previous shallower
 *      line; semantics: a and (b or c))
 *   3. fall back to "binary or text" sniffing: a NUL or a control
 *      char in the first 128 bytes -> application/octet-stream,
 *      else text/plain
 * plus a small built-in glob table for systems without the
 * database (the sandbox's bare image), and the icon-name mapping
 * per type for the icon theme.
 *
 * Spec source: the Shared MIME-info Database specification
 * (specifications.freedesktop.org/shared-mime-info) — see README.
 */
#ifndef FILES_MIME_H
#define FILES_MIME_H

#include "files.h"

#include <stddef.h>

/* Loads the globs2 + magic databases (fail-soft: the built-in
 * table answers when absent). Call once at startup. */
void mime_init(void);
void mime_shutdown(void);

/* The type of `name` (basename) with content sniffing: `path` may
 * be NULL to skip magic. Fills out_type (always a valid type —
 * application/octet-stream / text/plain floor) and out_icon (the
 * icon-theme name for the row). */
void mime_of(const char *name, const char *path, char *out_type,
             size_t type_cap, char *out_icon, size_t icon_cap);

/* The generic icon for a directory / mount / special place. */
const char *mime_dir_icon(const char *dir_path);

/* A short human category ("Folder", "Image", "Archive", ...) for
 * the Type column and the a11y name. */
const char *mime_category(const char *mime_type);

/* True when the type is a known image format we can thumbnail. */
bool mime_is_thumbnailable(const char *mime_type);

#endif /* FILES_MIME_H */
