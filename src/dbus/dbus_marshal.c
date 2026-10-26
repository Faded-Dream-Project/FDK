/*
 * dbus_marshal.c — D-Bus wire marshaling (1.1.0)
 *
 * The byte-level truth of the module, implemented from the public
 * D-Bus Specification. Every rule below was additionally verified
 * against the reference implementation's own bytes: an LD_PRELOAD
 * capture rig (scripts/wirepeek.c, test-only) recorded what libdbus
 * actually puts on the socket for Hello / ListNames / GetNameOwner /
 * a variant argument / an int64 array, and the marshaler reproduces
 * those layouts exactly — see tests/test_dbus.c's pinned-byte
 * assertions (the 169-byte GetNameOwner frame among them).
 *
 * Layout rules (offsets are from the message's first byte):
 *   - fixed header: endianness, type, flags, version, body-length,
 *     serial, header-array-length (16 bytes; the array's CONTENTS
 *     begin at offset 16 — the length field at 12..15 is part of
 *     the fixed header, a subtlety that costs an afternoon if you
 *     double-count it);
 *   - the header array is an array of STRUCT(byte, VARIANT): each
 *     struct aligns to 8, the field code aligns to 1, the variant's
 *     signature aligns to 1, the variant's VALUE aligns to its own
 *     type;
 *   - the body begins at the next 8-boundary after the array;
 *   - alignment of a type: y=1 b=4 n=2 q=2 i=4 u=4 x=8 t=8 d=8
 *     s=4 o=4 g=1 v=1 a=4 (=8 {=8 (the VARIANT=1 and the
 *     empty-array-still-pads rules are the two everyone gets wrong
 *     from memory — both pinned by the captured bytes and the spec
 *     table);
 *   - an array marshals as u32 length (counting ELEMENT BYTES ONLY),
 *     then padding to the element type's alignment — required even
 *     for an empty array — then the elements;
 *   - a variant marshals its signature (no alignment) then its value
 *     (aligned to the value's own type);
 *   - no padding trails the final field of a message (the reference
 *     implementation sends a 169-byte message with a 25-byte body).
 *
 * FDK marshals little-endian ('l'); the reader accepts BOTH
 * endiannesses. Every read is bounds-checked, alignment-checked,
 * depth-capped, and grammar-validated: the reader treats input as
 * hostile (see dbus_internal.h's security posture).
 */

#define FDK_LOG_TAG "dbus"

#include "dbus/dbus_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- growable byte buffer ------------------------------------------------ */

int fdk__dbuf_reserve(fdk__dbuf *b, size_t extra) {
    if (b->ok != FDK_OK) {
        return b->ok;
    }
    if (extra > SIZE_MAX - b->len) {
        b->ok = FDK_ERR_LIMIT;
        return b->ok;
    }
    if (b->len + extra <= b->cap) {
        return FDK_OK;
    }
    size_t want = b->len + extra;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < want) {
        if (cap > SIZE_MAX / 2) {
            cap = want;
            break;
        }
        cap *= 2;
    }
    uint8_t *grown = fdk_realloc(b->data, cap);
    if (grown == NULL) {
        b->ok = FDK_ERR_OUT_OF_MEMORY;
        return b->ok;
    }
    b->data = grown;
    b->cap = cap;
    return FDK_OK;
}

int fdk__dbuf_put(fdk__dbuf *b, const void *bytes, size_t n) {
    int r = fdk__dbuf_reserve(b, n);
    if (r != FDK_OK) {
        return r;
    }
    if (n > 0) {
        memcpy(b->data + b->len, bytes, n);
        b->len += n;
    }
    return FDK_OK;
}

int fdk__dbuf_align(fdk__dbuf *b, size_t alignment) {
    if (b->ok != FDK_OK) {
        return b->ok;
    }
    /* alignment is 1/2/4/8 by construction */
    size_t pad = (b->len % alignment == 0) ? 0 : alignment - b->len % alignment;
    if (pad == 0) {
        return FDK_OK;
    }
    static const uint8_t zeros[8] = {0};
    return fdk__dbuf_put(b, zeros, pad);
}

void fdk__dbuf_reset(fdk__dbuf *b) {
    b->len = 0;
    b->ok = FDK_OK;
}

void fdk__dbuf_free(fdk__dbuf *b) {
    fdk_free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->ok = FDK_OK;
}

