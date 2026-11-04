/* store.c — fdk-notes' model: notebooks and notes in flat arrays,
 * persisted through fdk_prefs as one JSON-free "key=v\n" blob per
 * note (prefs are the only persistence FDK offers an app, and the
 * honest scope for round 21: a real notes app would use files, but
 * the README's target is the FDK surface, and prefs exercise it
 * exactly). */
#include "notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

nt_app napp;

nt_note *nt_note_at(size_t index) {
    return (index < napp.note_count) ? &napp.notes[index] : NULL;
}

nt_note *nt_note_current(void) {
    return nt_note_at(napp.current);
}

bool nt_book_add(const char *name) {
    if (napp.book_count >= NT_MAX_NOTEBOOKS || name == NULL ||
        name[0] == '\0') {
        return false;
    }
    nt_notebook *b = &napp.books[napp.book_count];
    snprintf(b->name, sizeof b->name, "%.60s", name);
    b->first_note = napp.note_count;
    b->note_count = 0;
    napp.book_count++;
    return true;
}

bool nt_note_add(size_t book, const char *title) {
    if (book >= napp.book_count || napp.note_count >= NT_MAX_NOTES) {
        return false;
    }
    nt_notebook *b = &napp.books[book];
    /* the flat array keeps each notebook's run contiguous: the
     * notes after this book's run shift up one slot                */
    memmove(&napp.notes[b->first_note + b->note_count + 1],
            &napp.notes[b->first_note + b->note_count],
            (napp.note_count - (b->first_note + b->note_count)) *
                sizeof(nt_note));
    nt_note *n = &napp.notes[b->first_note + b->note_count];
    memset(n, 0, sizeof *n);
    snprintf(n->title, sizeof n->title, "%.90s",
             (title != NULL && title[0] != '\0') ? title
                                                 : "Untitled note");
    n->notebook = book;
    for (size_t i = book + 1; i < napp.book_count; i++) {
        napp.books[i].first_note++;
    }
    b->note_count++;
    napp.note_count++;
    return true;
}

/* ---- persistence: one pref key per field ---------------------------- */

static void esc(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 4 < cap; p++) {
        if (*p == '\n') {
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (*p == '\\') {
            out[o++] = '\\';
            out[o++] = '\\';
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static void unesc(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 1 < cap; p++) {
        if (*p == '\\' && p[1] == 'n') {
            out[o++] = '\n';
            p++;
        } else if (*p == '\\' && p[1] == '\\') {
            out[o++] = '\\';
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

void nt_save_all(void) {
    if (napp.base.prefs == NULL) {
        return;
    }
    char key[64], val[NT_BODY_MAX + 16];
    (void)fdk_prefs_set_int(napp.base.prefs, "notes.books",
                            (int)napp.book_count);
    for (size_t b = 0; b < napp.book_count; b++) {
        snprintf(key, sizeof key, "book.%zu.name", b);
        (void)fdk_prefs_set(napp.base.prefs, key,
                                   napp.books[b].name);
    }
    (void)fdk_prefs_set_int(napp.base.prefs, "notes.count",
                            (int)napp.note_count);
    for (size_t i = 0; i < napp.note_count; i++) {
        const nt_note *n = &napp.notes[i];
        snprintf(key, sizeof key, "note.%zu.book", i);
        (void)fdk_prefs_set_int(napp.base.prefs, key,
                                (int)n->notebook);
        snprintf(key, sizeof key, "note.%zu.title", i);
        char t[NT_TITLE_MAX * 2];
        esc(n->title, t, sizeof t);
        (void)fdk_prefs_set(napp.base.prefs, key, t);
        snprintf(key, sizeof key, "note.%zu.body", i);
        esc(n->body, val, sizeof val);
        (void)fdk_prefs_set(napp.base.prefs, key, val);
        snprintf(key, sizeof key, "note.%zu.flags", i);
        (void)fdk_prefs_set_int(napp.base.prefs, key,
                                (n->bold ? 1 : 0) +
                                    (n->italic ? 2 : 0));
        snprintf(key, sizeof key, "note.%zu.rem", i);
        (void)fdk_prefs_set_int(napp.base.prefs, key,
                                (int)n->reminder_at);
    }
}

void nt_load_all(void) {
    if (napp.base.prefs == NULL) {
        return;
    }
    int books = ta_pref_int(&napp.base, "notes.books", -1);
    if (books <= 0) {
        return; /* nothing saved (or the old shape): start fresh    */
    }
    char key[64], name[64];
    for (int b = 0; b < books && napp.book_count < NT_MAX_NOTEBOOKS;
         b++) {
        snprintf(key, sizeof key, "book.%d.name", b);
        ta_pref_str(&napp.base, key, "", name, sizeof name);
        if (name[0] == '\0') {
            snprintf(name, sizeof name, "Notebook %d", b + 1);
        }
        (void)nt_book_add(name);
    }
    int count = ta_pref_int(&napp.base, "notes.count", 0);
    for (int i = 0;
         i < count && napp.note_count < NT_MAX_NOTES; i++) {
        int book = ta_pref_int(&napp.base, "note.i.book", 0);
        snprintf(key, sizeof key, "note.%d.book", i);
        book = ta_pref_int(&napp.base, key, 0);
        if (book < 0 || (size_t)book >= napp.book_count) {
            book = 0;
        }
        snprintf(key, sizeof key, "note.%d.title", i);
        char t[NT_TITLE_MAX * 2], title[NT_TITLE_MAX];
        ta_pref_str(&napp.base, key, "", t, sizeof t);
        unesc(t, title, sizeof title);
        if (!nt_note_add((size_t)book, title)) {
            break;
        }
        nt_note *n = nt_note_current();
        snprintf(key, sizeof key, "note.%d.body", i);
        char raw[NT_BODY_MAX + 16];
        ta_pref_str(&napp.base, key, "", raw, sizeof raw);
        unesc(raw, n->body, sizeof n->body);
        n->body_len = strlen(n->body);
        snprintf(key, sizeof key, "note.%d.flags", i);
        int flags = ta_pref_int(&napp.base, key, 0);
        n->bold = (flags & 1) != 0;
        n->italic = (flags & 2) != 0;
        snprintf(key, sizeof key, "note.%d.rem", i);
        n->reminder_at =
            (long)ta_pref_int(&napp.base, key, 0);
        n->reminder_fired = false;
    }
    napp.current = 0;
}

/* the reminder sweep: fires each due note once (the notification
 * rides ui.c's lane; here only the state turns over)              */
void nt_reminder_check(void) {
    time_t now = time(NULL);
    for (size_t i = 0; i < napp.note_count; i++) {
        nt_note *n = &napp.notes[i];
        if (n->reminder_at > 0 && !n->reminder_fired &&
            (long)now >= n->reminder_at) {
            n->reminder_fired = true;
        }
    }
}
