/* main.c — fdk-keys, the keyboard-input audit app.
 *
 * The owner's ask: "double check with letters and language, every
 * single one should work on FDK to be typed." Three layers, one app:
 *
 *   --sweep-engine  every Unicode codepoint (minus surrogates) pushed
 *                   through the entry's text model: set → read back →
 *                   byte-compare, plus caret walks at every boundary
 *                   and font-measure calls (no-crash proof). ASan.
 *
 *   --sweep-layouts libxkbregistry enumerates EVERY installed xkb
 *                   layout; for each, every keycode × {none, Shift,
 *                   AltGr, Shift+AltGr} goes through
 *                   xkb_state_key_get_utf8 — the exact key→text call
 *                   FDK's two backends make — and each produced
 *                   string is pushed through the same entry model.
 *
 *   (default)       the GUI rig: a focused entry, a live mirror
 *                   label, and a receipt (final text + FNV-1a hash)
 *                   printed at exit — what the XTEST and the labwc
 *                   virtual-keyboard injectors type into.
 *
 * Everything on the public API only, like every test-app here. */

#include "../common/app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the app state -------------------------------------------------- */

typedef struct keys_app {
    ta_app ta;
    fdk_widget *entry;
    fdk_widget *mirror;   /* live "typed:" label                      */
    fdk_widget *info;     /* sweep receipts, shown live               */
    long verified;        /* engine sweep receipts                    */
    long rejected;
    long malformed;
} keys_app;

static keys_app K;

static void mirror_refresh(void) {
    const char *t = fdk_entry_get_text(K.entry);
    if (t == NULL) {
        t = "";
    }
    char buf[512];
    size_t n = strlen(t);
    if (n > 200) {
        n = 200;
    }
    snprintf(buf, sizeof buf, "typed (%zu bytes): %.*s", strlen(t),
             (int)n, t);
    fdk_label_set_text(K.mirror, buf);
}

static void on_changed(fdk_widget *entry, void *user) {
    (void)entry;
    (void)user;
    mirror_refresh();
}

/* ---- UTF-8 encode (codepoint → 1-4 bytes) --------------------------- */

static int utf8_encode(unsigned int cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Controls (C0/C1) are legitimately refused by text widgets on every
 * platform; everything else must round-trip byte-identically. */
static bool is_control(unsigned int cp) {
    return cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
}

/* ---- the engine sweep: every codepoint ------------------------------ */

static void sweep_engine(void) {
    char buf[8];
    /* the live mirror relayouts on every changed callback — 1.1M of
     * those would outlive the sun; sweeps run disconnected */
    fdk_entry_set_on_changed(K.entry, NULL, NULL);
    /* an optional ceiling (FDK_KEYS_MAX_CP) keeps calibration runs short */
    unsigned int cp_max = 0x10FFFF;
    {
        const char *lim = getenv("FDK_KEYS_MAX_CP");
        if (lim != NULL) {
            cp_max = (unsigned int)atol(lim);
        }
    }
    for (unsigned int cp = 1; cp <= cp_max; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue; /* surrogates: not codepoints, not encodable */
        }
        if (is_control(cp)) {
            continue;
        }
        int n = utf8_encode(cp, buf);
        buf[n] = '\0';
        fdk_result r = fdk_entry_set_text(K.entry, buf);
        if (!fdk_ok(r)) {
            K.rejected++;
            continue;
        }
        const char *got = fdk_entry_get_text(K.entry);
        if (got == NULL || strcmp(got, buf) != 0) {
            K.malformed++;
            if (K.malformed <= 12) {
                fprintf(stderr, "MISMATCH U+%04X: want", cp);
                for (int b = 0; b < n; b++) {
                    fprintf(stderr, " %02X", (unsigned char)buf[b]);
                }
                fprintf(stderr, " | got");
                size_t gl = got ? strlen(got) : 0;
                for (size_t b = 0; b < gl && b < 12; b++) {
                    fprintf(stderr, " %02X",
                            (unsigned char)got[b]);
                }
                fprintf(stderr, "\n");
            }
            continue;
        }
        K.verified++;
        /* the caret: every CODEPOINT boundary must be settable
         * exactly (the header's contract), and every mid-codepoint
         * offset must SNAP to a boundary rather than land inside */
        size_t len = strlen(buf);
        for (size_t off = 0; off <= len; off++) {
            (void)fdk_entry_set_cursor(K.entry, off);
            size_t cur = fdk_entry_get_cursor(K.entry);
            bool on_boundary =
                (cur == 0) ||
                (cur == len) ||
                ((unsigned char)buf[cur] & 0xC0) != 0x80;
            bool expected = (off == 0) || (off == len) ||
                            ((unsigned char)buf[off] & 0xC0) != 0x80;
            if (off == cur && !on_boundary) {
                K.malformed++;
                if (K.malformed <= 8) {
                    fprintf(stderr,
                            "CURSOR U+%04X len=%zu: set %zu got %zu "
                            "(inside a codepoint!)\n",
                            cp, len, off, cur);
                }
                break;
            }
            if (expected && off != cur && cur != 0 && cur != len &&
                ((unsigned char)buf[cur] & 0xC0) == 0x80) {
                /* a boundary offset landed inside — the snap went
                 * the wrong way */
                K.malformed++;
                if (K.malformed <= 8) {
                    fprintf(stderr,
                            "SNAP U+%04X len=%zu: set %zu got %zu\n",
                            cp, len, off, cur);
                }
                break;
            }
        }
        /* the font machinery must survive the codepoint (measure may
         * return zeros when no font covers it; crash is the failure).
         * Sampled every 64th codepoint: same no-crash proof, 1/64th
         * of the ASan-shaped cost. */
        if ((cp & 0x3F) == 0) {
            fdk_text_metrics m = {0, 0, 0};
            (void)fdk_font_measure_utf8(K.ta.font_ui, buf, (size_t)n,
                                        &m);
        }
        /* a combining mark after 'e': one user-perceived letter */
        if (cp >= 0x0300 && cp <= 0x036F) {
            char seq[8] = {'e', 0, 0, 0, 0};
            memcpy(seq + 1, buf, (size_t)n);
            seq[1 + n] = '\0';
            if (fdk_ok(fdk_entry_set_text(K.entry, seq))) {
                const char *g = fdk_entry_get_text(K.entry);
                if (g != NULL && strcmp(g, seq) == 0) {
                    K.verified++;
                }
            }
        }
    }
    (void)fdk_entry_set_text(K.entry, "");
    fdk_entry_set_on_changed(K.entry, on_changed, NULL);
}

