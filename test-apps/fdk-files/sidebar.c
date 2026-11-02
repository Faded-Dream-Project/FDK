/* sidebar.c — the §31/PART-5 model: XDG places, the shared GTK
 * bookmarks file, and /proc/self/mountinfo devices. Pure data; the
 * view (sidebar_view.c) renders it. */
#include "sidebar.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/statvfs.h>

/* ---- the XDG user dirs ---------------------------------------------- */

/* Reads $XDG_CONFIG_HOME/user-dirs.dirs looking for
 * XDG_<NAME>_DIR="$HOME/Sub" or an absolute path. */
static bool xdg_user_dir(const char *name, char *out, size_t cap) {
    const char *home = getenv("HOME");
    const char *cfg = getenv("XDG_CONFIG_HOME");
    char path[512];
    if (cfg != NULL && cfg[0] != '\0') {
        snprintf(path, sizeof path, "%s/user-dirs.dirs", cfg);
    } else if (home != NULL && home[0] != '\0') {
        snprintf(path, sizeof path, "%s/.config/user-dirs.dirs",
                 home);
    } else {
        return false;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char key[64];
    snprintf(key, sizeof key, "XDG_%s_DIR", name);
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof line, f) != NULL) {
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *k = line;
        while (*k == ' ' || *k == '\t') {
            k++;
        }
        if (strcmp(k, key) != 0) {
            continue;
        }
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        /* strip the trailing newline and quotes */
        size_t vl = strlen(v);
        while (vl > 0 && (v[vl - 1] == '\n' || v[vl - 1] == '\r' ||
                          v[vl - 1] == '"')) {
            v[--vl] = '\0';
        }
        if (v[0] == '"') {
            v++;
        }
        if (strncmp(v, "$HOME/", 6) == 0 && home != NULL) {
            snprintf(out, cap, "%s/%s", home, v + 6);
        } else if (v[0] == '/') {
            snprintf(out, cap, "%s", v);
        } else {
            continue;
        }
        found = true;
        break;
    }
    fclose(f);
    return found;
}

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void sb_model_places(sb_model *m) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        home = "/";
    }
    /* Home is always there. */
    sb_row *r = &m->rows[m->count++];
    memset(r, 0, sizeof *r);
    r->section = SB_SECTION_PLACES;
    snprintf(r->label, sizeof r->label, "Home");
    snprintf(r->path, sizeof r->path, "%s", home);
    snprintf(r->icon, sizeof r->icon, "user-home");

    /* the XDG dirs: shown ONLY when they exist (the spec's rule) */
    static const struct {
        const char *name;      /* the user-dirs.dirs key             */
        const char *fallback;  /* the conventional subdirectory      */
        const char *label;
        const char *icon;
    } dirs[] = {
        {"DESKTOP", "Desktop", "Desktop", "user-desktop"},
        {"DOCUMENTS", "Documents", "Documents", "folder-documents"},
        {"DOWNLOAD", "Downloads", "Downloads", "folder-download"},
        {"MUSIC", "Music", "Music", "folder-music"},
        {"PICTURES", "Pictures", "Pictures", "folder-pictures"},
        {"VIDEOS", "Videos", "Videos", "folder-videos"},
    };
    for (size_t i = 0;
         i < sizeof dirs / sizeof *dirs && m->count < 64; i++) {
        char path[512];
        bool have = xdg_user_dir(dirs[i].name, path, sizeof path);
        if (!have) {
            ta_join(path, sizeof path, home, dirs[i].fallback);
        }
        if (!dir_exists(path)) {
            continue; /* the spec: shown only if it exists */
        }
        r = &m->rows[m->count++];
        memset(r, 0, sizeof *r);
        r->section = SB_SECTION_PLACES;
        snprintf(r->label, sizeof r->label, "%s", dirs[i].label);
        snprintf(r->path, sizeof r->path, "%s", path);
        snprintf(r->icon, sizeof r->icon, "%s", dirs[i].icon);
    }

    /* Trash lives HERE (not under Devices — S4's misplacement). */
    r = &m->rows[m->count++];
    memset(r, 0, sizeof *r);
    r->section = SB_SECTION_PLACES;
    snprintf(r->label, sizeof r->label, "Trash");
    {
        char tp[512];
        ta_join(tp, sizeof tp, home, ".local/share/Trash");
        snprintf(r->path, sizeof r->path, "trash://%.480s", tp);
    }
    r->trash = true;
    snprintf(r->icon, sizeof r->icon,
             sb_trash_nonempty() ? "user-trash-full" : "user-trash");
}

