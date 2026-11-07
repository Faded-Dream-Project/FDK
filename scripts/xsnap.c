/* xsnap.c — round 14 rig tool: capture an X window to a binary PPM
 * (P6). Window selection: -w 0x... by id, else -t <title-substring>
 * (case-insensitive), else the first mapped toplevel under bare
 * Xvfb there is exactly one client anyway. Build:
 *   cc -O1 -I/tmp/x11dev/usr/include -o /tmp/xsnap scripts/xsnap.c \
 *      -L/tmp/x11dev/usr/lib/x86_64-linux-gnu -lX11 \
 *      -Wl,-rpath,/tmp/x11dev/usr/lib/x86_64-linux-gnu
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h> /* XGetPixel/XDestroyImage/fetch */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Window find_window(Display *d, const char *title_sub, Window want) {
    if (want != None) {
        return want;
    }
    Window root = DefaultRootWindow(d);
    Window dummy, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(d, root, &dummy, &dummy, &kids, &n) || kids == NULL) {
        return None;
    }
    Window best = None;
    long best_area = -1;
    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(d, kids[i], &wa) ||
            wa.map_state != IsViewable || wa.width < 8 || wa.height < 8) {
            continue;
        }
        if (title_sub != NULL) {
            char *name = NULL;
            if (XFetchName(d, kids[i], &name) && name != NULL) {
                char a[256], b[256];
                size_t j;
                for (j = 0; name[j] != '\0' && j < sizeof a - 1; j++) {
                    a[j] = (char)tolower((unsigned char)name[j]);
                }
                a[j] = '\0';
                for (j = 0; title_sub[j] != '\0' && j < sizeof b - 1; j++) {
                    b[j] = (char)tolower((unsigned char)title_sub[j]);
                }
                b[j] = '\0';
                XFree(name);
                if (strstr(a, b) != NULL) {
                    best = kids[i];
                    break;
                }
            } else if (name != NULL) {
                XFree(name);
            }
            continue;
        }
        long area = (long)wa.width * (long)wa.height;
        if (area > best_area) {
            best_area = area;
            best = kids[i];
        }
    }
    XFree(kids);
    return best;
}

int main(int argc, char **argv) {
    const char *dpy_name = NULL, *title_sub = NULL, *out_path = NULL;
    Window want = None;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dpy_name = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            title_sub = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            want = (Window)strtoul(argv[++i], NULL, 0);
        } else {
            out_path = argv[i];
        }
    }
    if (out_path == NULL) {
        fprintf(stderr, "usage: xsnap [-d DISP] [-w 0xID | -t title] "
                        "out.ppm\n");
        return 2;
    }
    Display *d = XOpenDisplay(dpy_name);
    if (d == NULL) {
        fprintf(stderr, "xsnap: cannot open display\n");
        return 2;
    }
    Window w = find_window(d, title_sub, want);
    if (w == None) {
        fprintf(stderr, "xsnap: no window found\n");
        return 2;
    }
    XWindowAttributes wa;
    if (!XGetWindowAttributes(d, w, &wa)) {
        fprintf(stderr, "xsnap: attributes failed\n");
        return 2;
    }
    int x = 0, y = 0;
    Window child;
    XTranslateCoordinates(d, w, DefaultRootWindow(d), 0, 0, &x, &y, &child);
    fprintf(stderr, "xsnap: window 0x%lx at +%d+%d %dx%d\n",
            (unsigned long)w, x, y, wa.width, wa.height);
    XImage *img = XGetImage(d, w, 0, 0, (unsigned)wa.width,
                            (unsigned)wa.height, AllPlanes, ZPixmap);
    if (img == NULL) {
        fprintf(stderr, "xsnap: XGetImage failed\n");
        return 2;
    }
    /* mask shifts */
    int rs = 0, gs = 0, bs = 0;
    unsigned long m = img->red_mask;
    while (m != 0 && (m & 1) == 0) { rs++; m >>= 1; }
    m = img->green_mask;
    while (m != 0 && (m & 1) == 0) { gs++; m >>= 1; }
    m = img->blue_mask;
    while (m != 0 && (m & 1) == 0) { bs++; m >>= 1; }

    FILE *f = fopen(out_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "xsnap: cannot write %s\n", out_path);
        return 2;
    }
    fprintf(f, "P6\n%ld %ld\n255\n", (long)img->width, (long)img->height);
    for (int py = 0; py < img->height; py++) {
        for (int px = 0; px < img->width; px++) {
            unsigned long pix = XGetPixel(img, px, py);
            unsigned char rgb[3];
            rgb[0] = (unsigned char)((pix & img->red_mask) >> rs);
            rgb[1] = (unsigned char)((pix & img->green_mask) >> gs);
            rgb[2] = (unsigned char)((pix & img->blue_mask) >> bs);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    XDestroyImage(img);
    XCloseDisplay(d);
    fprintf(stderr, "xsnap: wrote %s\n", out_path);
    return 0;
}
