/* trash.c — see trash.h. */
#include "trash.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

static void data_home(char *out, size_t cap) {
    const char *x = getenv("XDG_DATA_HOME");
    if (x != NULL && x[0] == '/') {
        snprintf(out, cap, "%s", x);
        return;
    }
    const char *home = getenv("HOME");
    if (home == NULL) {
        home = "/";
    }
    snprintf(out, cap, "%s/.local/share", home);
}

bool trash_dir(char *out, size_t cap) {
    char dh[FI_PATH_MAX];
    data_home(dh, sizeof dh);
    snprintf(out, cap, "%s/Trash", dh);
    char sub[FI_PATH_MAX];
    snprintf(sub, sizeof sub, "%s/files", out);
    if (mkdir(out, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    if (mkdir(sub, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    snprintf(sub, sizeof sub, "%s/info", out);
    if (mkdir(sub, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

/* RFC 2396 escaping for the Path= value (the spec's raw-bytes +
 * URL-escape rule; we escape the reserved set conservatively). */
static void uri_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)in;
         *s != '\0' && o + 4 < cap; s++) {
        unsigned char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '.' ||
            c == '-' || c == '_') {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, cap - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

static void uri_unescape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *s = in; *s != '\0' && o + 1 < cap; s++) {
        if (s[0] == '%' && s[1] != '\0' && s[2] != '\0') {
            char hex[3] = {s[1], s[2], 0};
            out[o++] = (char)strtoul(hex, NULL, 16);
            s += 2;
        } else {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
}

static void unique_stem(const char *base, char *out, size_t cap) {
    /* strip the .trashinfo-safe stem: foo -> foo, foo.2, foo.3... */
    char files_dir[FI_JOIN_MAX];
    if (!trash_dir(files_dir, sizeof files_dir)) {
        snprintf(out, cap, "%s", base);
        return;
    }
    snprintf(out, cap, "%s", base);
    for (int attempt = 2; attempt < 10000; attempt++) {
        char probe[FI_JOIN_MAX * 2];
        fi_join2(probe, sizeof probe, files_dir, "files", out);
        struct stat st;
        if (stat(probe, &st) != 0) {
            char info_probe[FI_JOIN_MAX * 2];
            fi_join3(info_probe, sizeof info_probe, files_dir, "info",
                     out, ".trashinfo");
            if (stat(info_probe, &st) != 0) {
                return; /* neither side exists: unique */
            }
        }
        snprintf(out, cap, "%s.%d", base, attempt);
    }
}

static int copy_stream(int in_fd, int out_fd) {
    char buf[65536];
    for (;;) {
        ssize_t r = read(in_fd, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return 0;
        }
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out_fd, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            off += w;
        }
    }
}

static bool copy_any(const char *src, const char *dst,
                     char *err, size_t cap) {
    struct stat st;
    if (stat(src, &st) != 0) {
        snprintf(err, cap, "stat: %s", strerror(errno));
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) != 0 && errno != EEXIST) {
            snprintf(err, cap, "mkdir: %s", strerror(errno));
            return false;
        }
        DIR *d = opendir(src);
        if (d == NULL) {
            snprintf(err, cap, "opendir: %s", strerror(errno));
            return false;
        }
        struct dirent *de;
        bool ok = true;
        while (ok && (de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0) {
                continue;
            }
            char s2[FI_PATH_MAX], d2[FI_PATH_MAX];
            snprintf(s2, sizeof s2, "%s/%s", src, de->d_name);
            snprintf(d2, sizeof d2, "%s/%s", dst, de->d_name);
            ok = copy_any(s2, d2, err, cap);
        }
        closedir(d);
        return ok;
    }
    int in = open(src, O_RDONLY);
    if (in < 0) {
        snprintf(err, cap, "open %s: %s", src, strerror(errno));
        return false;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL,
                   st.st_mode & 07777);
    if (out < 0) {
        close(in);
        snprintf(err, cap, "create %s: %s", dst, strerror(errno));
        return false;
    }
    bool ok = copy_stream(in, out) == 0;
    if (ok) {
        /* preserve mtime */
        struct timespec times[2] = {
            st.st_atim, st.st_mtim,
        };
        (void)futimens(out, times);
    }
    close(in);
    close(out);
    if (!ok) {
        unlink(dst);
        snprintf(err, cap, "copy %s: read/write failed", src);
    }
    return ok;
}

static bool move_any(const char *src, const char *dst,
                     char *err, size_t cap) {
    if (rename(src, dst) == 0) {
        return true;
    }
    if (errno != EXDEV) {
        snprintf(err, cap, "rename: %s", strerror(errno));
        return false;
    }
    if (!copy_any(src, dst, err, cap)) {
        return false;
    }
    /* remove the source tree */
    struct stat st;
    if (stat(src, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(src);
        if (d != NULL) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 ||
                    strcmp(de->d_name, "..") == 0) {
                    continue;
                }
                char s2[FI_JOIN_MAX];
                snprintf(s2, sizeof s2, "%s/%s", src, de->d_name);
                (void)move_any(s2, "/dev/null", err, cap);
            }
            closedir(d);
        }
        rmdir(src);
    } else {
        unlink(src);
    }
    return true;
}

bool trash_file(const char *path, char *err, size_t cap) {
    char tdir[FI_PATH_MAX];
    if (!trash_dir(tdir, sizeof tdir)) {
        snprintf(err, cap, "cannot create the trash directory");
        return false;
    }

    const char *base = strrchr(path, '/');
    base = (base != NULL) ? base + 1 : path;
    char stem[FI_NAME_MAX];
    unique_stem(base, stem, sizeof stem);

    /* the .trashinfo FIRST, atomically: tmp + rename */
    char info_path[FI_JOIN_MAX * 2], tmp_path[FI_JOIN_MAX * 2];
    fi_join3(info_path, sizeof info_path, tdir, "info", stem,
             ".trashinfo");
    snprintf(tmp_path, sizeof tmp_path, "%.4000s.tmp", info_path);

    char escaped[FI_JOIN_MAX * 3];
    uri_escape(path, escaped, sizeof escaped);
    char now[40];
    time_t t = time(NULL);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    strftime(now, sizeof now, "%Y-%m-%dT%H:%M:%S", &tm_buf);

    FILE *f = fopen(tmp_path, "w");
    if (f == NULL) {
        snprintf(err, cap, "write %s: %s", tmp_path, strerror(errno));
        return false;
    }
    fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", escaped,
            now);
    fclose(f);
    if (rename(tmp_path, info_path) != 0) {
        snprintf(err, cap, "rename info: %s", strerror(errno));
        unlink(tmp_path);
        return false;
    }

    char dst[FI_JOIN_MAX * 2];
    fi_join2(dst, sizeof dst, tdir, "files", stem);
    if (!move_any(path, dst, err, cap)) {
        unlink(info_path);
        return false;
    }
    return true;
}