/* ---- the trash ------------------------------------------------------ */

bool sb_trash_nonempty(void) {
    const char *home = getenv("HOME");
    if (home == NULL) {
        return false;
    }
    char files[512];
    snprintf(files, sizeof files, "%s/.local/share/Trash/files",
             home);
    DIR *d = opendir(files);
    if (d == NULL) {
        return false;
    }
    struct dirent *de;
    bool any = false;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        any = true;
        break;
    }
    closedir(d);
    return any;
}

/* ---- the bookmarks ---------------------------------------------------
 * Own store (fdk-files/bookmarks, the pre-§31 format) AND the shared
 * ~/.config/gtk-3.0/bookmarks ("file:///path Label" per line). The
 * shared file is read when present and MERGED; writes go to BOTH
 * (the shared one so other file managers see ours). */

typedef struct bm {
    char path[512];
    char label[128];
} bm;

static bm g_bms[32];
static size_t g_bm_count;

/* parses "file:///path Label" (label optional) or a bare path */
static bool bm_parse_line(const char *line, char *path, size_t pcap,
                          char *label, size_t lcap) {
    if (line[0] == '#' || line[0] == '\0') {
        return false;
    }
    if (strncmp(line, "file://", 7) == 0) {
        const char *p = line + 7;
        const char *sp = strchr(p, ' ');
        if (sp != NULL) {
            size_t plen = (size_t)(sp - p);
            if (plen >= pcap) {
                plen = pcap - 1;
            }
            memcpy(path, p, plen);
            path[plen] = '\0';
            ta_copy(label, lcap, sp + 1);
        } else {
            ta_copy(path, pcap, p);
            label[0] = '\0';
        }
        return path[0] != '\0';
    }
    /* the own-store format: "/path" or "/path # comment" */
    ta_copy(path, pcap, line);
    char *sp = strchr(path, ' ');
    if (sp != NULL) {
        *sp = '\0';
    }
    label[0] = '\0';
    return path[0] == '/';
}

static void bm_load_file(const char *path, bool gtk_format) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof line, f) != NULL &&
           g_bm_count < 32) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = '\0';
        }
        bm b;
        if (!bm_parse_line(line, b.path, sizeof b.path, b.label,
                           sizeof b.label)) {
            continue;
        }
        if (!gtk_format) {
            /* the own store's lines are paths only */
        }
        /* dedupe by path */
        bool dup = false;
        for (size_t i = 0; i < g_bm_count; i++) {
            if (strcmp(g_bms[i].path, b.path) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup && dir_exists(b.path)) {
            g_bms[g_bm_count++] = b;
        }
    }
    fclose(f);
}

static void bookmarks_load_all(void) {
    g_bm_count = 0;
    const char *home = getenv("HOME");
    const char *cfg = getenv("XDG_CONFIG_HOME");
    char own[512], shared[512];
    if (cfg != NULL && cfg[0] != '\0') {
        snprintf(own, sizeof own, "%s/fdk-files/bookmarks", cfg);
        snprintf(shared, sizeof shared,
                 "%s/gtk-3.0/bookmarks", cfg);
    } else if (home != NULL && home[0] != '\0') {
        snprintf(own, sizeof own,
                 "%s/.config/fdk-files/bookmarks", home);
        snprintf(shared, sizeof shared,
                 "%s/.config/gtk-3.0/bookmarks", home);
    } else {
        return;
    }
    /* the own store first (display order), then the shared file's
     * additions merged in */
    bm_load_file(own, false);
    bm_load_file(shared, true);
}

static const char *bm_basename(const char *path) {
    const char *b = strrchr(path, '/');
    return (b != NULL && b[1] != '\0') ? b + 1 : path;
}

