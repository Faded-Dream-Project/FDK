/* fsops.c — see fsops.h. */
#include "fsops.h"
#include "mime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* a bounded error-string setter: the op's err field is 256 bytes
 * and the sources are paths up to FI_JOIN_MAX — truncate once,
 * here, where the bound is stated. */

/* ---- shared copy engine (also used by trash.c via move_any) ------ */


/* ---- listing ------------------------------------------------------ */

static int fi_cmp(const void *a, const void *b) {
    const file_info *fa = a, *fb = b;
    if (fa->is_dir != fb->is_dir) {
        return fa->is_dir ? -1 : 1;
    }
    return strcasecmp(fa->name, fb->name);
}

file_info *fs_list_dir(const char *dir, bool hidden, size_t *count) {
    *count = 0;
    DIR *d = opendir(dir);
    if (d == NULL) {
        return NULL;
    }
    file_info *v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' &&
            (strcmp(de->d_name, ".") == 0 ||
             strcmp(de->d_name, "..") == 0)) {
            continue;
        }
        if (!hidden && de->d_name[0] == '.') {
            continue;
        }
        if (strlen(de->d_name) >= FI_NAME_MAX) {
            continue; /* pathological: skipped, not crashed */
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            file_info *g = realloc(v, cap * sizeof *g);
            if (g == NULL) {
                break;
            }
            v = g;
        }
        file_info *fi = &v[n];
        memset(fi, 0, sizeof *fi);
        snprintf(fi->name, sizeof fi->name, "%s", de->d_name);
        snprintf(fi->path, sizeof fi->path, "%s/%s", dir,
                 de->d_name);
        fi->is_hidden = de->d_name[0] == '.';
        n++;
    }
    closedir(d);

    /* stat + mime pass (the 50k gate: one stat + one glob match per
     * entry, no magic sniff unless the glob fails) */
    for (size_t i = 0; i < n; i++) {
        fs_stat_entry(&v[i]);
        if (!v[i].is_dir) {
            char icon[48];
            mime_of(v[i].name, v[i].path, v[i].mime,
                    sizeof v[i].mime, icon, sizeof icon);
            snprintf(v[i].icon, sizeof v[i].icon, "%s", icon);
        } else {
            snprintf(v[i].mime, sizeof v[i].mime, "inode/directory");
            snprintf(v[i].icon, sizeof v[i].icon, "folder");
        }
    }

    /* an EMPTY directory leaves v == NULL (nothing was ever
     * allocated) and n == 0 — qsort(NULL, 0, ...) is UB by the
     * library contract even though it is a no-op in practice; the
     * §31 rig caught it booting into a fresh empty $HOME. */
    if (n > 0) {
        qsort(v, n, sizeof *v, fi_cmp);
    }
    *count = n;
    return v;
}

void fs_stat_entry(file_info *fi) {
    struct stat st;
    if (stat(fi->path, &st) != 0) {
        fi->is_dir = false;
        fi->size = 0;
        fi->mtime = 0;
        return;
    }
    fi->is_dir = S_ISDIR(st.st_mode);
    fi->is_link = S_ISLNK(st.st_mode);
    fi->size = st.st_size;
    fi->mtime = st.st_mtime;
    fi->uid = st.st_uid;
    fi->gid = st.st_gid;
    fi->mode = st.st_mode;
    fi->is_executable =
        (st.st_mode & S_IXUSR) != 0 && !S_ISDIR(st.st_mode);
}

off_t fs_tree_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return st.st_size;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return 0;
    }
    off_t total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' &&
            (strcmp(de->d_name, ".") == 0 ||
             strcmp(de->d_name, "..") == 0)) {
            continue;
        }
        char sub[FI_PATH_MAX];
        snprintf(sub, sizeof sub, "%.3800s/%.200s", path, de->d_name);
        total += fs_tree_size(sub);
    }
    closedir(d);
    return total;
}