int fdk__dbuf_u16(fdk__dbuf *b, uint16_t v) {
    uint8_t tmp[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    return fdk__dbuf_put(b, tmp, 2);
}

int fdk__dbuf_u32(fdk__dbuf *b, uint32_t v) {
    uint8_t tmp[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                      (uint8_t)(v >> 24)};
    return fdk__dbuf_put(b, tmp, 4);
}

int fdk__dbuf_u64(fdk__dbuf *b, uint64_t v) {
    uint8_t tmp[8];
    for (int i = 0; i < 8; i++) {
        tmp[i] = (uint8_t)(v >> (8 * i));
    }
    return fdk__dbuf_put(b, tmp, 8);
}

int fdk__dbuf_double(fdk__dbuf *b, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    return fdk__dbuf_u64(b, bits);
}

int fdk__dbuf_string(fdk__dbuf *b, const char *s) {
    if (s == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    size_t n = strlen(s);
    if (n > UINT32_MAX) {
        return FDK_ERR_LIMIT;
    }
    int r = fdk__dbuf_align(b, 4);
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_u32(b, (uint32_t)n);
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_put(b, s, n + 1); /* bytes + NUL */
    return r;
}

int fdk__dbuf_signature(fdk__dbuf *b, const char *s) {
    if (s == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    size_t n = strlen(s);
    if (n > FDK_DBUS_SIGNATURE_MAX) {
        return FDK_ERR_LIMIT;
    }
    int r = fdk__dbuf_put(b, &(uint8_t){(uint8_t)n}, 1);
    if (r != FDK_OK) {
        return r;
    }
    return fdk__dbuf_put(b, s, n + 1);
}

/* ---- bounded reader ------------------------------------------------------ */

static void rd_fail(fdk__dreader *r, int code) {
    if (r->err == FDK_OK) {
        r->err = code;
    }
}

int fdk__rd_ok(const fdk__dreader *r) {
    return r->err == FDK_OK;
}

int fdk__rd_align(fdk__dreader *r, size_t alignment) {
    if (r->err != FDK_OK) {
        return r->err;
    }
    if (alignment <= 1) {
        return FDK_OK;
    }
    size_t pad = (r->pos % alignment == 0) ? 0
                                           : alignment - r->pos % alignment;
    if (pad > r->len - r->pos) {
        rd_fail(r, FDK_ERR_LIMIT); /* truncated padding */
        return r->err;
    }
    r->pos += pad;
    return FDK_OK;
}

static int rd_fixed(fdk__dreader *r, size_t size, size_t alignment,
                    uint8_t *out) {
    if (r->err != FDK_OK) {
        return r->err;
    }
    if (fdk__rd_align(r, alignment) != FDK_OK) {
        return r->err;
    }
    if (size > r->len - r->pos) {
        rd_fail(r, FDK_ERR_LIMIT);
        return r->err;
    }
    if (r->endian == 'l') {
        for (size_t i = 0; i < size; i++) {
            out[i] = r->data[r->pos + i];
        }
    } else {
        for (size_t i = 0; i < size; i++) {
            out[i] = r->data[r->pos + size - 1 - i];
        }
    }
    r->pos += size;
    return FDK_OK;
}

uint8_t fdk__rd_u8(fdk__dreader *r) {
    uint8_t v = 0;
    (void)rd_fixed(r, 1, 1, &v);
    return v;
}

uint16_t fdk__rd_u16(fdk__dreader *r) {
    uint8_t tmp[2] = {0, 0};
    (void)rd_fixed(r, 2, 2, tmp);
    return (uint16_t)(tmp[0] | ((uint16_t)tmp[1] << 8));
}

uint32_t fdk__rd_u32(fdk__dreader *r) {
    uint8_t tmp[4] = {0, 0, 0, 0};
    (void)rd_fixed(r, 4, 4, tmp);
    return (uint32_t)tmp[0] | ((uint32_t)tmp[1] << 8) |
           ((uint32_t)tmp[2] << 16) | ((uint32_t)tmp[3] << 24);
}

uint64_t fdk__rd_u64(fdk__dreader *r) {
    uint8_t tmp[8];
    memset(tmp, 0, sizeof(tmp));
    (void)rd_fixed(r, 8, 8, tmp);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | tmp[i];
    }
    return v;
}

double fdk__rd_double(fdk__dreader *r) {
    uint64_t bits = fdk__rd_u64(r);
    double v;
    memcpy(&v, &bits, 8);
    return v;
}

char *fdk__rd_string(fdk__dreader *r) {
    if (r->err != FDK_OK) {
        return NULL;
    }
    if (fdk__rd_align(r, 4) != FDK_OK) {
        return NULL;
    }
    uint32_t n = fdk__rd_u32(r);
    if (r->err != FDK_OK) {
        return NULL;
    }
    /* n + NUL must fit in what remains. */
    if (n > r->len - r->pos || r->len - r->pos - n < 1) {
        rd_fail(r, FDK_ERR_LIMIT);
        return NULL;
    }
    const uint8_t *bytes = r->data + r->pos;
    if (bytes[n] != 0) {
        rd_fail(r, FDK_ERR_INVALID_ARGUMENT); /* missing NUL */
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (bytes[i] == 0) {
            rd_fail(r, FDK_ERR_INVALID_ARGUMENT); /* embedded NUL */
            return NULL;
        }
    }
    if (!fdk__dbus_valid_utf8((const char *)bytes, n)) {
        rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
        return NULL;
    }
    char *copy = fdk_alloc((size_t)n + 1);
    if (copy == NULL) {
        /* OOM is distinct from a protocol violation: the caller
         * distinguishes through fdk__dreader_ok(), which stays OK
         * here — allocation failure is reported by the NULL itself
         * and the reader's position is still advanced. */
        r->pos += (size_t)n + 1;
        return NULL;
    }
    memcpy(copy, bytes, n);
    copy[n] = '\0';
    r->pos += (size_t)n + 1;
    return copy;
}

char *fdk__rd_signature(fdk__dreader *r) {
    if (r->err != FDK_OK) {
        return NULL;
    }
    uint8_t n = fdk__rd_u8(r);
    if (r->err != FDK_OK) {
        return NULL;
    }
    if ((size_t)n + 1 > r->len - r->pos) {
        rd_fail(r, FDK_ERR_LIMIT);
        return NULL;
    }
    const uint8_t *bytes = r->data + r->pos;
    if (bytes[n] != 0) {
        rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
        return NULL;
    }
    char *copy = fdk_alloc((size_t)n + 1);
    if (copy == NULL) {
        r->pos += (size_t)n + 1;
        return NULL;
    }
    memcpy(copy, bytes, n);
    copy[n] = '\0';
    r->pos += (size_t)n + 1;
    return copy;
}

/* ---- grammar validators ---------------------------------------------------- */

int fdk__dbus_name_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

int fdk__dbus_valid_bus_name(const char *name) {
    if (name == NULL || *name == '\0') {
        return 0;
    }
    if (strlen(name) > FDK_DBUS_SIGNATURE_MAX) {
        return 0; /* the spec's name-length ceiling (255)        */
    }
    const char *p = name;
    int unique = (*p == ':');
    if (unique) {
        p++;
        if (*p == '\0') {
            return 0;
        }
    }
    while (*p != '\0') {
        size_t elen = 0;
        while (*p != '\0' && *p != '.') {
            unsigned char c = (unsigned char)*p;
            if (!fdk__dbus_name_char(c) && c != '-') {
                return 0;
            }
            if (!unique && elen == 0 && c >= '0' && c <= '9') {
                return 0; /* well-known elements may not start digit */
            }
            elen++;
            p++;
        }
        if (elen == 0) {
            return 0; /* empty element ("..", trailing '.') */
        }
        if (*p == '.') {
            p++;
            if (*p == '\0') {
                return 0; /* trailing dot */
            }
        }
    }
    return 1;
}

int fdk__dbus_valid_interface(const char *name) {
    if (name == NULL || *name == '\0') {
        return 0;
    }
    if (strlen(name) > FDK_DBUS_SIGNATURE_MAX) {
        return 0;
    }
    const char *p = name;
    int dots = 0;
    while (*p != '\0') {
        size_t elen = 0;
        while (*p != '\0' && *p != '.') {
            unsigned char c = (unsigned char)*p;
            if (!fdk__dbus_name_char(c)) {
                return 0;
            }
            if (elen == 0 && c >= '0' && c <= '9') {
                return 0;
            }
            elen++;
            p++;
        }
        if (elen == 0) {
            return 0;
        }
        if (*p == '.') {
            dots++;
            p++;
            if (*p == '\0') {
                return 0;
            }
        }
    }
    return dots >= 1; /* at least one dot, no leading/trailing dot */
}

int fdk__dbus_valid_member(const char *name) {
    if (name == NULL || *name == '\0') {
        return 0;
    }
    const char *p = name;
    size_t elen = 0;
    for (; *p != '\0'; p++, elen++) {
        if (!fdk__dbus_name_char((unsigned char)*p)) {
            return 0;
        }
    }
    if (elen == 0) {
        return 0;
    }
    unsigned char first = (unsigned char)name[0];
    return !(first >= '0' && first <= '9');
}

int fdk__dbus_valid_object_path(const char *path) {
    if (path == NULL || path[0] != '/') {
        return 0;
    }
    if (path[1] == '\0') {
        return 1; /* the root path */
    }
    const char *p = path + 1;
    while (*p != '\0') {
        size_t elen = 0;
        while (*p != '\0' && *p != '/') {
            if (!fdk__dbus_name_char((unsigned char)*p)) {
                return 0;
            }
            elen++;
            p++;
        }
        if (elen == 0) {
            return 0; /* "//" or trailing '/' */
        }
        if (*p == '/') {
            p++;
            if (*p == '\0') {
                return 0; /* trailing slash */
            }
        }
    }
    return 1;
}

int fdk__dbus_valid_utf8(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t cont; /* continuation bytes required */
        uint32_t cp; /* the code point being assembled */
        if ((c & 0xE0) == 0xC0) {
            cont = 1;
            cp = c & 0x1F;
            if (cp < 0x02) {
                return 0; /* overlong 2-byte */
            }
        } else if ((c & 0xF0) == 0xE0) {
            cont = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            cont = 3;
            cp = c & 0x07;
            if (cp > 0x04) {
                return 0; /* > U+10FFFF */
            }
        } else {
            return 0; /* continuation byte in lead position or 0xF8+ */
        }
        if (i + cont >= n) {
            return 0; /* truncated sequence */
        }
        for (size_t k = 1; k <= cont; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xC0) != 0x80) {
                return 0;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cont == 2 && cp < 0x800) {
            return 0; /* overlong 3-byte */
        }
        if (cont == 3 && cp < 0x10000) {
            return 0; /* overlong 4-byte */
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return 0; /* surrogate */
        }
        if (cp > 0x10FFFF) {
            return 0; /* beyond Unicode (0xF4 with high bits) */
        }
        i += cont + 1;
    }
    return 1;
}

