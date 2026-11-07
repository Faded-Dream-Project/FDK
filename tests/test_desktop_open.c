/* test_desktop_open.c — fdk_desktop_open_uri/_path: the validation
 * ladder and the honest unsupported/ok split. A real OPEN cannot be
 * asserted headlessly (the desktop decides what happens after the
 * launch); what CAN be pinned is: garbage never reaches the launch,
 * and a valid URI on a machine with xdg-open reports OK (the
 * detached child is nobody to wait for). */
#include "fdk/fdk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_launches;

static void test_validation(void) {
    /* the garbage ladder: every one of these must be refused with
     * FDK_ERR_INVALID_ARGUMENT — BEFORE any PATH probe or fork. */
    assert(fdk_desktop_open_uri(NULL) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_desktop_open_uri("") == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_desktop_open_uri("-rf") == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_desktop_open_uri("--help") == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_desktop_open_uri("http://x/\tinline-tab")
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_desktop_open_uri("no-scheme-relative") ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_desktop_open_path(NULL) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_desktop_open_path("relative/path") ==
           FDK_ERR_INVALID_ARGUMENT);
    g_launches = 0;

    /* a valid URI: xdg-open present -> OK (launched detached, the
     * call returns at once); absent -> UNSUPPORTED. Both are honest
     * outcomes; anything else (ARGUMENT, PLATFORM) is a bug. */
    fdk_result r = fdk_desktop_open_uri("https://example.org/");
    assert(r == FDK_OK || r == FDK_ERR_UNSUPPORTED);
    if (r == FDK_OK) {
        g_launches++;
    }

    /* schemes of every flavor pass validation; the split is the
     * same OK/UNSUPPORTED pair */
    const char *ok_uris[] = {
        "http://a", "https://b/c?d=e", "mailto:u@h", "ftp://f",
        "file:///tmp/x", "man:gcc(1)", "x-scheme-thing:payload",
    };
    for (size_t i = 0; i < sizeof ok_uris / sizeof *ok_uris; i++) {
        fdk_result rr = fdk_desktop_open_uri(ok_uris[i]);
        assert(rr == FDK_OK || rr == FDK_ERR_UNSUPPORTED);
        if (rr == FDK_OK) {
            g_launches++;
        }
    }

    /* absolute path: percent-encoding of spaces happens internally;
     * the outcome pair is the same */
    fdk_result pr = fdk_desktop_open_path("/tmp");
    assert(pr == FDK_OK || pr == FDK_ERR_UNSUPPORTED);
    if (pr == FDK_OK) {
        g_launches++;
    }

    printf("[ok] desktop open: validation ladder + honest %s\n",
           g_launches > 0 ? "launch" : "unsupported");
}

int main(void) {
    test_validation();
    printf("all desktop-open tests passed\n");
    return 0;
}