size_t trash_list(trash_entry **out_entries) {
    *out_entries = NULL;
    char tdir[FI_PATH_MAX];
    if (!trash_dir(tdir, sizeof tdir)) {
        return 0;
    }
    char info_dir[FI_JOIN_MAX];
    snprintf(info_dir, sizeof info_dir, "%s/info", tdir);
    DIR *d = opendir(info_dir);
    if (d == NULL) {
        return 0;
    }
    trash_entry *v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 11 ||
            strcmp(de->d_name + nl - 10, ".trashinfo") != 0) {
            continue;
        }
        char path[FI_JOIN_MAX];
        snprintf(path, sizeof path, "%.4000s/%.180s", info_dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            trash_entry *g = realloc(v, cap * sizeof *v);
            if (g == NULL) {
                break;
            }
            v = g;
        }
        trash_entry *e = &v[n];
        memset(e, 0, sizeof *e);
        char stem[FI_NAME_MAX];
        snprintf(stem, sizeof stem, "%.*s", (int)(nl - 10),
                 de->d_name);
        snprintf(e->name, sizeof e->name, "%s", stem);
        char line[FI_PATH_MAX * 3 + 32];
        bool in_group = false;
        while (fgets(line, sizeof line, f) != NULL) {
            if (strncmp(line, "[Trash Info]", 12) == 0) {
                in_group = true;
                continue;
            }
            if (!in_group) {
                continue;
            }
            if (strncmp(line, "Path=", 5) == 0) {
                char raw[FI_JOIN_MAX * 3];
                snprintf(raw, sizeof raw, "%s", line + 5);
                char *nl2 = strpbrk(raw, "\r\n");
                if (nl2 != NULL) {
                    *nl2 = '\0';
                }
                uri_unescape(raw, e->orig_path, sizeof e->orig_path);
            } else if (strncmp(line, "DeletionDate=", 13) == 0) {
                snprintf(e->deleted, sizeof e->deleted, "%.19s",
                         line + 13);
                char *nl2 = strpbrk(e->deleted, "\r\n");
                if (nl2 != NULL) {
                    *nl2 = '\0';
                }
            }
        }
        fclose(f);
        n++;
    }
    closedir(d);
    *out_entries = v;
    return n;
}