/* ---- signature walking ----------------------------------------------------- */

/* Alignment of a type given its FIRST signature character (all
 * complete types align by their leading code; verified against the
 * reference bytes). */
static size_t sig_lead_alignment(char c) {
    switch (c) {
    case 'y':
    case 'g':
    case 'v':
        return 1;
    case 'n':
    case 'q':
        return 2;
    case 'b':
    case 'i':
    case 'u':
    case 's':
    case 'o':
    case 'a':
        return 4;
    case 'x':
    case 't':
    case 'd':
    case '(':
    case '{':
        return 8;
    default:
        return 0; /* invalid code */
    }
}

/* Is `c` a complete single-character basic type? */
static int is_basic_code(char c) {
    return c == 'y' || c == 'b' || c == 'n' || c == 'q' || c == 'i' ||
           c == 'u' || c == 'x' || c == 't' || c == 'd' || c == 's' ||
           c == 'o' || c == 'g';
}

int fdk__dbus_sig_one_complete(const char *sig, size_t *pos, int depth_in,
                               int allow_dict, char **out_sig) {
    size_t start = *pos;
    char c = sig[start];
    if (c == '\0') {
        return -1;
    }
    if (depth_in > FDK_DBUS_MAX_DEPTH) {
        return -1;
    }
    size_t consumed;
    if (is_basic_code(c)) {
        consumed = 1;
    } else if (c == 'v') {
        consumed = 1;
    } else if (c == 'a') {
        size_t p = start + 1;
        if (fdk__dbus_sig_one_complete(sig, &p, depth_in + 1, 1,
                                       NULL) < 0) {
            return -1;
        }
        consumed = p - start;
    } else if (c == '(') {
        size_t p = start + 1;
        while (sig[p] != '\0' && sig[p] != ')') {
            if (fdk__dbus_sig_one_complete(sig, &p, depth_in + 1, 0,
                                           NULL) < 0) {
                return -1;
            }
        }
        if (sig[p] != ')') {
            return -1;
        }
        consumed = p - start + 1;
    } else if (c == '{') {
        if (!allow_dict) {
            return -1; /* dict entries only directly inside arrays */
        }
        size_t p = start + 1;
        /* the key: exactly one basic type */
        if (!is_basic_code(sig[p])) {
            return -1;
        }
        p++;
        if (fdk__dbus_sig_one_complete(sig, &p, depth_in + 1, 0, NULL) < 0) {
            return -1;
        }
        if (sig[p] != '}') {
            return -1;
        }
        consumed = p - start + 1;
    } else {
        return -1; /* garbage character */
    }
    if (out_sig != NULL) {
        *out_sig = fdk_alloc(consumed + 1);
        if (*out_sig == NULL) {
            return -1; /* OOM — indistinguishable from invalid; callers
                          treat a -1 as refusal either way */
        }
        memcpy(*out_sig, sig + start, consumed);
        (*out_sig)[consumed] = '\0';
    }
    *pos = start + consumed;
    return (int)consumed;
}