/* ---- mounts -------------------------------------------------------- */

static void add_mount(mount_info **v, size_t *n, size_t *cap,
                      const char *dir, const char *dev,
                      const char *fstype) {
    /* skip the pseudo filesystems a file manager never shows */
    static const char *const SKIP[] = {
        "/proc", "/sys", "/dev", "/run", "/tmp", "/var/lib/nfs",
        "/proc/sys/fs/binfmt_misc", "/dev/pts", "/dev/shm",
    };
    for (size_t i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++) {
        if (strcmp(dir, SKIP[i]) == 0) {
            return;
        }
    }
    if (strncmp(fstype, "proc", 4) == 0 ||
        strncmp(fstype, "sysfs", 5) == 0 ||
        strncmp(fstype, "devpts", 6) == 0 ||
        strncmp(fstype, "tmpfs", 5) == 0 ||
        strncmp(fstype, "cgroup", 6) == 0 ||
        strncmp(fstype, "securityfs", 10) == 0 ||
        strncmp(fstype, "pstore", 6) == 0 ||
        strncmp(fstype, "bpf", 3) == 0 ||
        strncmp(fstype, "tracefs", 7) == 0 ||
        strncmp(fstype, "debugfs", 7) == 0 ||
        strncmp(fstype, "configfs", 8) == 0 ||
        strncmp(fstype, "fusectl", 7) == 0 ||
        strncmp(fstype, "mqueue", 6) == 0 ||
        strncmp(fstype, "hugetlbfs", 9) == 0 ||
        strncmp(fstype, "binfmt_misc", 11) == 0 ||
        strncmp(fstype, "ramfs", 5) == 0) {
        return;
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        mount_info *g = realloc(*v, *cap * sizeof **v);
        if (g == NULL) {
            return;
        }
        *v = g;
    }
    mount_info *m = &(*v)[(*n)++];
    memset(m, 0, sizeof *m);
    snprintf(m->dir, sizeof m->dir, "%.4000s", dir);
    snprintf(m->dev, sizeof m->dev, "%.120s", dev);
    snprintf(m->fstype, sizeof m->fstype, "%s", fstype);

    struct statvfs vfs;
    if (statvfs(dir, &vfs) == 0) {
        m->total = (off_t)vfs.f_blocks * (off_t)vfs.f_frsize;
        m->free_bytes = (off_t)vfs.f_bavail * (off_t)vfs.f_frsize;
    }
}

size_t fs_mounts(mount_info **out) {
    *out = NULL;
    mount_info *v = NULL;
    size_t n = 0, cap = 0;

    /* mountinfo field 4 is the root inside the fs, field 5 the
     * mount point (escaped as \040), field 9 the source, field 10
     * the fstype. */
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL) {
        f = fopen("/proc/mounts", "r");
        if (f == NULL) {
            return 0;
        }
        char dev[128], dir[FI_PATH_MAX], fstype[32];
        while (fscanf(f, "%127s %4095s %31s %*s %*d %*d", dev, dir,
                      fstype) == 3) {
            add_mount(&v, &n, &cap, dir, dev, fstype);
        }
        fclose(f);
        *out = v;
        return n;
    }
    char line[2048];
    while (fgets(line, sizeof line, f) != NULL) {
        char root[512], dir_esc[FI_PATH_MAX], dev[256], fstype[64];
        int mount_id, parent_id, maj, min;
        if (sscanf(line, "%d %d %d:%d %511s %4095s", &mount_id,
                   &parent_id, &maj, &min, root, dir_esc) != 6) {
            continue;
        }
        /* the fstype is the field after the optional per-mount
         * options: separator " - " then maj:min fstype source */
        char *sep = strstr(line, " - ");
        if (sep == NULL) {
            continue;
        }
        int maj2, min2;
        if (sscanf(sep + 3, "%d:%d %63s %255s", &maj2, &min2,
                   fstype, dev) != 4) {
            continue;
        }
        /* unescape \040 (space) in the mount point */
        char dir[FI_PATH_MAX];
        size_t o = 0;
        for (const char *s = dir_esc;
             *s != '\0' && o + 1 < sizeof dir; s++) {
            if (s[0] == '\\' && s[1] == '0' && s[2] == '4' &&
                s[3] == '0') {
                dir[o++] = ' ';
                s += 3;
            } else {
                dir[o++] = *s;
            }
        }
        dir[o] = '\0';
        add_mount(&v, &n, &cap, dir, dev, fstype);
    }
    fclose(f);

    /* /run/media/USER: removable mounts many desktops create */
    const char *user = getenv("USER");
    if (user != NULL) {
        char base[FI_PATH_MAX];
        snprintf(base, sizeof base, "/run/media/%s", user);
        DIR *d = opendir(base);
        if (d != NULL) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') {
                    continue;
                }
                char dir[FI_PATH_MAX];
                snprintf(dir, sizeof dir, "%.3800s/%.180s", base, de->d_name);
                add_mount(&v, &n, &cap, dir, de->d_name, "removable");
            }
            closedir(d);
        }
    }

    *out = v;
    return n;
}

