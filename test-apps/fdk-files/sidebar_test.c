/* sidebar_test.c — the §31/PART-5 model test: a REAL-LOOKING
 * fixture mountinfo (root btrfs, /home xfs, /boot/efi vfat, tmpfs,
 * proc, a /run/media/user/USB vfat, a docker overlay, an nfs
 * mount), the label tree, and the exact expected rows/order.
 * Standalone binary (make build/sidebar_test); no UI. */
#include "sidebar.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_root[256];
const char *g_root_home(void); /* defined below */

static void mk(const char *rel) {
    char p[512];
    snprintf(p, sizeof p, "%.250s/%.200s", g_root, rel);
    /* mkdir -p: the fixture nests (home/.config/...) */
    for (char *at = p + strlen(g_root) + 1; ; at++) {
        if (*at == '/' || *at == '\0') {
            char saved = *at;
            *at = '\0';
            mkdir(p, 0755);
            *at = saved;
            if (saved == '\0') {
                break;
            }
        }
    }
}

static void write_file(const char *rel, const char *content) {
    char p[512];
    snprintf(p, sizeof p, "%.250s/%.200s", g_root, rel);
    FILE *f = fopen(p, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

static void fixture(void) {
    char tmpl[] = "/tmp/fdk-sbtest-XXXXXX";
    char *d = mkdtemp(tmpl);
    assert(d != NULL);
    snprintf(g_root, sizeof g_root, "%s", d);

    /* the fixture HOME with the XDG dirs (some exist, some not) */
    mk("home");
    mk("home/Desktop");
    mk("home/Documents");
    mk("home/Downloads");
    /* NO Music, NO Pictures, NO Videos — the exist-check must drop
     * them */
    mk("home/.config");
    mk("home/.config/gtk-3.0");
    mk("home/.config/fdk-files");
    mk("home/.local");
    mk("home/.local/share");
    write_file("home/.config/user-dirs.dirs",
               "XDG_DESKTOP_DIR=\"$HOME/Desktop\"\n"
               "XDG_DOCUMENTS_DIR=\"$HOME/Documents\"\n"
               "XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n"
               "XDG_MUSIC_DIR=\"$HOME/Music\"\n"
               "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n"
               "XDG_VIDEOS_DIR=\"$HOME/Videos\"\n");

    /* the shared GTK bookmarks (two valid, one missing dir, one
     * comment) */
    {
        char bm[900];
        snprintf(bm, sizeof bm,
                 "# GTK bookmarks\n"
                 "file://%.250s/home/Documents Work\n"
                 "file://%.250s/home/Downloads\n"
                 "file:///gone/dir Nope\n",
                 g_root, g_root);
        write_file("home/.config/gtk-3.0/bookmarks", bm);
    }
    /* the own store (one more; the shared one dedupes against it).
     * The path must be FIXTURE-absolute: the exist-check stats the
     * real filesystem. */
    {
        char bm[600];
        snprintf(bm, sizeof bm, "%.250s/home/Desktop\n", g_root);
        write_file("home/.config/fdk-files/bookmarks", bm);
    }

    /* the fake /dev/disk tree: USB-Stick -> sdb1, Data -> sda2 */
    mk("dev");
    mk("dev/disk");
    mk("dev/disk/by-label");
    mk("dev/disk/by-partlabel");
    (void)system(NULL); /* silence -Wunused in some libcs */
    {
        char cmd[768];
        snprintf(cmd, sizeof cmd,
                 "ln -sf ../../sdb1 '%s/dev/disk/by-label/USB-Stick'",
                 g_root);
        (void)system(cmd);
        snprintf(cmd, sizeof cmd,
                 "ln -sf ../../sda2 '%s/dev/disk/by-label/Data'",
                 g_root);
        (void)system(cmd);
    }

    /* the fixture mountinfo: the §31 spec's exact scenario */
    {
        char mi[1600];
        snprintf(mi, sizeof mi,
            "30 0 0:26 / / rw,relatime - btrfs /dev/sda2 rw\n"
            "31 30 8:2 /home /home rw,relatime - xfs "
            "%.250s/dev/sda3 rw\n"
            "40 30 8:1 /boot/efi /boot/efi rw - vfat /dev/sda1 rw\n"
            "41 30 0:5 /run /run rw - tmpfs tmpfs rw\n"
            "42 30 0:6 / /proc rw - proc proc rw\n"
            "43 30 8:17 / /run/media/user/USB rw,nosuid - vfat "
            "%.250s/dev/sdb1 rw\n"
            "44 30 0:27 / /var/lib/docker/overlay2/x/merged rw - "
            "overlay overlay rw\n"
            "45 30 0:28 / /mnt/nas rw,relatime - nfs4 "
            "server:/export rw\n",
            g_root, g_root);
        write_file("mountinfo", mi);
    }
}

/* ---- the tests ------------------------------------------------------ */

static void test_places(void) {
    sb_model m;
    m.count = 0;
    setenv("HOME", g_root_home(), 1); /* set by main */
    sb_model_places(&m);
    /* Home + Desktop + Documents + Downloads + Trash = 5 rows;
     * Music/Pictures/Videos are MISSING dirs — dropped */
    assert(m.count == 5);
    assert(m.rows[0].section == SB_SECTION_PLACES);
    assert(strcmp(m.rows[0].label, "Home") == 0);
    assert(strcmp(m.rows[0].icon, "user-home") == 0);
    assert(strcmp(m.rows[1].label, "Desktop") == 0);
    assert(strcmp(m.rows[2].label, "Documents") == 0);
    assert(strcmp(m.rows[3].label, "Downloads") == 0);
    /* the XDG resolution honored the user-dirs.dirs paths */
    char want[512];
    snprintf(want, sizeof want, "%.250s/home/Desktop", g_root);
    assert(strcmp(m.rows[1].path, want) == 0);
    /* Trash is the LAST place, with the empty icon (the fixture
     * home has no .local/share/Trash/files) */
    assert(m.rows[4].trash);
    assert(strcmp(m.rows[4].icon, "user-trash") == 0);
    printf("[ok] sidebar model: places (XDG dirs exist-checked, "
           "trash placed HERE)\n");
}

static void test_bookmarks(void) {
    sb_model m;
    m.count = 0;
    sb_model_bookmarks(&m);
    /* own store (/home/Desktop) first, then the shared file's
     * Documents + Downloads; /gone/dir dropped (does not exist);
     * the comment dropped */
    char want0[512];
    snprintf(want0, sizeof want0, "%.250s/home/Desktop", g_root);
    /* The three expected rows, in order. Anything else is a bug —
     * name it loudly rather than adjusting the count. */
    char want1[512], want2[512];
    snprintf(want1, sizeof want1, "%.250s/home/Documents", g_root);
    snprintf(want2, sizeof want2, "%.250s/home/Downloads", g_root);
    const char *expect[] = {NULL, NULL, NULL};
    expect[0] = want0;
    expect[1] = want1;
    expect[2] = want2;
    if (m.count != 3) {
        for (size_t i = 0; i < m.count; i++) {
            fprintf(stderr, "  bookmarks row %zu: '%s' -> %s\n", i,
                    m.rows[i].label, m.rows[i].path);
        }
    }
    assert(m.count == 3);
    assert(strcmp(m.rows[0].path, expect[0]) == 0);
    assert(strcmp(m.rows[1].label, "Work") == 0);
    assert(strcmp(m.rows[1].path, want1) == 0);
    assert(strcmp(m.rows[2].path, want2) == 0);
    assert(strcmp(m.rows[0].icon, "folder") == 0);
    printf("[ok] sidebar model: bookmarks (own store + shared GTK "
           "file merged, deduped, exist-checked)\n");
}

static void test_devices(void) {
    char mi[512], lr[512];
    snprintf(mi, sizeof mi, "%s/mountinfo", g_root);
    snprintf(lr, sizeof lr, "%s/dev/disk", g_root);
    sb_model m;
    m.count = 0;
    sb_model_devices(&m, mi, lr);
    /* File System + /home + the USB + the nfs = 4;
     * /boot/efi (excluded prefix), tmpfs+proc (pseudo), docker
     * (excluded prefix) are NOT listed */
    if (m.count != 4) {
        for (size_t i = 0; i < m.count; i++) {
            fprintf(stderr, "  row %zu: %s -> %s\n", i,
                    m.rows[i].label, m.rows[i].path);
        }
    }
    assert(m.count == 4);
    /* row 0: File System, /, drive-harddisk, has usage */
    assert(strcmp(m.rows[0].label, "File System") == 0);
    assert(strcmp(m.rows[0].path, "/") == 0);
    assert(strcmp(m.rows[0].icon, "drive-harddisk") == 0);
    /* row 1: /home — xfs, label falls back to the mount point's
     * last component ("home") — no by-label entry matches sda3 */
    assert(strcmp(m.rows[1].path, "/home") == 0);
    assert(strcmp(m.rows[1].label, "home") == 0);
    /* row 2: the USB — LABEL RESOLVED from by-label (USB-Stick),
     * removable (under /run/media), drive-removable-media */
    assert(strcmp(m.rows[2].path, "/run/media/user/USB") == 0);
    assert(strcmp(m.rows[2].label, "USB-Stick") == 0);
    assert(m.rows[2].removable);
    assert(strcmp(m.rows[2].icon, "drive-removable-media") == 0);
    /* row 3: the nfs — network icon */
    assert(strcmp(m.rows[3].path, "/mnt/nas") == 0);
    assert(m.rows[3].network);
    assert(strcmp(m.rows[3].icon, "network-server") == 0);
    printf("[ok] sidebar model: devices (fixture mountinfo: 4 rows, "
           "pseudo/prefix/docker excluded, label resolution)\n");
}

/* the §31/5.5 line parser on its own, including \040 escapes */
static void test_parse_line(void) {
    char mp[512], fs[64], src[512];
    assert(sb_parse_mountinfo_line(
        "43 30 8:17 / /run/media/my\\040user/USB rw - vfat "
        "/dev/sdb1 rw",
        mp, sizeof mp, fs, sizeof fs, src, sizeof src));
    assert(strcmp(mp, "/run/media/my user/USB") == 0);
    assert(strcmp(fs, "vfat") == 0);
    assert(strcmp(src, "/dev/sdb1") == 0);
    /* too few fields */
    assert(!sb_parse_mountinfo_line("1 2 3:4 / /", mp, sizeof mp,
                                    fs, sizeof fs, src,
                                    sizeof src));
    printf("[ok] sidebar model: mountinfo line parse (fields, "
           "optional-field dash, \\040 escape)\n");
}

/* ---- helper the tests reach ----------------------------------------- */

static char g_home[256];
const char *g_root_home(void) {
    return g_home;
}

int main(void) {
    fixture();
    snprintf(g_home, sizeof g_home, "%.250s/home", g_root);
    setenv("HOME", g_home, 1);
    setenv("XDG_CONFIG_HOME", "", 1); /* the $HOME/.config path */

    test_parse_line();
    test_places();
    test_bookmarks();
    test_devices();

    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
    (void)system(cmd);
    printf("all sidebar-model tests passed\n");
    return 0;
}