int fdk__dbus_valid_signature(const char *sig, size_t max_types) {
    if (sig == NULL) {
        return 0;
    }
    size_t pos = 0;
    size_t types = 0;
    while (sig[pos] != '\0') {
        if (types >= max_types) {
            return 0;
        }
        if (fdk__dbus_sig_one_complete(sig, &pos, 0, 0, NULL) < 0) {
            return 0;
        }
        types++;
    }
    return 1;
}

int fdk__dbus_signature_is_single(const char *sig) {
    if (sig == NULL || sig[0] == '\0') {
        return 0;
    }
    size_t pos = 0;
    if (fdk__dbus_sig_one_complete(sig, &pos, 0, 1, NULL) < 0) {
        return 0;
    }
    return sig[pos] == '\0';
}

/* ---- marshaling a value ----------------------------------------------------- */

static int marshal_basic(fdk__dbuf *b, const fdk_dbus_value *v) {
    switch (v->type) {
    case FDK_DBUS_BYTE: {
        uint8_t byte = v->v.byte_;
        return fdk__dbuf_put(b, &byte, 1);
    }
    case FDK_DBUS_BOOLEAN:
        if (fdk__dbuf_align(b, 4) != FDK_OK) {
            return b->ok;
        }
        return fdk__dbuf_u32(b, v->v.boolean_ ? 1u : 0u);
    case FDK_DBUS_INT16: {
        uint16_t u;
        int16_t s = v->v.i16;
        memcpy(&u, &s, 2);
        if (fdk__dbuf_align(b, 2) != FDK_OK) {
            return b->ok;
        }
        return fdk__dbuf_u16(b, u);
    }
    case FDK_DBUS_UINT16:
        if (fdk__dbuf_align(b, 2) != FDK_OK) {
            return b->ok;
        }
        return fdk__dbuf_u16(b, v->v.u16);
    case FDK_DBUS_INT32: {
        uint32_t u;
        int32_t s = v->v.i32;
        memcpy(&u, &s, 4);
        if (fdk__dbuf_align(b, 4) != FDK_OK) {
            return b->ok;
        }
        return fdk__dbuf_u32(b, u);
    }
    case FDK_DBUS_UINT32:
        if (fdk__dbuf_align(b, 4) != FDK_OK) {
            return b->ok;
        }
        return fdk__dbuf_u32(b, v->v.u32);
    case FDK_DBUS_INT64: {
        uint64_t u;
        int64_t s = v->v.i64;
        memcpy(&u, &s, 8);
        if (fdk__dbuf_align(b, 8) != FDK_OK) {
            return b->ok;
        }
        return fdk__dbuf_u64(b, u);
    }
    case FDK_DBUS_UINT64:
        if (fdk__dbuf_align(b, 8) != FDK_OK) {
            return b->ok;
        }
        return fdk__dbuf_u64(b, v->v.u64);
    case FDK_DBUS_DOUBLE:
        if (fdk__dbuf_align(b, 8) != FDK_OK) {
            return b->ok;
        }
        return fdk__dbuf_double(b, v->v.dbl);
    default:
        return FDK_ERR_INVALID_ARGUMENT;
    }
}