void fs_mounts_free(mount_info *m) {
    free(m);
}

/* ---- inotify ------------------------------------------------------- */

int fs_watch_start(const char *dir) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    int wd = inotify_add_watch(
        fd, dir, IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
                     IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
    if (wd < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int fs_watch_poll(int fd) {
    char buf[8192] __attribute__((aligned(8)));
    ssize_t r = read(fd, buf, sizeof buf);
    if (r <= 0) {
        return (r == 0) ? 0 : ((errno == EAGAIN || errno == EWOULDBLOCK)
                                   ? 0
                                   : -1);
    }
    int changes = 0;
    ssize_t off = 0;
    while (off + (ssize_t)sizeof(struct inotify_event) <= r) {
        const struct inotify_event *ev =
            (const struct inotify_event *)(buf + off);
        off += (ssize_t)(sizeof(struct inotify_event) + ev->len);
        changes++;
    }
    return changes;
}

void fs_watch_stop(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

/* ---- the sliced operation engine ----------------------------------
 *
 * A depth-first work stack of (path, is_dir, op) items. Each slice
 * copies/deletes a bounded number of payload bytes, so the pump
 * stays responsive; the progress dialog drives the slices from a
 * timer and reads done/total for its bar. */

typedef enum work_kind { WORK_READ_DIR, WORK_COPY_FILE, WORK_UNLINK,
                         WORK_RMDIR } work_kind;

typedef struct work_item {
    work_kind kind;
    char src[FI_JOIN_MAX];
    char dst[FI_JOIN_MAX];
    off_t size;
    /* the in-flight copy's fds (WORK_COPY_FILE mid-file) */
    int in_fd, out_fd;
    off_t copied;
} work_item;

struct fi_stack_impl {
    work_item *items;
    size_t count, cap;
};

static void stack_push(void *sp, const work_item *it) {
    struct fi_stack_impl *s = sp;
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        work_item *g = realloc(s->items, s->cap * sizeof *g);
        if (g == NULL) {
            return;
        }
        s->items = g;
    }
    s->items[s->count++] = *it;
}

static bool stack_pop(void *sp, work_item *out) {
    struct fi_stack_impl *s = sp;
    if (s->count == 0) {
        return false;
    }
    *out = s->items[--s->count];
    return true;
}

fi_op *fi_op_start(fi_op_kind kind, const char *src, const char *dst) {
    fi_op *op = calloc(1, sizeof *op);
    if (op == NULL) {
        return NULL;
    }
    op->kind = kind;
    snprintf(op->src, sizeof op->src, "%s", src);
    if (dst != NULL) {
        snprintf(op->dst, sizeof op->dst, "%s", dst);
    }
    struct fi_stack_impl *s = calloc(1, sizeof *s);
    op->stack = s;
    work_item root;
    memset(&root, 0, sizeof root);
    snprintf(root.src, sizeof root.src, "%s", src);
    if (dst != NULL) {
        snprintf(root.dst, sizeof root.dst, "%s", dst);
    }
    root.kind = WORK_READ_DIR;
    root.in_fd = -1;
    root.out_fd = -1;
    struct stat st;
    if (stat(src, &st) == 0) {
        root.size = S_ISDIR(st.st_mode) ? 0 : st.st_size;
        if (!S_ISDIR(st.st_mode)) {
            root.kind = (kind == FI_OP_DELETE) ? WORK_UNLINK
                                               : WORK_COPY_FILE;
        }
    }
    stack_push(s, &root);
    op->total_files = 1;
    return op;
}

bool fi_op_step(fi_op *op) {
    struct fi_stack_impl *s = op->stack;
    work_item it;
    if (!stack_pop(s, &it)) {
        return false; /* finished */
    }

    switch (it.kind) {
    case WORK_READ_DIR: {
        struct stat st;
        if (stat(it.src, &st) != 0 || !S_ISDIR(st.st_mode)) {
            {
                char e[256];
                snprintf(e, sizeof e, "stat: %.180s",
                         strerror(errno));
                snprintf(op->err, sizeof op->err, "%s", e);
            }
            op->failed = true;
            return false;
        }
        if (op->kind != FI_OP_DELETE) {
            if (mkdir(it.dst, st.st_mode & 07777) != 0 &&
                errno != EEXIST) {
                {
                char e[128];
                snprintf(e, sizeof e, "mkdir: %.90s",
                         strerror(errno));
                snprintf(op->err, sizeof op->err, "%s", e);
            }
                op->failed = true;
                return false;
            }
        }
        DIR *d = opendir(it.src);
        if (d == NULL) {
            {
                char e[128];
                snprintf(e, sizeof e, "opendir: %.90s",
                         strerror(errno));
                snprintf(op->err, sizeof op->err, "%s", e);
            }
            op->failed = true;
            return false;
        }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0) {
                continue;
            }
            work_item child;
            memset(&child, 0, sizeof child);
            snprintf(child.src, sizeof child.src,
                     "%.4000s/%.200s", it.src, de->d_name);
            child.kind = WORK_READ_DIR;
            child.in_fd = -1;
            child.out_fd = -1;
            if (op->kind != FI_OP_DELETE) {
                snprintf(child.dst, sizeof child.dst,
                         "%.4000s/%.200s", it.dst, de->d_name);
            }
            struct stat cst;
            if (stat(child.src, &cst) == 0) {
                if (S_ISDIR(cst.st_mode)) {
                    stack_push(s, &child);
                } else {
                    child.kind = (op->kind == FI_OP_DELETE)
                                     ? WORK_UNLINK
                                     : WORK_COPY_FILE;
                    child.size = cst.st_size;
                    stack_push(s, &child);
                    op->total_files++;
                    op->total_bytes += cst.st_size;
                }
            }
        }
        closedir(d);
        if (op->kind == FI_OP_DELETE) {
            work_item rm = it;
            rm.kind = WORK_RMDIR;
            stack_push(s, &rm);
        }
        return true;
    }
    case WORK_COPY_FILE: {
        if (it.in_fd < 0) {
            it.in_fd = open(it.src, O_RDONLY);
            if (it.in_fd < 0) {
                {
                char e[128];
                snprintf(e, sizeof e, "open: %.90s",
                         strerror(errno));
                snprintf(op->err, sizeof op->err, "%s", e);
            }
                op->failed = true;
                return false;
            }
            it.out_fd = open(it.dst,
                             O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (it.out_fd < 0) {
                close(it.in_fd);
                it.in_fd = -1;
                {
                char e[128];
                snprintf(e, sizeof e, "create: %.90s",
                         strerror(errno));
                snprintf(op->err, sizeof op->err, "%s", e);
            }
                op->failed = true;
                return false;
            }
            snprintf(op->cur_file, sizeof op->cur_file, "%.4000s",
                     it.src);
        }
        /* bounded payload slice */
        char buf[262144];
        ssize_t r = read(it.in_fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) {
                r = 0;
            } else {
                {
                char e[128];
                snprintf(e, sizeof e, "read: %.90s",
                         strerror(errno));
                snprintf(op->err, sizeof op->err, "%s", e);
            }
                op->failed = true;
                close(it.in_fd);
                close(it.out_fd);
                return false;
            }
        }
        if (r > 0) {
            ssize_t off = 0;
            while (off < r) {
                ssize_t w = write(it.out_fd, buf + off,
                                  (size_t)(r - off));
                if (w < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    snprintf(op->err, sizeof op->err, "write: %s",
                             strerror(errno));
                    op->failed = true;
                    close(it.in_fd);
                    close(it.out_fd);
                    return false;
                }
                off += w;
            }
            it.copied += r;
            op->done_bytes += r;
            stack_push(s, &it); /* resume the same file next slice */
            return true;
        }
        /* finished this file */
        close(it.in_fd);
        close(it.out_fd);
        op->done_files++;
        if (op->kind == FI_OP_MOVE) {
            unlink(it.src);
        }
        return true;
    }
    case WORK_UNLINK:
        if (unlink(it.src) != 0) {
            {
                char e[128];
                snprintf(e, sizeof e, "unlink: %.90s",
                         strerror(errno));
                snprintf(op->err, sizeof op->err, "%s", e);
            }
            op->failed = true;
            return false;
        }
        op->done_files++;
        return true;
    case WORK_RMDIR:
        if (rmdir(it.src) != 0 && errno != ENOTEMPTY) {
            {
                char e[128];
                snprintf(e, sizeof e, "rmdir: %.90s",
                         strerror(errno));
                snprintf(op->err, sizeof op->err, "%s", e);
            }
            op->failed = true;
            return false;
        }
        return true;
    }
    return false;
}