static void bookmarks_save_all(void) {
    const char *home = getenv("HOME");
    const char *cfg = getenv("XDG_CONFIG_HOME");
    char own[512], shared[512];
    if (cfg != NULL && cfg[0] != '\0') {
        snprintf(own, sizeof own, "%s/fdk-files/bookmarks", cfg);
        snprintf(shared, sizeof shared,
                 "%s/gtk-3.0/bookmarks", cfg);
    } else if (home != NULL && home[0] != '\0') {
        snprintf(own, sizeof own,
                 "%s/.config/fdk-files/bookmarks", home);
        snprintf(shared, sizeof shared,
                 "%s/.config/gtk-3.0/bookmarks", home);
    } else {
        return;
    }
    FILE *f = fopen(own, "w");
    if (f != NULL) {
        for (size_t i = 0; i < g_bm_count; i++) {
            fprintf(f, "%s\n", g_bms[i].path);
        }
        fclose(f);
    }
    f = fopen(shared, "w");
    if (f != NULL) {
        for (size_t i = 0; i < g_bm_count; i++) {
            fprintf(f, "file://%s %s\n", g_bms[i].path,
                    g_bms[i].label[0] != '\0'
                        ? g_bms[i].label
                        : bm_basename(g_bms[i].path));
        }
        fclose(f);
    }
}

void sb_bookmark_add(const char *path, const char *label) {
    if (path == NULL || path[0] == '\0' || g_bm_count >= 32) {
        return;
    }
    for (size_t i = 0; i < g_bm_count; i++) {
        if (strcmp(g_bms[i].path, path) == 0) {
            return; /* already bookmarked */
        }
    }
    bm *b = &g_bms[g_bm_count++];
    snprintf(b->path, sizeof b->path, "%s", path);
    snprintf(b->label, sizeof b->label, "%s",
             (label != NULL && label[0] != '\0')
                 ? label
                 : bm_basename(path));
    bookmarks_save_all();
}

void sb_model_bookmarks(sb_model *m) {
    bookmarks_load_all();
    for (size_t i = 0; i < g_bm_count && m->count < 64; i++) {
        sb_row *r = &m->rows[m->count++];
        memset(r, 0, sizeof *r);
        r->section = SB_SECTION_BOOKMARKS;
        snprintf(r->label, sizeof r->label, "%s",
                 g_bms[i].label[0] != '\0'
                     ? g_bms[i].label
                     : bm_basename(g_bms[i].path));
        snprintf(r->path, sizeof r->path, "%s", g_bms[i].path);
        snprintf(r->icon, sizeof r->icon, "%s", "folder");
    }
}

/* ---- the devices (mountinfo) ---------------------------------------- */

bool sb_parse_mountinfo_line(const char *line, char *mountpoint,
                             size_t mp_cap, char *fstype,
                             size_t fs_cap, char *source,
                             size_t src_cap) {
    /* fields: id parent maj:min root MOUNTPOINT opts [opt...] - FSTYPE
     * SOURCE super-opts */
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", line);
    char *fields[16];
    size_t nf = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save);
         tok != NULL && nf < 16; tok = strtok_r(NULL, " \t", &save)) {
        fields[nf++] = tok;
    }
    if (nf < 7) {
        return false;
    }
    /* find the '-' separator: everything before is optional fields */
    size_t dash = 0;
    bool have_dash = false;
    for (size_t i = 6; i < nf; i++) {
        if (strcmp(fields[i], "-") == 0) {
            dash = i;
            have_dash = true;
            break;
        }
    }
    if (!have_dash || dash + 2 >= nf) {
        return false;
    }
    /* octal escapes in the mountpoint (\040 = space) */
    const char *mp = fields[4];
    size_t w = 0;
    for (size_t i = 0; mp[i] != '\0' && w + 1 < mp_cap; i++) {
        if (mp[i] == '\\' && mp[i + 1] == '0' && mp[i + 2] == '4' &&
            mp[i + 3] == '0') {
            mountpoint[w++] = ' ';
            i += 3;
        } else {
            mountpoint[w++] = mp[i];
        }
    }
    mountpoint[w] = '\0';
    snprintf(fstype, fs_cap, "%s", fields[dash + 1]);
    snprintf(source, src_cap, "%s", fields[dash + 2]);
    return true;
}