int fdk__dbus_marshal_value(fdk__dbuf *b, const fdk_dbus_value *v,
                            int depth) {
    if (v == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (depth > FDK_DBUS_MAX_DEPTH) {
        return FDK_ERR_LIMIT;
    }
    switch (v->type) {
    case FDK_DBUS_BYTE:
    case FDK_DBUS_BOOLEAN:
    case FDK_DBUS_INT16:
    case FDK_DBUS_UINT16:
    case FDK_DBUS_INT32:
    case FDK_DBUS_UINT32:
    case FDK_DBUS_INT64:
    case FDK_DBUS_UINT64:
    case FDK_DBUS_DOUBLE:
        return marshal_basic(b, v);

    case FDK_DBUS_STRING:
    case FDK_DBUS_OBJECT_PATH:
    case FDK_DBUS_SIGNATURE:
        if (v->v.str == NULL) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        if (v->type == FDK_DBUS_SIGNATURE) {
            return fdk__dbuf_signature(b, v->v.str);
        }
        return fdk__dbuf_string(b, v->v.str);

    case FDK_DBUS_VARIANT: {
        /* signature (alignment 1) then the value at ITS alignment */
        int r = fdk__dbuf_signature(b, v->v.variant.sig);
        if (r != FDK_OK) {
            return r;
        }
        return fdk__dbus_marshal_value(b, v->v.variant.child, depth + 1);
    }

    case FDK_DBUS_ARRAY: {
        const char *es = v->v.array.elem_sig;
        if (es == NULL || es[0] == '\0') {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        size_t elem_align = sig_lead_alignment(es[0]);
        if (elem_align == 0) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        int r = fdk__dbuf_align(b, 4); /* the u32 length */
        if (r != FDK_OK) {
            return r;
        }
        size_t len_pos = b->len;
        r = fdk__dbuf_u32(b, 0); /* placeholder */
        if (r != FDK_OK) {
            return r;
        }
        /* Padding to element alignment is required even when the
         * array is empty (the spec's explicit rule; the reference
         * implementation pads an empty a(ay) the same way). */
        r = fdk__dbuf_align(b, elem_align);
        if (r != FDK_OK) {
            return r;
        }
        size_t start = b->len;
        for (size_t i = 0; i < v->v.array.n; i++) {
            r = fdk__dbus_marshal_value(b, v->v.array.items[i], depth + 1);
            if (r != FDK_OK) {
                return r;
            }
        }
        size_t n = b->len - start;
        if (n > UINT32_MAX) {
            return FDK_ERR_LIMIT;
        }
        b->data[len_pos + 0] = (uint8_t)(n & 0xFF);
        b->data[len_pos + 1] = (uint8_t)((n >> 8) & 0xFF);
        b->data[len_pos + 2] = (uint8_t)((n >> 16) & 0xFF);
        b->data[len_pos + 3] = (uint8_t)((n >> 24) & 0xFF);
        return FDK_OK;
    }

    case FDK_DBUS_STRUCT:
    case FDK_DBUS_DICT_ENTRY: {
        int r = fdk__dbuf_align(b, 8);
        if (r != FDK_OK) {
            return r;
        }
        for (size_t i = 0; i < v->v.structure.n; i++) {
            r = fdk__dbus_marshal_value(b, v->v.structure.items[i],
                                        depth + 1);
            if (r != FDK_OK) {
                return r;
            }
        }
        return FDK_OK;
    }

    default:
        return FDK_ERR_INVALID_ARGUMENT;
    }
}

/* ---- unmarshaling a value --------------------------------------------------- */

/* The reader's error discipline: any violation sticks r->err and
 * all subsequent reads are no-ops. A NULL return with r->err clean
 * means allocation failure. The caller checks fdk__dreader_ok()
 * after each node and at the end of the walk. */

static fdk_dbus_value *unmarshal_typed(fdk__dreader *r, const char *sig);

/* Local mirror of dbus_types.c's static constructor (kept private
 * here to avoid widening the internal surface). */
static fdk_dbus_value *value_new_local(uint8_t type) {
    fdk_dbus_value *v = fdk_alloc(sizeof(*v));
    if (v != NULL) {
        v->type = type;
        memset(&v->v, 0, sizeof(v->v));
    }
    return v;
}

static fdk_dbus_value *unmarshal_dict_entry(fdk__dreader *r,
                                            const char *sig) {
    /* sig is "{<key><value>}" — the key is exactly one basic */
    fdk_dbus_value *key = unmarshal_typed(r, sig + 1);
    if (key == NULL) {
        return NULL;
    }
    if (!fdk__rd_ok(r)) {
        fdk_dbus_value_free(key);
        return NULL;
    }
    /* the value's signature follows the key's single type: a basic
     * is always exactly one character, so the value starts at 2 */
    fdk_dbus_value *val = unmarshal_typed(r, sig + 2);
    if (val == NULL) {
        fdk_dbus_value_free(key);
        return NULL;
    }
    if (!fdk__rd_ok(r)) {
        fdk_dbus_value_free(key);
        fdk_dbus_value_free(val);
        return NULL;
    }
    fdk_dbus_value *e = value_new_local(FDK_DBUS_DICT_ENTRY);
    if (e == NULL) {
        fdk_dbus_value_free(key);
        fdk_dbus_value_free(val);
        return NULL;
    }
    e->v.structure.items = fdk_alloc_array(2, sizeof(fdk_dbus_value *));
    if (e->v.structure.items == NULL) {
        fdk_free(e);
        fdk_dbus_value_free(key);
        fdk_dbus_value_free(val);
        return NULL;
    }
    e->v.structure.items[0] = key;
    e->v.structure.items[1] = val;
    e->v.structure.n = 2;
    return e;
}

static fdk_dbus_value *unmarshal_dict_entry(fdk__dreader *r,
                                            const char *sig);

static fdk_dbus_value *unmarshal_typed(fdk__dreader *r, const char *sig) {
    if (!fdk__rd_ok(r)) {
        return NULL;
    }
    char c = sig[0];
    switch (c) {
    case 'y': {
        fdk_dbus_value *v = value_new_local(FDK_DBUS_BYTE);
        if (v == NULL) {
            return NULL;
        }
        v->v.byte_ = fdk__rd_u8(r);
        return v;
    }
    case 'b': {
        uint32_t raw = 0;
        if (fdk__rd_align(r, 4) != FDK_OK) {
            return NULL;
        }
        raw = fdk__rd_u32(r);
        if (!fdk__rd_ok(r)) {
            return NULL;
        }
        if (raw > 1) {
            rd_fail(r, FDK_ERR_INVALID_ARGUMENT); /* only 0/1 on the wire */
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_BOOLEAN);
        if (v == NULL) {
            return NULL;
        }
        v->v.boolean_ = (int)raw;
        return v;
    }
    case 'n': {
        if (fdk__rd_align(r, 2) != FDK_OK) {
            return NULL;
        }
        uint16_t u = fdk__rd_u16(r);
        if (!fdk__rd_ok(r)) {
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_INT16);
        if (v == NULL) {
            return NULL;
        }
        int16_t s;
        memcpy(&s, &u, 2);
        v->v.i16 = s;
        return v;
    }
    case 'q': {
        if (fdk__rd_align(r, 2) != FDK_OK) {
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_UINT16);
        if (v == NULL) {
            return NULL;
        }
        v->v.u16 = fdk__rd_u16(r);
        return v;
    }
    case 'i': {
        if (fdk__rd_align(r, 4) != FDK_OK) {
            return NULL;
        }
        uint32_t u = fdk__rd_u32(r);
        if (!fdk__rd_ok(r)) {
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_INT32);
        if (v == NULL) {
            return NULL;
        }
        int32_t s;
        memcpy(&s, &u, 4);
        v->v.i32 = s;
        return v;
    }
    case 'u': {
        if (fdk__rd_align(r, 4) != FDK_OK) {
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_UINT32);
        if (v == NULL) {
            return NULL;
        }
        v->v.u32 = fdk__rd_u32(r);
        return v;
    }
    case 'x': {
        if (fdk__rd_align(r, 8) != FDK_OK) {
            return NULL;
        }
        uint64_t u = fdk__rd_u64(r);
        if (!fdk__rd_ok(r)) {
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_INT64);
        if (v == NULL) {
            return NULL;
        }
        int64_t s;
        memcpy(&s, &u, 8);
        v->v.i64 = s;
        return v;
    }
    case 't': {
        if (fdk__rd_align(r, 8) != FDK_OK) {
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_UINT64);
        if (v == NULL) {
            return NULL;
        }
        v->v.u64 = fdk__rd_u64(r);
        return v;
    }
    case 'd': {
        if (fdk__rd_align(r, 8) != FDK_OK) {
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_DOUBLE);
        if (v == NULL) {
            return NULL;
        }
        v->v.dbl = fdk__rd_double(r);
        return v;
    }
    case 's': {
        char *s = fdk__rd_string(r);
        if (s == NULL) {
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_STRING);
        if (v == NULL) {
            fdk_free(s);
            return NULL;
        }
        v->v.str = s;
        return v;
    }
    case 'o': {
        char *s = fdk__rd_string(r);
        if (s == NULL) {
            return NULL;
        }
        if (!fdk__rd_ok(r)) {
            fdk_free(s);
            return NULL;
        }
        if (!fdk__dbus_valid_object_path(s)) {
            rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
            fdk_free(s);
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_OBJECT_PATH);
        if (v == NULL) {
            fdk_free(s);
            return NULL;
        }
        v->v.str = s;
        return v;
    }
    case 'g': {
        char *s = fdk__rd_signature(r);
        if (s == NULL) {
            return NULL;
        }
        if (!fdk__rd_ok(r)) {
            fdk_free(s);
            return NULL;
        }
        if (!fdk__dbus_valid_signature(s, FDK_DBUS_SIGNATURE_MAX)) {
            rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
            fdk_free(s);
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_SIGNATURE);
        if (v == NULL) {
            fdk_free(s);
            return NULL;
        }
        v->v.str = s;
        return v;
    }
    case 'v': {
        char *inner = fdk__rd_signature(r);
        if (inner == NULL) {
            return NULL;
        }
        if (!fdk__rd_ok(r)) {
            fdk_free(inner);
            return NULL;
        }
        if (!fdk__dbus_signature_is_single(inner)) {
            rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
            fdk_free(inner);
            return NULL;
        }
        r->depth++;
        if (r->depth > FDK_DBUS_MAX_DEPTH) {
            rd_fail(r, FDK_ERR_LIMIT);
            fdk_free(inner);
            return NULL;
        }
        fdk_dbus_value *child = unmarshal_typed(r, inner);
        r->depth--;
        if (child == NULL) {
            fdk_free(inner);
            return NULL;
        }
        if (!fdk__rd_ok(r)) {
            fdk_free(inner);
            fdk_dbus_value_free(child);
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_VARIANT);
        if (v == NULL) {
            fdk_free(inner);
            fdk_dbus_value_free(child);
            return NULL;
        }
        v->v.variant.sig = inner;
        v->v.variant.child = child;
        return v;
    }
    case 'a': {
        /* element signature: one complete type after 'a' */
        size_t p = 1;
        char *es = NULL;
        if (fdk__dbus_sig_one_complete(sig, &p, 1, 1, &es) < 0) {
            rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
            return NULL;
        }
        size_t elem_align = sig_lead_alignment(es[0]);
        if (elem_align == 0) {
            fdk_free(es);
            rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
            return NULL;
        }
        /* the u32 length */
        if (fdk__rd_align(r, 4) != FDK_OK) {
            fdk_free(es);
            return NULL;
        }
        uint32_t n = fdk__rd_u32(r);
        if (!fdk__rd_ok(r)) {
            fdk_free(es);
            return NULL;
        }
        /* skip to element alignment, then the n-byte region */
        if (fdk__rd_align(r, elem_align) != FDK_OK) {
            fdk_free(es);
            return NULL;
        }
        if (n > r->len - r->pos) {
            rd_fail(r, FDK_ERR_LIMIT);
            fdk_free(es);
            return NULL;
        }
        size_t region_end = r->pos + n;

        r->depth++;
        if (r->depth > FDK_DBUS_MAX_DEPTH) {
            rd_fail(r, FDK_ERR_LIMIT);
            fdk_free(es);
            r->depth--;
            return NULL;
        }

        /* Parse elements one at a time into a growing pointer array.
         * The element count is NOT known up front (variable-size
         * elements) — and an attacker-controlled n cannot make us
         * pre-allocate anything: the region is already bounds-fit. */
        fdk_dbus_value **items = NULL;
        size_t count = 0;
        size_t cap = 0;
        while (r->pos < region_end) {
            fdk_dbus_value *item = unmarshal_typed(r, es);
            if (item == NULL || !fdk__rd_ok(r)) {
                fdk_dbus_values_free(items, count);
                fdk_free(items);
                fdk_free(es);
                r->depth--;
                return NULL;
            }
            if (count == cap) {
                size_t ncap = cap ? cap * 2 : 4;
                fdk_dbus_value **grown =
                    fdk_realloc(items, ncap * sizeof(*grown));
                if (grown == NULL) {
                    fdk_dbus_value_free(item);
                    fdk_dbus_values_free(items, count);
                fdk_free(items);
                    fdk_free(es);
                    r->depth--;
                    return NULL;
                }
                items = grown;
                cap = ncap;
            }
            items[count++] = item;
            /* every element must END inside the region; trailing
             * alignment padding between elements is consumed by the
             * next element's own alignment walk. */
            if (r->pos > region_end) {
                rd_fail(r, FDK_ERR_LIMIT);
                fdk_dbus_values_free(items, count);
                fdk_free(items);
                fdk_free(es);
                r->depth--;
                return NULL;
            }
        }
        r->depth--;

        fdk_dbus_value *v = value_new_local(FDK_DBUS_ARRAY);
        if (v == NULL) {
            fdk_free(es);
            fdk_dbus_values_free(items, count);
                fdk_free(items);
            return NULL;
        }
        v->v.array.elem_sig = es; /* ownership moves to the node */
        v->v.array.items = items;
        v->v.array.n = count;
        return v;
    }
    case '(': {
        if (fdk__rd_align(r, 8) != FDK_OK) {
            return NULL;
        }
        r->depth++;
        if (r->depth > FDK_DBUS_MAX_DEPTH) {
            rd_fail(r, FDK_ERR_LIMIT);
            r->depth--;
            return NULL;
        }
        fdk_dbus_value **items = NULL;
        size_t count = 0;
        size_t cap = 0;
        size_t p = 1;
        while (sig[p] != ')' && sig[p] != '\0') {
            size_t save = p;
            if (fdk__dbus_sig_one_complete(sig, &p, 0, 0, NULL) < 0) {
                rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
                fdk_dbus_values_free(items, count);
                fdk_free(items);
                r->depth--;
                return NULL;
            }
            char one[256];
            size_t olen = p - save;
            if (olen + 1 > sizeof(one)) {
                rd_fail(r, FDK_ERR_LIMIT); /* beyond any legal signature */
                fdk_dbus_values_free(items, count);
                fdk_free(items);
                r->depth--;
                return NULL;
            }
            memcpy(one, sig + save, olen);
            one[olen] = '\0';
            fdk_dbus_value *item = unmarshal_typed(r, one);
            if (item == NULL || !fdk__rd_ok(r)) {
                fdk_dbus_values_free(items, count);
                fdk_free(items);
                r->depth--;
                return NULL;
            }
            if (count == cap) {
                size_t ncap = cap ? cap * 2 : 4;
                fdk_dbus_value **grown =
                    fdk_realloc(items, ncap * sizeof(*grown));
                if (grown == NULL) {
                    fdk_dbus_value_free(item);
                    fdk_dbus_values_free(items, count);
                fdk_free(items);
                    r->depth--;
                    return NULL;
                }
                items = grown;
                cap = ncap;
            }
            items[count++] = item;
        }
        r->depth--;
        if (sig[p] != ')') {
            fdk_dbus_values_free(items, count);
                fdk_free(items);
            return NULL;
        }
        fdk_dbus_value *v = value_new_local(FDK_DBUS_STRUCT);
        if (v == NULL) {
            fdk_dbus_values_free(items, count);
                fdk_free(items);
            return NULL;
        }
        v->v.structure.items = items;
        v->v.structure.n = count;
        return v;
    }
    case '{':
        if (fdk__rd_align(r, 8) != FDK_OK) {
            return NULL;
        }
        return unmarshal_dict_entry(r, sig);
    default:
        rd_fail(r, FDK_ERR_INVALID_ARGUMENT);
        return NULL;
    }
}

fdk_dbus_value *fdk__dbus_unmarshal_value(fdk__dreader *r, const char *sig) {
    if (r == NULL || sig == NULL || sig[0] == '\0') {
        return NULL;
    }
    /* one complete type, covering the whole string, is the contract */
    size_t pos = 0;
    if (fdk__dbus_sig_one_complete(sig, &pos, 0, 1, NULL) < 0 ||
        sig[pos] != '\0') {
        if (r->err == FDK_OK) {
            r->err = FDK_ERR_INVALID_ARGUMENT;
        }
        return NULL;
    }
    return unmarshal_typed(r, sig);
}
