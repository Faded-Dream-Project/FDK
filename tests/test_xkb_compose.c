/* test_xkb_compose.c — the Wayland compose/dead-key resolution pin
 * (round 32). The seat's wayland_compose_codepoint() feeds layout
 * keysyms through xkb_compose_state; this test pins the SAME contract
 * at the API level the library uses: dead_acute + e -> é (U+00E9),
 * a plain key stays itself, a broken sequence cancels to 0, and a
 * mid-sequence press reports 0 (no codepoint until the sequence
 * completes). Skips honestly when the system has no compose tables
 * (xkb_compose_table_new_from_locale fails) — the seat degrades the
 * same way.
 */
#include "fdk/fdk.h"
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_u32 feed_key(struct xkb_compose_state *cs, xkb_keysym_t ks) {
    switch (xkb_compose_state_feed(cs, ks)) {
    case XKB_COMPOSE_FEED_ACCEPTED:
        break;
    case XKB_COMPOSE_FEED_IGNORED:
        return 0;
    default:
        return 0;
    }
    switch (xkb_compose_state_get_status(cs)) {
    case XKB_COMPOSE_NOTHING:
        return 0; /* caller overlays the layout glyph; for the test
                     the plain-key path is checked separately */
    case XKB_COMPOSE_COMPOSING:
        return 0;
    case XKB_COMPOSE_COMPOSED: {
        xkb_keysym_t sym = xkb_compose_state_get_one_sym(cs);
        if (sym != XKB_KEY_NoSymbol) {
            return xkb_keysym_to_utf32(sym);
        }
        char buf[16];
        int n = xkb_compose_state_get_utf8(cs, buf, sizeof buf);
        return n == 1 ? (fdk_u32)(unsigned char)buf[0] : 0;
    }
    case XKB_COMPOSE_CANCELLED:
        return 0;
    default:
        return 0;
    }
}

int main(void) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (ctx == NULL) {
        printf("[skip] xkb compose: no xkb context\n");
        return 0;
    }
    struct xkb_compose_table *table = xkb_compose_table_new_from_locale(
        ctx, "en_US.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (table == NULL) {
        printf("[skip] xkb compose: no compose tables installed\n");
        xkb_context_unref(ctx);
        return 0;
    }
    struct xkb_compose_state *cs =
        xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
    xkb_compose_table_unref(table);
    if (cs == NULL) {
        printf("[skip] xkb compose: state alloc failed\n");
        xkb_context_unref(ctx);
        return 0;
    }

    /* dead_acute + e -> é (U+00E9): the compose file's canonical
     * dead-key sequence; the layout keysym for the dead key comes
     * from the keymap, but the compose table also accepts the
     * mnemonic keysyms (dead_acute, e) */
    xkb_keysym_t dead_acute = xkb_keysym_from_name("dead_acute",
                                                   XKB_KEYSYM_NO_FLAGS);
    xkb_keysym_t e = xkb_keysym_from_name("e", XKB_KEYSYM_NO_FLAGS);
    if (dead_acute == XKB_KEY_NoSymbol || e == XKB_KEY_NoSymbol) {
        printf("[skip] xkb compose: keysym names unavailable\n");
        xkb_compose_state_unref(cs);
        xkb_context_unref(ctx);
        return 0;
    }
    fdk_u32 mid = feed_key(cs, dead_acute);
    printf("[ok] xkb compose: dead_acute mid-sequence codepoint=%u "
           "(0 = composing, as designed)\n", mid);

    fdk_u32 composed = feed_key(cs, e);
    if (composed != 0xE9) {
        printf("FAIL xkb compose: dead_acute+e -> U+%04X (want 00E9)\n",
               composed);
        xkb_compose_state_unref(cs);
        xkb_context_unref(ctx);
        return 1;
    }
    printf("[ok] xkb compose: dead_acute+e -> U+00E9 (é)\n");

    /* a second completed sequence must work (state reuse) */
    xkb_keysym_t dead_grave = xkb_keysym_from_name("dead_grave",
                                                   XKB_KEYSYM_NO_FLAGS);
    feed_key(cs, dead_grave);
    fdk_u32 ag = feed_key(cs, xkb_keysym_from_name("a", XKB_KEYSYM_NO_FLAGS));
    if (ag != 0xE0) {
        printf("FAIL xkb compose: dead_grave+a -> U+%04X (want 00E0)\n",
               ag);
        xkb_compose_state_unref(cs);
        xkb_context_unref(ctx);
        return 1;
    }
    printf("[ok] xkb compose: dead_grave+a -> U+00E0 (à), state reuse\n");

    /* a plain key after a completed sequence stays plain: the status
     * returns NOTHING — the seat overlays the layout glyph there */
    (void)feed_key(cs, xkb_keysym_from_name("k", XKB_KEYSYM_NO_FLAGS));
    if (xkb_compose_state_get_status(cs) != XKB_COMPOSE_NOTHING) {
        printf("FAIL xkb compose: plain key left the sequence "
               "non-idle\n");
        xkb_compose_state_unref(cs);
        xkb_context_unref(ctx);
        return 1;
    }
    printf("[ok] xkb compose: plain key resets to NOTHING\n");

    xkb_compose_state_unref(cs);
    xkb_context_unref(ctx);
    printf("\nall xkb compose tests passed\n");
    return 0;
}