bool sb_fs_is_real(const char *fstype, const char *mountpoint) {
    /* pseudo/virtual types never list */
    static const char *const pseudo[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "ramfs",
        "cgroup", "cgroup2", "overlay", "squashfs", "autofs", "bpf",
        "tracefs", "debugfs", "securityfs", "configfs", "fusectl",
        "mqueue", "hugetlbfs", "pstore", "efivarfs", "binfmt_misc",
        "devtmpfs",
    };
    for (size_t i = 0; i < sizeof pseudo / sizeof *pseudo; i++) {
        if (strcmp(fstype, pseudo[i]) == 0) {
            return false;
        }
    }
    /* excluded prefixes (spec): sys, proc, dev and run trees
     * (EXCEPT run-media-user and run-mount), docker, snapd, boot;
     * the literal slashes live in the table below */
    static const char *const bad_prefixes[] = {
        "/sys", "/proc", "/dev", "/run/user", "/var/lib/docker",
        "/var/lib/snapd", "/boot",
    };
    for (size_t i = 0; i < sizeof bad_prefixes / sizeof *bad_prefixes;
         i++) {
        size_t n = strlen(bad_prefixes[i]);
        if (strncmp(mountpoint, bad_prefixes[i], n) == 0) {
            /* run-media-user and run-mount are the exceptions
             * (only against the run prefixes) */
            if (strncmp(mountpoint, "/run/media/", 11) == 0 &&
                strlen(mountpoint) > 11) {
                return true;
            }
            if (strncmp(mountpoint, "/run/mount", 10) == 0 &&
                (mountpoint[10] == '\0' || mountpoint[10] == '/')) {
                return true;
            }
            return false;
        }
    }
    if (strncmp(mountpoint, "/run", 4) == 0) {
        /* the general /run exclusion with the same two exceptions */
        if (strncmp(mountpoint, "/run/media/", 11) == 0 &&
            strlen(mountpoint) > 11) {
            return true;
        }
        if (strncmp(mountpoint, "/run/mount", 10) == 0 &&
            (mountpoint[10] == '\0' || mountpoint[10] == '/')) {
            return true;
        }
        return false;
    }
    /* squashfs of loop devices (the live-ISO root trick) */
    if (strcmp(fstype, "squashfs") == 0) {
        return false;
    }
    return true;
}

/* the label from /dev/disk/by-label (symlinks resolved to match the
 * mount source), else by-partlabel, else the last path component */
static void device_label(const char *source, const char *mountpoint,
                         const char *label_root, char *out,
                         size_t cap) {
    char by[512];
    const char *tables[] = {"by-label", "by-partlabel"};
    for (size_t t = 0; t < 2; t++) {
        snprintf(by, sizeof by, "%s/%s", label_root, tables[t]);
        DIR *d = opendir(by);
        if (d == NULL) {
            continue;
        }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') {
                continue;
            }
            char link[768];
            snprintf(link, sizeof link, "%s/%s", by, de->d_name);
            char target[512];
            ssize_t n = readlink(link, target, sizeof target - 1);
            if (n <= 0) {
                continue;
            }
            target[n] = '\0';
            /* the target is ../../sda1 style: make it absolute */
            char abs[768];
            if (target[0] == '/') {
                snprintf(abs, sizeof abs, "%s", target);
            } else {
                char *slash = strrchr(link, '/');
                if (slash != NULL) {
                    *slash = '\0';
                    ta_join(abs, sizeof abs, link, target);
                } else {
                    continue;
                }
            }
            /* LEXICALLY normalize: the target is ../../sdb1 style;
             * collapse the dot-dot segments so the comparison against
             * the mountinfo's absolute source works (real /dev/disk
             * symlinks are exactly this shape). */
            char norm[768];
            size_t nw = 0;
            char seg[768];
            snprintf(seg, sizeof seg, "%s", abs);
            for (char *tok = strtok(seg, "/"); tok != NULL;
                 tok = strtok(NULL, "/")) {
                if (strcmp(tok, ".") == 0) {
                    continue;
                }
                if (strcmp(tok, "..") == 0) {
                    while (nw > 0 && norm[nw - 1] != '/') {
                        nw--;
                    }
                    if (nw > 0) {
                        nw--; /* drop the '/' */
                    }
                    continue;
                }
                nw += (size_t)snprintf(norm + nw,
                                       sizeof norm - nw, "/%s",
                                       tok);
                if (nw >= sizeof norm) {
                    nw = sizeof norm - 1;
                }
            }
            if (strcmp(norm, source) == 0) {
                snprintf(out, cap, "%s", de->d_name);
                closedir(d);
                return;
            }
        }
        closedir(d);
    }
    /* the mount point's last component; "/" itself stays "/" */
    const char *b = strrchr(mountpoint, '/');
    if (mountpoint[0] == '/' && mountpoint[1] == '\0') {
        snprintf(out, cap, "File System");
        return;
    }
    ta_copy(out, cap,
            (b != NULL && b[1] != '\0') ? b + 1 : mountpoint);
}