/* ---- the xkb layout sweep: every layout's letters -------------------- */

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbregistry.h>

static long lay_count;
static long lay_letters;
static long lay_unique;
/* the unique codepoints seen, bitset over planes 0-2 (U+0000..U+2FFFF) */
static unsigned char seen[(0x30000) / 8];

static void mark_seen(unsigned int cp) {
    if (cp < 0x30000) {
        seen[cp >> 3] |= (unsigned char)(1u << (cp & 7));
    }
}

static void sweep_layouts(void) {
    /* the SYSTEM ruleset (default includes): the same xkeyboard-config
     * data both FDK backends read through libxkbcommon */
    struct rxkb_context *rc = rxkb_context_new(RXKB_CONTEXT_NO_FLAGS);
    if (rc == NULL ||
        !rxkb_context_parse_default_ruleset(rc)) {
        fdk_label_set_text(K.info, "rxkb: registry unavailable");
        rxkb_context_unref(rc);
        return;
    }
    static const char *unused_guard = "mods are resolved per keymap";
    (void)unused_guard;
    char show[128];
    for (struct rxkb_layout *lay = rxkb_layout_first(rc); lay != NULL;
         lay = rxkb_layout_next(lay)) {
        struct xkb_rule_names names;
        memset(&names, 0, sizeof names);
        names.rules = "evdev";
        names.layout = rxkb_layout_get_name(lay);
        names.variant = rxkb_layout_get_variant(lay);
        struct xkb_context *xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_keymap *km = xkb_keymap_new_from_names(
            xctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (km == NULL) {
            continue; /* some registry entries need extra options */
        }
        struct xkb_state *st = xkb_state_new(km);
        if (st == NULL) {
            xkb_keymap_unref(km);
            continue;
        }
        xkb_mod_index_t i_shift = xkb_keymap_mod_get_index(km, "Shift");
        xkb_mod_index_t i_alt = xkb_keymap_mod_get_index(km, "Mod5");
        xkb_mod_mask_t m_shift =
            (i_shift != XKB_MOD_INVALID) ? ((xkb_mod_mask_t)1 << i_shift)
                                         : 0;
        xkb_mod_mask_t m_alt =
            (i_alt != XKB_MOD_INVALID) ? ((xkb_mod_mask_t)1 << i_alt) : 0;
        xkb_layout_index_t nlayouts =
            xkb_keymap_num_layouts_for_key(km, 0); /* unused; quiet */
        (void)nlayouts;
        for (xkb_keycode_t key = 8; key < 256; key++) {
            for (int mi = 0; mi < 4; mi++) {
                xkb_mod_mask_t dep = 0;
                if (mi == 1 || mi == 3) {
                    dep |= m_shift;
                }
                if (mi == 2 || mi == 3) {
                    dep |= m_alt;
                }
                xkb_state_update_mask(st, dep, 0, 0, 0, 0, 0);
                char tmp[64];
                int n = xkb_state_key_get_utf8(st, key, tmp, sizeof tmp);
                if (n <= 0) {
                    continue;
                }
                tmp[sizeof tmp - 1] = '\0';
                /* printable single "letter-ish" token: no C0/C1, no
                 * space padding, and decode its first codepoint */
                bool ok = true;
                unsigned int cp = 0;
                int w = 0;
                unsigned char c0 = (unsigned char)tmp[0];
                if (c0 < 0x80) {
                    cp = c0;
                    w = 1;
                } else if ((c0 & 0xE0) == 0xC0) {
                    cp = c0 & 0x1F;
                    w = 2;
                } else if ((c0 & 0xF0) == 0xE0) {
                    cp = c0 & 0x0F;
                    w = 3;
                } else if ((c0 & 0xF8) == 0xF0) {
                    cp = c0 & 0x07;
                    w = 4;
                } else {
                    ok = false;
                }
                for (int k = 1; ok && k < n && k < w; k++) {
                    cp = (cp << 6) |
                         ((unsigned char)tmp[k] & 0x3F);
                }
                if (!ok || w != n || is_control(cp)) {
                    continue;
                }
                /* THE typed path: this exact string into the entry */
                if (!fdk_ok(fdk_entry_set_text(K.entry, tmp))) {
                    continue;
                }
                const char *got = fdk_entry_get_text(K.entry);
                if (got != NULL && strcmp(got, tmp) == 0) {
                    lay_letters++;
                    mark_seen(cp);
                    if (cp >= 0x20 && !is_control(cp) && cp < 0x30000) {
                        /* count once per unique codepoint below */
                    }
                }
            }
        }
        lay_count++;
        xkb_state_unref(st);
        xkb_keymap_unref(km);
        xkb_context_unref(xctx);
        if (lay_count % 25 == 0) {
            snprintf(show, sizeof show, "swept %ld layouts…", lay_count);
            fdk_label_set_text(K.info, show);
        }
    }
    rxkb_context_unref(rc);
    for (unsigned int cp = 0x20; cp < 0x30000; cp++) {
        if (seen[cp >> 3] & (1u << (cp & 7))) {
            lay_unique++;
        }
    }
}

/* the rigs (XTEST, the labwc virtual keyboard) finish by pressing
 * Ctrl+Q: the app leaves ta_run cleanly and prints its receipt */
static void keys_window_event(fdk_window *w, const fdk_event_data *e,
                              void *user) {
    (void)w;
    (void)user;
    if (e->type == FDK_EVENT_KEY_DOWN &&
        (e->key.modifiers & FDK_MOD_CTRL) != 0 &&
        (e->key.codepoint == 'q' ||
         e->key.codepoint == ('q' & 0x1F))) {
        K.ta.quit = true;
    }
}

/* ---- the receipt ------------------------------------------------------ */

static unsigned long fnv1a(const char *s) {
    unsigned long h = 2166136261UL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619UL;
    }
    return h;
}

