/* sidebar.h — fdk-files' sidebar, §31/PART-5 spec.
 *
 * One scrollable column, three sections, exact geometry:
 *   Places     Home, Desktop/Documents/Downloads/Music/Pictures/
 *              Videos (XDG dirs, shown only if they exist), Trash
 *              (user-trash / user-trash-full by ~/.local/share/
 *              Trash/files emptiness)
 *   Bookmarks  the user's bookmarks — own store AND the shared
 *              ~/.config/gtk-3.0/bookmarks (format
 *              "file:///path Label"), imported once, written back
 *   Devices    File System (/) + every real mounted volume from
 *              /proc/self/mountinfo (the include/exclude table),
 *              labels from /dev/disk/by-label, a 3px usage bar
 *
 * The MODEL is plain data (sidebar_model.c, unit-testable with a
 * fixture mountinfo); the VIEW is a List fed from it.
 */
#ifndef FDK_FILES_SIDEBAR_H
#define FDK_FILES_SIDEBAR_H

#include "../common/app.h"
#include "../common/ta_buf.h"

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include <stdbool.h>
#include <stddef.h>

/* ---- the model ------------------------------------------------------ */

typedef enum sb_section {
    SB_SECTION_PLACES = 0,
    SB_SECTION_BOOKMARKS,
    SB_SECTION_DEVICES,
    SB_SECTION_COUNT,
} sb_section;

typedef struct sb_row {
    sb_section section;
    char label[128];
    char path[512];
    char icon[64];       /* the freedesktop icon name             */
    bool trash;          /* the trash row (full-detection re-run)  */
    bool removable;      /* a device that can eject               */
    bool network;        /* nfs/cifs/smb/sshfs                    */
    /* the device rows' usage bar (filled by the model on refresh) */
    bool has_usage;
    unsigned long long used_bytes, total_bytes;
} sb_row;

/* The model: rows in display order, sections in enum order. */
typedef struct sb_model {
    sb_row rows[64];
    size_t count;
} sb_model;

/* Builds the model. `mountinfo_path` is normally
 * "/proc/self/mountinfo"; tests pass a fixture file. `label_root`
 * is normally "/dev/disk" (by-label + by-partlabel resolution);
 * tests pass a fake tree. Returns the row count. */
size_t sb_model_build(sb_model *m, const char *mountinfo_path,
                      const char *label_root);

/* The XDG-user-dir resolution: $XDG_CONFIG_HOME/user-dirs.dirs
 * ("XDG_DOCUMENTS_DIR=\"$HOME/Documents\"" lines), fallback to the
 * conventional $HOME/<name>. Exists-check included. */
void sb_model_places(sb_model *m);
void sb_model_bookmarks(sb_model *m);
void sb_model_devices(sb_model *m, const char *mountinfo_path,
                      const char *label_root);

/* One line of /proc/self/mountinfo:
 * 36 35 98:0 /mnt1 /some/mountpoint rw,noatime master:1 - ext3
 * /dev/root rw,errors=continue
 * (fields: id parent maj:min root mountpoint options [optional
 * fields] - fstype source super-options). */
bool sb_parse_mountinfo_line(const char *line, char *mountpoint,
                             size_t mp_cap, char *fstype,
                             size_t fs_cap, char *source,
                             size_t src_cap);

/* The include/exclude table (true = a real, listable volume). */
bool sb_fs_is_real(const char *fstype, const char *mountpoint);

/* Trash fullness: ~/.local/share/Trash/files non-empty. */
bool sb_trash_nonempty(void);

/* ---- the view -------------------------------------------------------- */

/* Builds the sidebar inside `parent` (a vertical box); `width` is
 * the initial width (persisted by the caller). */
void sb_build(fdk_widget *parent, fdk_i32 width);

/* Re-reads everything (mount changes, trash state) and rebuilds
 * the view. Called at boot, on the mountinfo poll event, and after
 * bookmark edits. */
void sb_refresh(void);

/* The selected/current row's path ("" when none). */
const char *sb_current_path(void);

/* Selects the row whose path matches (the current-folder
 * highlight); no match clears the highlight. */
void sb_highlight_path(const char *path);

/* Bookmarks: add the path (label = basename), used by Ctrl+D. */
void sb_bookmark_add(const char *path, const char *label);

/* The prefs-persisted width. */
fdk_i32 sb_get_width(void);
void sb_set_width(fdk_i32 w);

#endif /* FDK_FILES_SIDEBAR_H */