void sb_model_devices(sb_model *m, const char *mountinfo_path,
                      const char *label_root) {
    /* File System first (the spec's order) */
    sb_row *r = &m->rows[m->count++];
    memset(r, 0, sizeof *r);
    r->section = SB_SECTION_DEVICES;
    snprintf(r->label, sizeof r->label, "File System");
    snprintf(r->path, sizeof r->path, "/");
    snprintf(r->icon, sizeof r->icon, "drive-harddisk");
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        r->has_usage = true;
        unsigned long long total =
            (unsigned long long)vfs.f_frsize * vfs.f_blocks;
        unsigned long long free_ =
            (unsigned long long)vfs.f_frsize * vfs.f_bavail;
        r->total_bytes = total;
        r->used_bytes = total > free_ ? total - free_ : 0;
    }

    FILE *f = fopen(mountinfo_path != NULL ? mountinfo_path
                                           : "/proc/self/mountinfo",
                    "r");
    if (f == NULL) {
        return;
    }
    char line[2048];
    while (fgets(line, sizeof line, f) != NULL && m->count < 64) {
        char mp[512], fs[64], src[512];
        if (!sb_parse_mountinfo_line(line, mp, sizeof mp, fs,
                                     sizeof fs, src, sizeof src)) {
            continue;
        }
        if (strcmp(mp, "/") == 0) {
            continue; /* already listed as File System */
        }
        if (!sb_fs_is_real(fs, mp)) {
            continue;
        }
        r = &m->rows[m->count++];
        memset(r, 0, sizeof *r);
        r->section = SB_SECTION_DEVICES;
        {
            char label[256]; /* bounded through the helper */
            device_label(src, mp, label_root, label, sizeof label);
            ta_copy(r->label, sizeof r->label, label);
        }
        snprintf(r->path, sizeof r->path, "%s", mp);
        r->network = strncmp(fs, "nfs", 3) == 0 ||
                     strncmp(fs, "cifs", 4) == 0 ||
                     strncmp(fs, "smb", 3) == 0 ||
                     strstr(fs, "sshfs") != NULL;
        bool removable = strncmp(mp, "/run/media/", 11) == 0;
        if (!removable && src[0] == '/') {
            /* /sys/block/<dev>/removable == 1 */
            char *dev = strrchr(src, '/');
            if (dev != NULL) {
                dev++;
                /* strip partition suffix: sda1 -> sda, nvme0n1p2 ->
                 * nvme0n1 (the kernel's disk-level dirs) */
                char disk[64];
                snprintf(disk, sizeof disk, "%s", dev);
                size_t dl = strlen(disk);
                if (dl > 0 && (disk[dl - 1] >= '0' &&
                               disk[dl - 1] <= '9')) {
                    if (disk[dl - 2] == 'p' &&
                        ((disk[dl - 3] >= '0' &&
                          disk[dl - 3] <= '9') ||
                         (dl >= 3 && disk[0] == 'n'))) {
                        disk[dl - 2] = '\0'; /* nvme0n1p2 */
                    } else {
                        disk[dl - 1] = '\0'; /* sda1 */
                    }
                }
                char rpath[256];
                snprintf(rpath, sizeof rpath,
                         "/sys/block/%s/removable", disk);
                FILE *rf = fopen(rpath, "r");
                if (rf != NULL) {
                    int v = fgetc(rf);
                    fclose(rf);
                    removable = (v == '1');
                }
            }
        }
        r->removable = removable;
        if (strcmp(fs, "iso9660") == 0 || strcmp(fs, "udf") == 0) {
            snprintf(r->icon, sizeof r->icon, "drive-optical");
        } else if (r->network) {
            snprintf(r->icon, sizeof r->icon, "network-server");
        } else if (removable) {
            snprintf(r->icon, sizeof r->icon,
                     "drive-removable-media");
        } else {
            snprintf(r->icon, sizeof r->icon, "drive-harddisk");
        }
        if (statvfs(mp, &vfs) == 0) {
            r->has_usage = true;
            unsigned long long total =
                (unsigned long long)vfs.f_frsize * vfs.f_blocks;
            unsigned long long free_ =
                (unsigned long long)vfs.f_frsize * vfs.f_bavail;
            r->total_bytes = total;
            r->used_bytes = total > free_ ? total - free_ : 0;
        }
    }
    fclose(f);
}

size_t sb_model_build(sb_model *m, const char *mountinfo_path,
                      const char *label_root) {
    m->count = 0;
    sb_model_places(m);
    sb_model_bookmarks(m);
    sb_model_devices(m, mountinfo_path, label_root);
    return m->count;
}