void fi_op_free(fi_op *op) {
    if (op == NULL) {
        return;
    }
    struct fi_stack_impl *s = op->stack;
    if (s != NULL) {
        for (size_t i = 0; i < s->count; i++) {
            if (s->items[i].in_fd >= 0) {
                close(s->items[i].in_fd);
            }
            if (s->items[i].out_fd >= 0) {
                close(s->items[i].out_fd);
            }
        }
        free(s->items);
        free(s);
    }
    free(op);
}

bool fi_conflict(const char *dst, bool *dst_dir) {
    struct stat st;
    if (stat(dst, &st) != 0) {
        return false;
    }
    if (dst_dir != NULL) {
        *dst_dir = S_ISDIR(st.st_mode);
    }
    return true;
}

void fi_unique_sibling(const char *dst, char *out, size_t cap) {
    struct stat st;
    if (stat(dst, &st) != 0) {
        snprintf(out, cap, "%s", dst);
        return;
    }
    char dir[FI_JOIN_MAX];
    fi_copy(dir, sizeof dir, dst);
    char *slash = strrchr(dir, '/');
    const char *base = "item";
    if (slash != NULL) {
        *slash = '\0';
        base = slash + 1;
    } else {
        snprintf(dir, sizeof dir, ".");
    }
    for (int k = 2; k < 10000; k++) {
        char probe[FI_JOIN_MAX * 2];
        snprintf(probe, sizeof probe, "%.4000s/%.200s (%d)", dir, base, k);
        if (stat(probe, &st) != 0) {
            snprintf(out, cap, "%s", probe);
            return;
        }
    }
    snprintf(out, cap, "%s", dst);
}