void trash_list_free(trash_entry *entries) {
    free(entries);
}

bool trash_restore(const char *trash_name, char *err, size_t cap) {
    char tdir[FI_PATH_MAX];
    if (!trash_dir(tdir, sizeof tdir)) {
        snprintf(err, cap, "no trash dir");
        return false;
    }
    trash_entry *all = NULL;
    size_t n = trash_list(&all);
    const trash_entry *found = NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(all[i].name, trash_name) == 0) {
            found = &all[i];
            break;
        }
    }
    if (found == NULL) {
        trash_list_free(all);
        snprintf(err, cap, "no info entry for %s", trash_name);
        return false;
    }
    char src[FI_JOIN_MAX];
    snprintf(src, sizeof src, "%s/files/%s", tdir, trash_name);
    char target[FI_JOIN_MAX];
    snprintf(target, sizeof target, "%s", found->orig_path);
    trash_list_free(all);

    /* if the target exists, pick a unique sibling name */
    struct stat st;
    if (stat(target, &st) == 0) {
        char dir[FI_JOIN_MAX];
        snprintf(dir, sizeof dir, "%s", target);
        char *slash = strrchr(dir, '/');
        const char *base = "restored";
        if (slash != NULL) {
            *slash = '\0';
            base = slash + 1;
        } else {
            snprintf(dir, sizeof dir, ".");
        }
        for (int k = 2; k < 1000; k++) {
            char probe[FI_JOIN_MAX * 2];
            snprintf(probe, sizeof probe, "%.4000s/%.200s (%d)",
                     dir, base, k);
            if (stat(probe, &st) != 0) {
                snprintf(target, sizeof target, "%.4000s", probe);
                break;
            }
        }
    }
    if (!move_any(src, target, err, cap)) {
        return false;
    }
    char info_path[FI_JOIN_MAX];
    snprintf(info_path, sizeof info_path, "%s/info/%s.trashinfo",
             tdir, trash_name);
    unlink(info_path);
    return true;
}

static off_t tree_size(const char *path) {
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
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char sub[FI_JOIN_MAX];
        snprintf(sub, sizeof sub, "%.4000s/%.180s", path, de->d_name);
        total += tree_size(sub);
    }
    closedir(d);
    return total;
}

off_t trash_size(void) {
    char tdir[FI_PATH_MAX];
    if (!trash_dir(tdir, sizeof tdir)) {
        return 0;
    }
    char files_dir[FI_JOIN_MAX];
    snprintf(files_dir, sizeof files_dir, "%s/files", tdir);
    return tree_size(files_dir);
}

bool trash_delete(const char *trash_name, char *err, size_t cap) {
    (void)err;
    (void)cap;
    char tdir[FI_PATH_MAX];
    if (!trash_dir(tdir, sizeof tdir)) {
        return false;
    }
    char path[FI_JOIN_MAX];
    snprintf(path, sizeof path, "%s/files/%s", tdir, trash_name);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d != NULL) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 ||
                    strcmp(de->d_name, "..") == 0) {
                    continue;
                }
                char sub[FI_JOIN_MAX];
                snprintf(sub, sizeof sub, "%.4000s/%.180s", path, de->d_name);
                (void)trash_delete(sub, err, cap);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
    char info_p[FI_JOIN_MAX * 2];
    fi_join3(info_p, sizeof info_p, tdir, "info", trash_name,
             ".trashinfo");
    unlink(info_p);
    return true;
}

bool trash_empty(char *err, size_t cap) {
    trash_entry *all = NULL;
    size_t n = trash_list(&all);
    for (size_t i = 0; i < n; i++) {
        if (!trash_delete(all[i].name, err, cap)) {
            trash_list_free(all);
            return false;
        }
    }
    trash_list_free(all);
    return true;
}
