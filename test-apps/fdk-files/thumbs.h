/* thumbs.h — asynchronous image thumbnails with an XDG cache (§25/4c).
 *
 * The worker way (G-2): the view marks image rows thumb_pending and
 * asks for a thumbnail; a worker decodes + down-scales + writes the
 * cache file; the UI thread receives the ready surface and repaints
 * the row. The cache lives under
 * $XDG_CACHE_HOME/fdk-files/thumbs/128/ (default ~/.cache/...),
 * keyed by (mtime, size, name) so edits invalidate.
 *
 * While loading, rows show the normal file icon (the loader's
 * fallback ladder) — no blank flashes.
 */
#ifndef FILES_THUMBS_H
#define FILES_THUMBS_H

#include "files.h"

#include "fdk/fdk_surface.h"

#define THUMB_SIZE 96

/* The cache directory (created on demand). False when it cannot
 * exist (read-only HOME etc. — thumbnails then simply miss). */
bool thumbs_dir(char *out, size_t cap);

/* The cache key path for one file (mtime-size-name hashed into a
 * safe file name). */
void thumbs_cache_path(const file_info *fi, char *out, size_t cap);

/* TRUE when `fi` should have a thumbnail (image types). */
bool thumbs_wanted(const file_info *fi);

/* Requests a thumbnail. Returns immediately:
 *   - cache hit  -> *out_surface set (caller blits; the surface is
 *                   owned by the thumbs module's LRU cache)
 *   - cache miss -> a worker job started; the row keeps its icon
 *                   until thumb_ready fires (UI thread)
 * The READY callback hands the file path it belongs to (the view
 * matches rows by path); surfaces are module-owned, valid until
 * thumbs_shutdown. */
typedef void (*thumb_ready_fn)(const char *path,
                               fdk_surface *thumb, void *user);

bool thumbs_get(const file_info *fi, fdk_surface **out_surface,
                thumb_ready_fn on_ready, void *user);

/* Drops the in-memory LRU (a live re-theme: the next get re-tints
 * through the icon ladder — thumbnails are photos, NOT tinted). */
void thumbs_invalidate(void);

/* Stops the thumbnail worker (joins; called at teardown). */
void thumbs_shutdown(void);

#endif /* FILES_THUMBS_H */