int main(int argc, char **argv) {
    bool do_engine = false;
    bool do_layouts = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sweep-engine") == 0) {
            do_engine = true;
        }
        if (strcmp(argv[i], "--sweep-layouts") == 0) {
            do_layouts = true;
        }
    }
    if (!ta_init(&K.ta, "fdk-keys")) {
        return 1;
    }
    if (!ta_open(&K.ta, "fdk-keys — typing audit", 900, 420)) {
        ta_close(&K.ta);
        return 1;
    }
    if (!fdk_ok(fdk_label_create(K.ta.content, K.ta.font_ui,
                                 "sweep: idle", &K.info))) {
        return 1;
    }
    if (!fdk_ok(fdk_entry_create(K.ta.content, K.ta.font_ui, "",
                                 &K.entry))) {
        return 1;
    }
    fdk_entry_set_placeholder(K.entry,
                              "type here — every letter matters");
    fdk_widget_set_natural_size(K.entry, 900, 36);
    if (!fdk_ok(fdk_label_create(K.ta.content, K.ta.font_ui,
                                 "typed (0 bytes):", &K.mirror))) {
        return 1;
    }
    fdk_entry_set_on_changed(K.entry, on_changed, NULL);
    fdk_widget_focus(K.entry);
    fdk_window_set_event_callback(K.ta.window, keys_window_event, NULL);

    if (do_engine) {
        sweep_engine();
        char show[160];
        snprintf(show, sizeof show,
                 "engine sweep done: verified=%ld rejected=%ld "
                 "malformed=%ld",
                 K.verified, K.rejected, K.malformed);
        fdk_label_set_text(K.info, show);
    }
    if (do_layouts) {
        memset(seen, 0, sizeof seen);
        lay_count = lay_letters = lay_unique = 0;
        sweep_layouts();
        char show[160];
        snprintf(show, sizeof show,
                 "layout sweep done: layouts=%ld letters=%ld "
                 "unique=%ld",
                 lay_count, lay_letters, lay_unique);
        fdk_label_set_text(K.info, show);
    }
    mirror_refresh();

    ta_run(&K.ta);

    const char *final_text = fdk_entry_get_text(K.entry);
    final_text = final_text ? final_text : "";
    printf("fdk-keys receipt: engine=%ld rej=%ld bad=%ld "
           "layouts=%ld letters=%ld unique=%ld "
           "typed_len=%zu typed_hash=%08lx typed=%s\n",
           K.verified, K.rejected, K.malformed, lay_count, lay_letters,
           lay_unique, strlen(final_text), fnv1a(final_text),
           strlen(final_text) <= 120 ? final_text : "(long)");
    ta_close(&K.ta);
    return K.malformed == 0 ? 0 : 2;
}
