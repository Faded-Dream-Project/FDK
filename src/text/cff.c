/*
 * cff.c — native CFF / CFF2 outline support (Type 2 charstrings).
 *
 * PROVENANCE AND LICENSE POSTURE: this file is an original
 * implementation written from the public font specifications —
 *   Adobe Technical Note #5176, "The Compact Font Format
 *   Specification" (4 Dec 2003),
 *   Adobe Technical Note #5177, "The Type 2 Charstring Format"
 *   (16 March 2000), and
 *   the OpenType 'CFF2' chapter (1.8.x)
 * — with the two normative data tables those specs define (the 391
 * Standard Strings and the 256-entry StandardEncoding) embedded as
 * spec data with their provenance noted at each table. No code was
 * copied or ported from FreeType, fontTools, stb_truetype's CFF
 * branch, or any other project. See docs/THIRD-PARTY.md.
 *
 * WHY NATIVE (the CFF-outline decision, docs/DECISIONS.md D-011): the vendored
 * stb_truetype v1.26 happens to carry a Type 2 interpreter, but (a)
 * it initializes its CFF view with a fabricated 512 MiB table size
 * ("@TODO this should use size from the table") — on truncated or
 * hostile files every operator read past the real table is bounded
 * only by that lie, which is exactly the class of out-of-bounds read
 * FDK's font-parsing policy forbids; (b) it has no CFF2 support at
 * all; (c) it ASSERTS on malformed INDEX/DICT bytes (stbtt__cff_int
 * dies on a single mutated Private DICT byte — found live by the
 * fuzz battery, not theorized); and (d) it is not ours to harden.
 * This module owns the whole CFF/CFF2 path with real bounds,
 * budgets and depth caps, and the loader runs it BEFORE stb ever
 * sees an OTTO face (text.c — the order is load-bearing).
 *
 * SCOPE: the sfnt-hosted variants FDK actually loads —
 *   'OTTO' + 'CFF ' (OpenType CFF, incl. CID-keyed via
 *   FDArray/FDSelect — the Noto CJK posture), and
 *   'OTTO' + 'CFF2' (variable-font era; the DEFAULT instance is
 *   rendered: blend deltas are dropped, which is exactly the
 *   default-coordinate composition).
 * Bare .cff FontSets (not wrapped in an sfnt) are not a thing FDK's
 * loader ever produces; the table-relative offsets are honored as
 * specified either way.
 *
 * SECURITY / ROBUSTNESS POSTURE (everything that parses untrusted
 * bytes is bounded here):
 *   - every read is bounds-checked against the REAL table extent
 *     (never the file, never a fabricated size);
 *   - INDEX structures: count, offSize (1..4), the whole offset
 *     array, and every item extent are validated before any pointer
 *     arithmetic produces a dereferenceable range;
 *   - subr call depth is capped at 12 (the spec's implementation
 *     limit is 10; one frame of grace) and a per-glyph operator
 *     budget of 1,000,000 kills cyclic subr graphs — a hang becomes
 *     a malformed-glyph report, never an infinite loop;
 *   - the argument stack is hard-capped (48 CFF1 / 513 CFF2 — the
 *     spec limits), the transient array at 32, stems at 4096 (spec
 *     limit 96; the mask arithmetic is allowed the headroom);
 *   - endchar-seac composition is depth-capped at 4 and forbidden
 *     entirely in CID-keyed and CFF2 fonts (spec: "may not be
 *     nested"; CID/CFF2 have no name machinery to resolve it);
 *   - coordinates accumulate as doubles (exact for every legal
 *     integer and 16.16 operand) and are clamped to int16 when
 *     emitted into stbtt_vertex, matching the rasterizer's
 *     coordinate type — a malformed 10^9-unit coordinate renders as
 *     a clamped box, not as an overflowed one.
 *
 * Single-threaded, like the rest of FDK's object model. Allocations
 * go through fdk_alloc/fdk_free.
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* =====================================================================
 * 1. Bounds-checked readers
 *
 * A cff_rng is a validated, non-aliasable byte range. Every helper
 * takes the range explicitly and NEVER reads past rng->len; reads
 * that would run past return a failure instead of a value.
 * ===================================================================== */

typedef struct {
    const fdk_u8 *p; /* first byte                                       */
    size_t len;      /* number of bytes                                  */
} cff_rng;

static bool cff_rng_ok(const cff_rng *r) {
    return r->p != NULL;
}

/* Absolute position r->p + at, only when [at, at+n) lies inside r. */
static const fdk_u8 *cff_at(const cff_rng *r, size_t at, size_t n) {
    if (at > r->len || n > r->len - at) {
        return NULL;
    }
    return r->p + at;
}

static bool cff_u8(const cff_rng *r, size_t at, fdk_u32 *out) {
    const fdk_u8 *p = cff_at(r, at, 1);
    if (p == NULL) {
        return false;
    }
    *out = p[0];
    return true;
}

static bool cff_u16(const cff_rng *r, size_t at, fdk_u32 *out) {
    const fdk_u8 *p = cff_at(r, at, 2);
    if (p == NULL) {
        return false;
    }
    *out = ((fdk_u32)p[0] << 8) | (fdk_u32)p[1];
    return true;
}

/* Unsigned n-byte big-endian value, 1 <= n <= 4 (INDEX offsets). */
static bool cff_un(const cff_rng *r, size_t at, int n, fdk_u32 *out) {
    const fdk_u8 *p = cff_at(r, at, (size_t)n);
    if (p == NULL) {
        return false;
    }
    fdk_u32 v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 8) | (fdk_u32)p[i];
    }
    *out = v;
    return true;
}

static bool cff_i16(const cff_rng *r, size_t at, fdk_i32 *out) {
    fdk_u32 u = 0;
    if (!cff_u16(r, at, &u)) {
        return false;
    }
    *out = (fdk_i32)(u ^ 0x8000u) - 0x8000; /* sign-extend */
    return true;
}

static bool cff_u32_at(const cff_rng *r, size_t at, fdk_u32 *out) {
    return cff_un(r, at, 4, out);
}

static bool cff_i32(const cff_rng *r, size_t at, fdk_i32 *out) {
    const fdk_u8 *p = cff_at(r, at, 4);
    if (p == NULL) {
        return false;
    }
    fdk_u32 u = ((fdk_u32)p[0] << 24) | ((fdk_u32)p[1] << 16) |
                ((fdk_u32)p[2] << 8) | (fdk_u32)p[3];
    *out = (fdk_i32)u;
    return true;
}

/* A sub-range [at, at+len) of r, validated in one place. */
static bool cff_sub(const cff_rng *r, size_t at, size_t len, cff_rng *out) {
    if (at > r->len || len > r->len - at) {
        return false;
    }
    out->p = r->p + at;
    out->len = len;
    return true;
}

/* =====================================================================
 * 2. INDEX structures (CFF spec §5; CFF2: count is uint32)
 *
 * Layout: count (2 or 4 bytes); if count > 0: offSize (1 byte),
 * offsets[count+1] of offSize bytes each (1-based, relative to the
 * byte preceding the object data), then the object data. An empty
 * INDEX is just the count field.
 * ===================================================================== */

typedef struct {
    size_t count;      /* number of objects (validated)                  */
    int off_size;      /* 1..4, or 0 when count == 0                    */
    size_t hdr;        /* header size: 2/4, or +1+... when count > 0    */
    size_t extent;     /* the INDEX's OWN byte size (validated)         */
    cff_rng base;      /* the whole INDEX slice                          */
} cff_index;

/* Parses the INDEX starting at base. Fills *out. Returns false when
 * the structure does not fit / is inconsistent. count and every
 * offset are validated against the slice extent NOW — later item
 * accesses cannot walk out of the table. */
static bool cff_index_parse(const cff_rng *base, int is_cff2,
                            cff_index *out) {
    memset(out, 0, sizeof(*out));
    out->base = *base;

    fdk_u32 count = 0;
    if (is_cff2) {
        if (!cff_u32_at(base, 0, &count)) { /* 4-byte count */
            return false;
        }
        out->hdr = 4;
    } else {
        if (!cff_u16(base, 0, &count)) {
            return false;
        }
        out->hdr = 2;
    }
    /* Refuse counts whose offset array cannot possibly fit even
     * before reading offSize: (count+1) * 4 bytes must fit. */
    if (count > base->len || (size_t)count + 1 > (base->len / 4u) + 1u) {
        return false;
    }
    out->count = count;
    if (count == 0) {
        out->off_size = 0;
        out->extent = is_cff2 ? 4 : 2; /* count field only */
        return true;
    }

    fdk_u32 off_size = 0;
    if (!cff_u8(base, out->hdr, &off_size) || off_size < 1 ||
        off_size > 4) {
        return false;
    }
    out->off_size = (int)off_size;
    out->hdr += 1;

    /* The offset array must fit in the slice. */
    size_t offs_pos = out->hdr; /* offsets start (absolute in base) */
    size_t offs_len = (size_t)(count + 1) * (size_t)off_size;
    if (offs_len > base->len || offs_pos > base->len - offs_len) {
        return false;
    }
    /* Data starts right after the offset array. */
    size_t data_off = offs_pos + offs_len;

    /* Offsets are 1-based relative to (data_off - 1): item i spans
     * [data_off + off[i] - 1, data_off + off[i+1] - 1). Validate the
     * LAST offset now so every item access only needs re-validation
     * of its own pair; the INDEX's own extent follows from it. */
    fdk_u32 last = 0;
    if (!cff_un(base, offs_pos + (size_t)count * (size_t)off_size,
                (int)off_size, &last)) {
        return false;
    }
    if (last == 0 || (size_t)(last - 1) > base->len - data_off) {
        return false; /* data runs past the INDEX slice */
    }
    out->extent = data_off + (size_t)last - 1;
    return true;
}

/* Item i of an INDEX as a validated range. Items are monotonically
 * ordered in a well-formed INDEX (off[i] <= off[i+1]); violations
 * are rejected here rather than trusted. */
static bool cff_index_item(const cff_index *ix, size_t i, cff_rng *out) {
    if (i >= ix->count || ix->off_size == 0) {
        return false;
    }
    size_t offs_pos = (size_t)ix->hdr; /* count[+offSize] already in hdr */
    fdk_u32 a = 0, b = 0;
    if (!cff_un(&ix->base, offs_pos + i * (size_t)ix->off_size,
                ix->off_size, &a) ||
        !cff_un(&ix->base, offs_pos + (i + 1) * (size_t)ix->off_size,
                ix->off_size, &b)) {
        return false;
    }
    if (a == 0 || b < a) {
        return false; /* 0 is not a legal 1-based offset; must ascend */
    }
    size_t data_off = offs_pos + (size_t)(ix->count + 1) *
                                  (size_t)ix->off_size;
    return cff_sub(&ix->base, data_off + (size_t)a - 1,
                   (size_t)(b - a), out);
}

/* =====================================================================
 * 3. DICT data (CFF spec §8 and Appendix H)
 *
 * Operands: 32..246 (v-139), 247..250 / 251..254 (+/-108..1131),
 * 28 (int16), 29 (int32), 30 (real, nibble-encoded). Operators are
 * 0..21 one-byte and 12,x two-byte. The parser below walks a DICT
 * and hands (operands, operator) pairs to a callback; the callback
 * returns false to abort (malformed font).
 * ===================================================================== */

#define CFF_DICT_MAX_OPERANDS 64

typedef struct {
    double v[CFF_DICT_MAX_OPERANDS];
    int n;
} cff_args;

/* Decodes one real operand (byte 30) starting at *io_pos (which
 * points AT the byte after 30's pair? no: at the first nibble byte).
 * Advances *io_pos past the terminating F nibble. The nibble alphabet
 * (Appendix H): 0-9 digits, A '.', B 'E', C 'E-', D '-', E reserved,
 * F end-of-number. */
static bool cff_real(const cff_rng *r, size_t *io_pos, double *out) {
    char buf[64];
    int n = 0;
    bool done = false;
    while (!done) {
        fdk_u32 byte = 0;
        if (!cff_u8(r, *io_pos, &byte)) {
            return false; /* ran off the DICT before the F nibble */
        }
        (*io_pos)++;
        for (int half = 0; half < 2; half++) {
            fdk_u32 nib = half == 0 ? (byte >> 4) : (byte & 0xFu);
            if (nib == 0xFu) {
                done = true;
                break;
            }
            if (nib <= 9) {
                if (n < (int)sizeof(buf) - 1) {
                    buf[n++] = (char)('0' + (int)nib);
                }
            } else if (nib == 0xAu) {
                if (n < (int)sizeof(buf) - 1) {
                    buf[n++] = '.';
                }
            } else if (nib == 0xBu) {
                if (n + 1 < (int)sizeof(buf) - 1) {
                    buf[n++] = 'E';
                    buf[n++] = '+';
                }
            } else if (nib == 0xCu) {
                if (n + 1 < (int)sizeof(buf) - 1) {
                    buf[n++] = 'E';
                    buf[n++] = '-';
                }
            } else if (nib == 0xDu) {
                if (n < (int)sizeof(buf) - 1) {
                    buf[n++] = '-';
                }
            } else if (nib == 0xEu) {
                /* reserved nibble: tolerated as a no-op separator so
                 * the rest of the number still parses */
            }
        }
    }
    buf[n] = '\0';
    *out = n > 0 ? strtod(buf, NULL) : 0.0;
    return true;
}

/* Parses the whole DICT in r, invoking cb(operands, op, escaped)
 * per operator. Returns false on structural error or callback veto.
 * Real operands are decoded (as doubles); integer operands exactly. */
typedef bool (*cff_dict_cb)(const cff_args *a, fdk_u32 op, bool escaped,
                            void *user);

static bool cff_dict_walk(const cff_rng *r, cff_dict_cb cb, void *user) {
    cff_args args;
    args.n = 0;
    size_t pos = 0;
    while (pos < r->len) {
        fdk_u32 b0 = 0;
        if (!cff_u8(r, pos, &b0)) {
            return false;
        }
        pos++;
        if (b0 == 29) { /* 32-bit int */
            fdk_i32 v = 0;
            if (!cff_i32(r, pos, &v)) {
                return false;
            }
            pos += 4;
            if (args.n < CFF_DICT_MAX_OPERANDS) {
                args.v[args.n++] = (double)v;
            }
        } else if (b0 == 28) { /* 16-bit int */
            fdk_i32 v = 0;
            if (!cff_i16(r, pos, &v)) {
                return false;
            }
            pos += 2;
            if (args.n < CFF_DICT_MAX_OPERANDS) {
                args.v[args.n++] = (double)v;
            }
        } else if (b0 == 30) { /* real */
            double v = 0;
            if (!cff_real(r, &pos, &v)) {
                return false;
            }
            if (args.n < CFF_DICT_MAX_OPERANDS) {
                args.v[args.n++] = v;
            }
        } else if (b0 >= 32 && b0 <= 246) {
            if (args.n < CFF_DICT_MAX_OPERANDS) {
                args.v[args.n++] = (double)(int)b0 - 139;
            }
        } else if (b0 >= 247 && b0 <= 250) {
            fdk_u32 b1 = 0;
            if (!cff_u8(r, pos, &b1)) {
                return false;
            }
            pos++;
            if (args.n < CFF_DICT_MAX_OPERANDS) {
                args.v[args.n++] =
                    (double)((int)(b0 - 247) * 256 + (int)b1 + 108);
            }
        } else if (b0 >= 251 && b0 <= 254) {
            fdk_u32 b1 = 0;
            if (!cff_u8(r, pos, &b1)) {
                return false;
            }
            pos++;
            if (args.n < CFF_DICT_MAX_OPERANDS) {
                args.v[args.n++] =
                    (double)(-((int)(b0 - 251) * 256) - (int)b1 - 108);
            }
        } else if (b0 <= 21) {
            bool escaped = false;
            fdk_u32 op = b0;
            if (b0 == 12) { /* escape */
                fdk_u32 b1 = 0;
                if (!cff_u8(r, pos, &b1)) {
                    return false;
                }
                pos++;
                escaped = true;
                op = b1;
            }
            if (!cb(&args, op, escaped, user)) {
                return false;
            }
            args.n = 0; /* operator clears the operand stack */
        } else {
            /* bytes 22..27 and 255 are not legal DICT entries */
            return false;
        }
    }
    return true;
}

/* =====================================================================
 * 4. The parsed font object
 * ===================================================================== */

/* One Private DICT's view of the world: the local Subrs INDEX (as a
 * raw slice, parsed lazily per glyph — the bias needs its count, so
 * it IS parsed eagerly below into subrs) and the two width defaults.
 * CFF2 Privates additionally carry a vsindex (recorded, unused: the
 * default instance never applies deltas). */
typedef struct {
    cff_index subrs;    /* parsed local subrs INDEX (may be empty)  */
    fdk_i32 nominal_width_x;
    fdk_i32 default_width_x;
    fdk_i32 vsindex;
} cff_private;

struct fdk_cff_font {
    int is_cff2;
    cff_rng table;          /* the whole 'CFF '/'CFF2' table           */

    cff_index gsubrs;       /* global subrs INDEX                      */
    cff_index charstrings;  /* glyph charstrings INDEX                 */
    cff_index fdarray;      /* Font DICT INDEX (CID/CFF2; empty else)  */
    cff_index strings;      /* String INDEX (CFF1; empty for CFF2)     */
    cff_index name_index;   /* Name INDEX (CFF1; kept for seac-free
                               diagnostics only)                      */

    /* Private DICTs: one for plain CFF1 fonts (Top DICT Private),
     * one per Font DICT otherwise. Plain CFF1 -> privs[0]. */
    cff_private *privs;
    int n_privs;

    /* CID-keyed (CFF1): FDSelect maps GID -> private index.        */
    bool has_fdselect;
    int fdselect_fmt;       /* 0, 3 (CFF2 also: 4)                    */
    cff_rng fdselect;

    /* charset (CFF1; CFF2 has none): GID -> SID/CID. Parsed lazily,
     * ONLY when an endchar-seac composition needs name resolution. */
    fdk_u32 charset_kind;   /* 0/1/2 predefined, 0xFFFFFFFF = offset  */
    size_t charset_off;
    bool is_cid;            /* Top DICT carried ROS                    */
    fdk_u16 *gid_to_sid;    /* lazy charset map; NULL until needed     */

    /* Glyph-space -> stb-unit affine (see the FontMatrix note in
     * fdk__cff_parse): v = M * g, with M pre-scaled so that a
     * default-matrix font is the identity. */
    fdk_f64 mx[6];          /* a b c d tx ty                           */
};

/* ---- Top / Font DICT extraction (one callback state machine) ---- */

typedef struct {
    fdk_cff_font *f;
    /* Top DICT outputs: */
    size_t charstrings_off;   /* 0 = absent                          */
    size_t fdarray_off;       /* 0 = absent                          */
    size_t fdselect_off;      /* 0 = absent                          */
    bool have_fdselect;
    size_t charset_op;        /* raw operand                          */
    size_t private_off, private_size;
    bool have_private;
    bool saw_ros;
    fdk_f64 fontmatrix[6];
    bool have_fontmatrix;
    int depth;                /* 0 = Top DICT, 1 = Font DICT         */
    /* Font DICT outputs (depth == 1): */
    size_t fd_private_off, fd_private_size;
    bool fd_have_private;
} cff_topdump;

static double cff_arg(const cff_args *a, int i) {
    return i < a->n ? a->v[i] : 0.0;
}

/* The one shared callback: records the operators this module needs
 * and skips (without complaint) everything else — a CFF Top DICT is
 * full of metadata (Notice, XUID, BlueValues...) that outline
 * generation never touches. */
static bool cff_top_cb(const cff_args *a, fdk_u32 op, bool escaped,
                       void *user) {
    cff_topdump *d = user;

    if (!escaped && op == 17) { /* CharStrings (Top only) */
        if (d->depth != 0) {
            return false;
        }
        d->charstrings_off = (size_t)(fdk_i64)cff_arg(a, 0);
        return d->charstrings_off != 0;
    }
    if (!escaped && op == 15) { /* charset (Top only, CFF1) */
        if (d->depth != 0) {
            return false;
        }
        d->charset_op = (size_t)(fdk_i64)cff_arg(a, 0);
        return true;
    }
    if (escaped && op == 36) { /* FDArray (Top only) */
        if (d->depth != 0) {
            return false;
        }
        d->fdarray_off = (size_t)(fdk_i64)cff_arg(a, 0);
        return true;
    }
    if (escaped && op == 37) { /* FDSelect (Top only) */
        if (d->depth != 0) {
            return false;
        }
        d->fdselect_off = (size_t)(fdk_i64)cff_arg(a, 0);
        d->have_fdselect = true;
        return true;
    }
    if (escaped && op == 30) { /* ROS: CID-keyed marker (Top only) */
        if (d->depth != 0) {
            return false;
        }
        d->saw_ros = true;
        return true;
    }
    if (escaped && op == 7) { /* FontMatrix (Top only; 6 operands) */
        if (d->depth != 0 || a->n < 6) {
            return false;
        }
        for (int i = 0; i < 6; i++) {
            d->fontmatrix[i] = a->v[i];
        }
        d->have_fontmatrix = true;
        return true;
    }
    if (!escaped && op == 18) { /* Private vs vstore, by dialect+depth */
        if (d->depth == 0 && !d->f->is_cff2) {
            /* Top DICT Private (CFF1 only): [size, offset] */
            if (a->n < 2) {
                return false;
            }
            d->private_size = (size_t)(fdk_i64)cff_arg(a, 0);
            d->private_off = (size_t)(fdk_i64)cff_arg(a, 1);
            d->have_private = true;
            return true;
        }
        if (d->depth == 1) {
            /* Font DICT Private (CFF1 CID + CFF2): [size, offset] */
            if (a->n < 2) {
                return false;
            }
            d->fd_private_size = (size_t)(fdk_i64)cff_arg(a, 0);
            d->fd_private_off = (size_t)(fdk_i64)cff_arg(a, 1);
            d->fd_have_private = true;
            return true;
        }
        /* CFF2 Top DICT op 18 is vstore: recorded, never dereferenced
         * (default instance). */
        return true;
    }
    return true; /* everything else: metadata, skipped */
}

/* ---- Private DICT extraction ---- */

typedef struct {
    int is_cff2;
    size_t priv_abs;       /* Private DICT start, TABLE-relative       */
    cff_rng table;         /* the whole table (Subrs may live past
                              the DICT's own extent)                  */
    cff_private *out;
} cff_privdump;

static bool cff_priv_cb(const cff_args *a, fdk_u32 op, bool escaped,
                        void *user) {
    cff_privdump *d = user;
    if (!escaped && op == 19) { /* Subrs: offset relative to the
                                 * Private DICT start (both dialects);
                                 * the INDEX itself is table data that
                                 * commonly follows the DICT bytes, so
                                 * it is sliced to the TABLE end, not
                                 * the Private DICT end. */
        size_t off = (size_t)(fdk_i64)cff_arg(a, 0);
        if (off > d->table.len - d->priv_abs) {
            return false;
        }
        cff_rng sub;
        if (!cff_sub(&d->table, d->priv_abs + off,
                     d->table.len - d->priv_abs - off, &sub)) {
            return false;
        }
        if (!cff_index_parse(&sub, d->is_cff2, &d->out->subrs)) {
            return false;
        }
        return true;
    }
    if (!escaped && op == 20) { /* nominalWidthX */
        d->out->nominal_width_x = (fdk_i32)cff_arg(a, 0);
        return true;
    }
    if (!escaped && op == 21) { /* defaultWidthX */
        d->out->default_width_x = (fdk_i32)cff_arg(a, 0);
        return true;
    }
    if (!escaped && op == 22) { /* vsindex (CFF2 Private) */
        d->out->vsindex = (fdk_i32)cff_arg(a, 0);
        return true;
    }
    return true; /* hinting private data: skipped */
}

/* Parses one Private DICT at [off, off+len) inside the table. */
static bool cff_private_parse(fdk_cff_font *f, size_t off, size_t len,
                              cff_private *out) {
    memset(out, 0, sizeof(*out));
    out->vsindex = -1;
    cff_rng priv;
    if (!cff_sub(&f->table, off, len, &priv)) {
        return false;
    }
    cff_privdump pd;
    pd.is_cff2 = f->is_cff2;
    pd.priv_abs = off;
    pd.table = f->table;
    pd.out = out;
    if (!cff_dict_walk(&priv, cff_priv_cb, &pd)) {
        return false;
    }
    return true;
}

/* =====================================================================
 * 5. FDSelect (CFF spec §19; CFF2 adds format 4)
 *
 * Maps GID -> Font DICT index. Format 0: one byte per glyph. Format
 * 3: u16 nRanges, (u16 first, u8 fd) ranges, u16 sentinel. Format 4
 * (CFF2 only): u32 nRanges, (u32 first, u16 fd) ranges, u32 sentinel.
 * ===================================================================== */

/* Returns the FD index for gid, or -1 when the FDSelect data is
 * malformed or the gid is out of range (caller treats as .notdef). */
static int cff_fdselect_lookup(const fdk_cff_font *f, size_t gid) {
    const cff_rng *r = &f->fdselect;
    if (!f->has_fdselect || !cff_rng_ok(r)) {
        return 0; /* no FDSelect: everything uses Font DICT 0 */
    }
    if (f->fdselect_fmt == 0) {
        fdk_u32 fd = 0;
        if (gid >= f->charstrings.count ||
            !cff_u8(r, 1 + gid, &fd)) {
            return -1;
        }
        return (int)fd;
    }
    if (f->fdselect_fmt == 3) {
        fdk_u32 nranges = 0;
        if (!cff_u16(r, 1, &nranges)) {
            return -1;
        }
        /* Ranges are sorted ascending by first GID, starting at 0,
         * terminated by a sentinel equal to nGlyphs. Binary search. */
        size_t lo = 0, hi = nranges; /* [lo, hi) */
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            fdk_u32 first = 0;
            if (!cff_u16(r, 3 + mid * 3, &first)) {
                return -1;
            }
            if (gid < first) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        if (lo == 0) {
            return -1; /* first range must start at 0 */
        }
        fdk_u32 fd = 0, prev_first = 0;
        if (!cff_u8(r, 3 + (lo - 1) * 3 + 2, &fd) ||
            !cff_u16(r, 3 + (lo - 1) * 3, &prev_first)) {
            return -1;
        }
        if (gid < prev_first) {
            return -1; /* unreachable given the search, kept honest */
        }
        return (int)fd;
    }
    if (f->fdselect_fmt == 4) { /* CFF2 only */
        fdk_u32 nranges = 0;
        if (!cff_u32_at(r, 1, &nranges)) {
            return -1;
        }
        size_t lo = 0, hi = nranges;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            fdk_u32 first = 0;
            if (!cff_u32_at(r, 5 + mid * 6, &first)) {
                return -1;
            }
            if (gid < first) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        if (lo == 0) {
            return -1;
        }
        fdk_u32 fd = 0;
        if (!cff_u16(r, 5 + (lo - 1) * 6 + 4, &fd)) {
            return -1;
        }
        return (int)fd;
    }
    return -1;
}

/* Validates FDSelect structure at parse time (so lookups above can
 * trust the extents): extent and sentinel checks per format. */
static bool cff_fdselect_validate(fdk_cff_font *f) {
    if (!f->has_fdselect) {
        return true;
    }
    const cff_rng *r = &f->fdselect;
    if (!cff_rng_ok(r)) {
        return false;
    }
    fdk_u32 fmt = 0;
    if (!cff_u8(r, 0, &fmt)) {
        return false;
    }
    f->fdselect_fmt = (int)fmt;
    if (fmt == 0) {
        return r->len >= 1 + f->charstrings.count;
    }
    if (fmt == 3) {
        fdk_u32 nranges = 0, sentinel = 0;
        if (!cff_u16(r, 1, &nranges) ||
            r->len < 3 + (size_t)nranges * 3 + 2) {
            return false;
        }
        if (!cff_u16(r, 3 + (size_t)nranges * 3, &sentinel)) {
            return false;
        }
        /* The sentinel delimits the last range; glyphs at/after it
         * would be out of the font anyway. */
        return sentinel <= f->charstrings.count + 1;
    }
    if (fmt == 4) {
        fdk_u32 nranges = 0, sentinel = 0;
        if (!cff_u32_at(r, 1, &nranges) ||
            r->len < 5 + (size_t)nranges * 6 + 4) {
            return false;
        }
        if (!cff_u32_at(r, 5 + (size_t)nranges * 6, &sentinel)) {
            return false;
        }
        return sentinel <= f->charstrings.count + 1;
    }
    return false; /* unknown format */
}

/* =====================================================================
 * 6. Charset (CFF spec §13) — parsed lazily, ONLY for seac
 *
 * Formats 0 (SID array), 1 and 2 (ranges). Predefined charsets
 * 0 = ISOAdobe (GID n -> SID n), 1 = Expert, 2 = ExpertSubset; the
 * Expert tables are not embedded — seac against them reports
 * malformed (no real font does it; the ISOAdobe identity needs no
 * table at all).
 * ===================================================================== */

/* Builds f->gid_to_sid (nGlyphs entries; GID 0 is always .notdef,
 * SID 0). Returns false when the charset runs past the table or
 * disagrees with the glyph count. */
static bool cff_charset_build(fdk_cff_font *f) {
    if (f->gid_to_sid != NULL) {
        return true;
    }
    size_t n = f->charstrings.count;
    if (n == 0) {
        return false;
    }
    fdk_u16 *map = fdk_alloc_array(n, sizeof(fdk_u16));
    if (map == NULL) {
        return false;
    }
    map[0] = 0; /* .notdef */

    if (f->is_cid) {
        /* CID charset: GID n -> CID; seac is forbidden in CID fonts
         * (checked before we get here), so the map is never used —
         * but format validation still runs. */
    }

    if (f->charset_kind != 0xFFFFFFFFu) {
        if (f->charset_kind != 0) {
            /* Expert / ExpertSubset predefined: not embedded. */
            fdk_free(map);
            return false;
        }
        /* ISOAdobe: identity GID -> SID. */
        for (size_t i = 1; i < n; i++) {
            map[i] = (fdk_u16)i;
        }
        f->gid_to_sid = map;
        return true;
    }

    cff_rng cs;
    if (!cff_sub(&f->table, f->charset_off, f->table.len - f->charset_off,
                 &cs)) {
        fdk_free(map);
        return false;
    }
    fdk_u32 fmt = 0;
    if (!cff_u8(&cs, 0, &fmt)) {
        fdk_free(map);
        return false;
    }
    size_t pos = 1, gid = 1;
    if (fmt == 0) {
        while (gid < n) {
            fdk_u32 sid = 0;
            if (!cff_u16(&cs, pos, &sid)) {
                fdk_free(map);
                return false;
            }
            map[gid++] = (fdk_u16)sid;
            pos += 2;
        }
    } else if (fmt == 1 || fmt == 2) {
        int step = fmt == 1 ? 3 : 4; /* SID first + nLeft (u8/u16) */
        while (gid < n) {
            fdk_u32 first = 0, nleft = 0;
            if (!cff_u16(&cs, pos, &first) ||
                (fmt == 1 && !cff_u8(&cs, pos + 2, &nleft)) ||
                (fmt == 2 && !cff_u16(&cs, pos + 2, &nleft))) {
                fdk_free(map);
                return false;
            }
            pos += (size_t)step;
            for (fdk_u32 k = 0; k <= nleft && gid < n; k++) {
                map[gid++] = (fdk_u16)(first + k);
            }
        }
    } else {
        fdk_free(map);
        return false;
    }
    f->gid_to_sid = map;
    return true;
}

/* =====================================================================
 * 7. Spec data tables (seac support)
 *
 * seac resolution needs only the StandardEncoding (Appendix B) table
 * below: its 256 entries map codes to STANDARD SIDs (0..390), and the
 * charset maps GIDs to SIDs — matching is by SID value, so the String
 * INDEX (custom SID names) is never consulted. No other spec table
 * is needed anywhere in this module.
 * ===================================================================== */

/* StandardEncoding (CFF spec Appendix B, the "Standard"
 * predefined encoding): code -> SID, 256 entries. seac accent
 * components arrive as StandardEncoding CODES; this table turns
 * them into the SIDs the charset maps. Spec DATA, not code. */
static const fdk_u16 cff_std_encoding[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80,
    81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
    0, 111, 112, 113, 114, 0, 115, 116, 117, 118, 119, 120, 121, 122, 0, 123,
    0, 124, 125, 126, 127, 128, 129, 130, 131, 0, 132, 133, 0, 134, 135, 136,
    137, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 138, 0, 139, 0, 0, 0, 0, 140, 141, 142, 143, 0, 0, 0, 0,
    0, 144, 0, 0, 0, 145, 0, 0, 146, 147, 148, 149, 0, 0, 0, 0,
};

/* ---- seac component resolution ---- */

/* Resolves a StandardEncoding code to the GID whose charset name is
 * that code's SID. Needs the lazy charset and (for custom SIDs) the
 * String INDEX. Returns -1 when unresolvable. */
static int cff_seac_gid(fdk_cff_font *f, fdk_u32 std_code) {
    if (f->is_cff2 || f->is_cid) {
        return -1; /* no name machinery (spec: no seac there) */
    }
    if (std_code > 255 || !cff_charset_build(f)) {
        return -1;
    }
    fdk_u32 want_sid = cff_std_encoding[std_code];
    size_t n = f->charstrings.count;
    for (size_t g = 0; g < n && g < (size_t)0xFFFF; g++) {
        if ((fdk_u32)f->gid_to_sid[g] == want_sid) {
            return (int)g;
        }
    }
    return -1;
}

/* =====================================================================
 * 8. The Type 2 charstring interpreter
 *
 * One iterative machine (explicit frame stack — subr calls push
 * frames, never C recursion) that walks charstrings and emits
 * stbtt_vertex entries (font units, int16-clamped) plus a bbox.
 * Every operator, operand encoding, width rule, hint-mask skip, flex
 * form, stack/arithmetic op and limit below is Type2-spec (5177) or
 * CFF2-spec material; section references sit at each case.
 * ===================================================================== */

#define CFF_T2_STACK_MAX 513      /* CFF1 limit 48, CFF2 513; the
                                   * array serves both, the CHECK is
                                   * per dialect                        */
#define CFF_T2_TRANSIENT 32       /* spec Appendix B                    */
#define CFF_T2_CALL_DEPTH 12      /* spec limit 10 + grace             */
#define CFF_T2_OPS_BUDGET 1000000 /* cyclic-subr takedown              */
#define CFF_T2_STEMS_MAX 4096     /* spec limit 96; mask math headroom */

typedef struct {
    cff_rng code;                 /* the executing byte range          */
    size_t pos;
} cff_frame;

/* ---- the shared output sink (vertices + bbox) ----
 * Separated from the per-charstring run state so seac components
 * append their outlines into the same glyph. */
typedef struct {
    stbtt_vertex *v;
    int nv, cap;
    fdk_f64 bx0, by0, bx1, by1;
    int has_ink;
    int dry; /* bbox-only mode: no vertex array is built */
} cff_sink;

static void cff_bbox_pt(cff_sink *k, fdk_f64 x, fdk_f64 y) {
    if (!k->has_ink) {
        k->bx0 = k->bx1 = x;
        k->by0 = k->by1 = y;
        k->has_ink = 1;
        return;
    }
    if (x < k->bx0) k->bx0 = x;
    if (x > k->bx1) k->bx1 = x;
    if (y < k->by0) k->by0 = y;
    if (y > k->by1) k->by1 = y;
}


/* ---- the per-charstring run state ---- */

typedef struct {
    /* operand stack */
    double s[CFF_T2_STACK_MAX];
    int n;
    /* transient array (put/get; 32 slots per the spec limits) */
    double t[CFF_T2_TRANSIENT];
    /* call frames; frames[0] is the glyph's own charstring */
    cff_frame fr[CFF_T2_CALL_DEPTH];
    int depth;
    /* current point (font units; doubles are exact for every legal
     * integer and 16.16 operand, and survive div/sqrt fractions) */
    fdk_f64 x, y;
    /* the open contour's start point (Type 2 contours close
     * IMPLICITLY: a charstring need not line back to its moveto —
     * the consumer closes it; 5177 §4.2: "the last point... need not
     * coincide with the first" — the synthesized closing line rides
     * the vertex stream so the rasterizer's polygon is closed) */
    fdk_f64 fx, fy;
    bool in_contour;
    /* width parsing (CFF1 §4.2 Note 4): armed until the first
     * stack-clearing operator consumes the optional width */
    bool width_pending;
    /* stems declared so far — sizes the hintmask/cntrmask skips */
    int stems;
    /* the font and the shared output sink */
    fdk_cff_font *f;
    cff_sink *sink;
    /* the Private DICT serving callsubr (resolved per glyph) */
    const cff_private *priv;
    /* operator budget: every executed operator costs 1 */
    fdk_i64 ops_left;
    /* deterministic LCG state for the (deprecated) random op */
    fdk_u64 lcg;
    /* seac composition depth (spec: "may not be nested"; 4 grace) */
    int seac_depth;
} cff_run;

/* Pushes an operand; the per-dialect stack limit is enforced (48 in
 * CFF1, 513 in CFF2 — Appendix B / CFF2 spec). */
static bool cff_push(cff_run *r, double v) {
    int limit = r->f->is_cff2 ? 513 : 48;
    if (r->n >= limit) {
        return false;
    }
    r->s[r->n++] = v;
    return true;
}

/* Pops the width argument off the BOTTOM of the stack when the width
 * rule fires: the first stack-clearing operator takes one extra
 * leading argument when the operand count's parity is off (5177
 * §3.1/§4.2 Note 4). CFF2 has no widths at all. */
static void cff_eat_width(cff_run *r, int parity) {
    if (!r->width_pending) {
        return;
    }
    r->width_pending = false;
    if (r->f->is_cff2 || r->n == 0 || (r->n % 2) == parity) {
        return;
    }
    /* The first operand is the width delta from nominalWidthX —
     * sfnt-hosted fonts take advances from hmtx, so the value is
     * discarded, only the stack position matters. */
    for (int i = 0; i + 1 < r->n; i++) {
        r->s[i] = r->s[i + 1];
    }
    r->n--;
}

/* ---- path construction (relative deltas from the current point) -- */

/* Emits one vertex; coordinates go through the FontMatrix affine
 * (identity for default-matrix fonts) and clamp to int16 — the
 * stbtt_vertex coordinate type. (x,y) is the segment END point;
 * control points ride along per stb's vertex layout. */
static bool cff_emit(cff_run *r, unsigned char type, fdk_f64 x, fdk_f64 y,
                     fdk_f64 cx, fdk_f64 cy, fdk_f64 cx1, fdk_f64 cy1) {
    cff_sink *k = r->sink;
    const fdk_f64 *m = r->f->mx;
    fdk_f64 X = m[0] * x + m[2] * y + m[4];
    fdk_f64 Y = m[1] * x + m[3] * y + m[5];
    fdk_f64 CX = m[0] * cx + m[2] * cy + m[4];
    fdk_f64 CY = m[1] * cx + m[3] * cy + m[5];
    fdk_f64 CX1 = m[0] * cx1 + m[2] * cy1 + m[4];
    fdk_f64 CY1 = m[1] * cx1 + m[3] * cy1 + m[5];

    cff_bbox_pt(k, X, Y);
    if (type == STBTT_vcurve || type == STBTT_vcubic) {
        cff_bbox_pt(k, CX, CY);
    }
    if (type == STBTT_vcubic) {
        cff_bbox_pt(k, CX1, CY1);
    }

    if (k->dry) {
        return true; /* bbox-only pass: geometry recorded, no alloc */
    }

    if (k->nv == k->cap) {
        int ncap = k->cap == 0 ? 32 : k->cap * 2;
        stbtt_vertex *nv =
            (stbtt_vertex *)fdk_realloc(k->v, (size_t)ncap *
                                                 sizeof(stbtt_vertex));
        if (nv == NULL) {
            return false;
        }
        k->v = nv;
        k->cap = ncap;
    }
    stbtt_vertex *e = &k->v[k->nv++];
    /* int16 clamp: a malformed 10^9-unit coordinate renders as a
     * clamped box instead of wrapping around the plane. */
    fdk_f64 cl;
    cl = X < -32768.0 ? -32768.0 : (X > 32767.0 ? 32767.0 : X);
    e->x = (short)lround(cl);
    cl = Y < -32768.0 ? -32768.0 : (Y > 32767.0 ? 32767.0 : Y);
    e->y = (short)lround(cl);
    cl = CX < -32768.0 ? -32768.0 : (CX > 32767.0 ? 32767.0 : CX);
    e->cx = (short)lround(cl);
    cl = CY < -32768.0 ? -32768.0 : (CY > 32767.0 ? 32767.0 : CY);
    e->cy = (short)lround(cl);
    cl = CX1 < -32768.0 ? -32768.0 : (CX1 > 32767.0 ? 32767.0 : CX1);
    e->cx1 = (short)lround(cl);
    cl = CY1 < -32768.0 ? -32768.0 : (CY1 > 32767.0 ? 32767.0 : CY1);
    e->cy1 = (short)lround(cl);
    e->type = type;
    e->padding = 0;
    return true;
}

/* Closes the open contour when its end drifted off the start point
 * — the implicit-close rule (see fx/fy above). A no-op when the
 * charstring already closed it explicitly or nothing was drawn. */
static bool t2_close_contour(cff_run *r) {
    if (!r->in_contour) {
        return true;
    }
    r->in_contour = false;
    if (r->x == r->fx && r->y == r->fy) {
        return true; /* explicitly closed already */
    }
    return cff_emit(r, STBTT_vline, r->fx, r->fy, 0, 0, 0, 0);
}

static bool t2_moveto(cff_run *r, fdk_f64 dx, fdk_f64 dy) {
    if (!t2_close_contour(r)) {
        return false;
    }
    r->x += dx;
    r->y += dy;
    r->fx = r->x;
    r->fy = r->y;
    r->in_contour = true;
    return cff_emit(r, STBTT_vmove, r->x, r->y, 0, 0, 0, 0);
}

static bool t2_lineto(cff_run *r, fdk_f64 dx, fdk_f64 dy) {
    r->x += dx;
    r->y += dy;
    return cff_emit(r, STBTT_vline, r->x, r->y, 0, 0, 0, 0);
}

/* One cubic Bézier: three deltas (control1, control2, endpoint). */
static bool t2_curveto(cff_run *r, fdk_f64 dx1, fdk_f64 dy1, fdk_f64 dx2,
                       fdk_f64 dy2, fdk_f64 dx3, fdk_f64 dy3) {
    fdk_f64 x1 = r->x + dx1, y1 = r->y + dy1;
    fdk_f64 x2 = x1 + dx2, y2 = y1 + dy2;
    r->x = x2 + dx3;
    r->y = y2 + dy3;
    return cff_emit(r, STBTT_vcubic, r->x, r->y, x1, y1, x2, y2);
}

/* ---- subr dispatch (bias rule: 5177 §2.3: <1240 -> 107,
 * <33900 -> 1131, else 32768) ---- */

static fdk_i64 cff_subr_bias(size_t count) {
    if (count < 1240) {
        return 107;
    }
    if (count < 33900) {
        return 1131;
    }
    return 32768;
}

static bool t2_callsubr(cff_run *r, const cff_index *ix, fdk_i64 arg) {
    if (ix == NULL || ix->count == 0) {
        return false; /* calling into an absent/empty INDEX */
    }
    fdk_i64 idx = arg + cff_subr_bias(ix->count);
    if (idx < 0 || (fdk_u64)idx >= ix->count) {
        return false;
    }
    if (r->depth + 1 >= CFF_T2_CALL_DEPTH) {
        return false; /* depth cap: spec limit 10, cap 12 */
    }
    cff_rng code;
    if (!cff_index_item(ix, (size_t)idx, &code)) {
        return false;
    }
    r->depth++;
    r->fr[r->depth].code = code;
    r->fr[r->depth].pos = 0;
    return true;
}

/* ---- hint-mask skipping ----
 * hintmask/cntrmask are followed by (stems + 7) / 8 mask bytes that
 * must be consumed as data, not operands (5177 §4.3). */
static bool t2_skip_mask(cff_run *r) {
    cff_frame *fr = &r->fr[r->depth];
    size_t bytes = ((size_t)r->stems + 7) / 8;
    if (fr->pos > fr->code.len || bytes > fr->code.len - fr->pos) {
        return false; /* mask runs past the charstring end */
    }
    fr->pos += bytes;
    return true;
}

/* ---- endchar + seac (5177 §4.2, Appendix C) ---- */

/* Forward: the interpreter (seac components re-enter it). */
static bool cff_interpret(fdk_cff_font *f, const cff_rng *cs,
                          const cff_private *priv, fdk_f64 ox, fdk_f64 oy,
                          cff_sink *sink, int seac_depth);

static bool t2_endchar(cff_run *r) {
    if (r->f->is_cff2) {
        return false; /* CFF2 charstrings have no endchar */
    }
    r->width_pending = false; /* endchar IS stack-clearing (Note 4) */
    /* Arg counts: 0 plain, 1 width, 4 seac, 5 width+seac (the width
     * rule's zero-or-one argument plus seac's four). Anything else
     * is malformed. */
    if (r->n == 1 || r->n == 5) {
        r->n--; /* drop the width value (hmtx owns real advances) */
    }
    if (r->n != 0 && r->n != 4) {
        return false;
    }
    if (!t2_close_contour(r)) {
        return false; /* the implicit contour close precedes both the
                         plain exit and the seac composition — the
                         accent's outlines must not weld onto an open
                         base contour */
    }
    if (r->n == 0) {
        r->n = 0;
        return true; /* glyph complete */
    }

    /* seac: adx ady bchar achar. bchar/achar are StandardEncoding
     * codes; components resolve through the charset and append into
     * the same sink, the accent translated by (adx, ady). Spec:
     * components may not nest; forbidden outright in CID/CFF2. */
    fdk_f64 adx = r->s[0], ady = r->s[1];
    fdk_u32 bchar = (fdk_u32)r->s[2], achar = (fdk_u32)r->s[3];
    r->n = 0;
    if (r->seac_depth >= 4 || bchar > 255 || achar > 255) {
        return false;
    }
    int base = cff_seac_gid(r->f, bchar);
    int accent = cff_seac_gid(r->f, achar);
    if (base < 0 || accent < 0) {
        return false;
    }
    cff_rng bc, ac;
    if (!cff_index_item(&r->f->charstrings, (size_t)base, &bc) ||
        !cff_index_item(&r->f->charstrings, (size_t)accent, &ac)) {
        return false;
    }
    if (!cff_interpret(r->f, &bc, r->priv, 0.0, 0.0, r->sink,
                       r->seac_depth + 1) ||
        !cff_interpret(r->f, &ac, r->priv, adx, ady, r->sink,
                       r->seac_depth + 1)) {
        return false;
    }
    return true;
}

/* ---- the flex family (5177 §4.1 "flex", "hflex", "hflex1", "flex1")
 * ---- Two Béziers that a hinter may flatten; unhinted renderers
 * always draw the curves (fd is a rasterizer hint, not geometry). */

static bool t2_flex(cff_run *r) { /* 12 35: 13 args: 6+6 deltas + fd */
    if (r->n != 13) {
        return false;
    }
    const double *s = r->s;
    if (!t2_curveto(r, s[0], s[1], s[2], s[3], s[4], s[5]) ||
        !t2_curveto(r, s[6], s[7], s[8], s[9], s[10], s[11])) {
        return false;
    }
    r->n = 0;
    return true;
}

static bool t2_hflex(cff_run *r) { /* 12 34: dx1 dx2 dy2 dx3 dx4 dx5 dx6 */
    if (r->n != 7) {
        return false;
    }
    const double *s = r->s;
    /* c1=(dx1,0) c2=(+dx2,+dy2) j=(+dx3,0) c3=(+dx4,0)
     * c4=(+dx5,-dy2) end=(+dx6,0) — the join and the control points
     * share y; start and end share y (the operator's contract). */
    if (!t2_curveto(r, s[0], 0, s[1], s[2], s[3], 0) ||
        !t2_curveto(r, s[4], 0, s[5], -s[2], s[6], 0)) {
        return false;
    }
    r->n = 0;
    return true;
}

static bool t2_hflex1(cff_run *r) { /* 12 36: 9 args, end returns to y0 */
    if (r->n != 9) {
        return false;
    }
    const double *s = r->s;
    /* c1=(dx1,dy1) c2=(+dx2,+dy2) j=(+dx3,0) c3=(+dx4,0)
     * c4=(+dx5,+dy5) end=(+dx6, -(dy1+dy2+dy5)) — the final dy is
     * implied by "starting and ending points have the same y". */
    if (!t2_curveto(r, s[0], s[1], s[2], s[3], s[4], 0) ||
        !t2_curveto(r, s[5], 0, s[6], s[7], s[8],
                    -(s[1] + s[3] + s[7]))) {
        return false;
    }
    r->n = 0;
    return true;
}

static bool t2_flex1(cff_run *r) { /* 12 37: 11 args + axis-picked d6 */
    if (r->n != 11) {
        return false;
    }
    const double *s = r->s;
    fdk_f64 x0 = r->x, y0 = r->y;
    if (!t2_curveto(r, s[0], s[1], s[2], s[3], s[4], s[5])) {
        return false;
    }
    fdk_f64 dx = s[0] + s[2] + s[4] + s[6] + s[8];
    fdk_f64 dy = s[1] + s[3] + s[5] + s[7] + s[9];
    fdk_f64 ex, ey;
    if (fabs(dx) > fabs(dy)) { /* horizontal flex: d6 is dx, y -> y0 */
        ex = x0 + dx + s[10];
        ey = y0;
    } else { /* vertical flex: d6 is dy, and the end point's x is
                 equal to the starting x (5177 §4.3: "the last
                 point's x-value is equal to x and its y-value is
                 given by d6") */
        ex = x0;
        ey = y0 + dy + s[10];
    }
    fdk_f64 xa = r->x, ya = r->y; /* the join point after curve 1 */
    bool ok = cff_emit(r, STBTT_vcubic, ex, ey, xa + s[6], ya + s[7],
                       xa + s[6] + s[8], ya + s[7] + s[9]);
    r->x = ex;
    r->y = ey;
    r->n = 0;
    return ok;
}

/* ---- the interpreter main loop ---- */

/* Interprets one charstring (the glyph's own, or an seac component)
 * from origin (ox, oy), appending geometry into `sink`. `priv` is
 * the Private DICT whose Subrs INDEX serves callsubr — resolved per
 * glyph by the caller (FDSelect for CID/CFF2, privs[0] otherwise;
 * seac components inherit the composing glyph's slot: their own FD
 * resolution would matter only in exotic CID layouts, and seac is
 * forbidden in CID fonts anyway).
 *
 * Returns false when the charstring is malformed (caller: no
 * outline, report-and-placeholder), out of budget, or out of
 * memory. A charstring that simply ends (CFF2 semantics; CFF1
 * tolerance for a missing terminal endchar) completes the glyph. */
static bool cff_interpret(fdk_cff_font *f, const cff_rng *cs,
                          const cff_private *priv, fdk_f64 ox, fdk_f64 oy,
                          cff_sink *sink, int seac_depth) {
    cff_run r;
    memset(&r, 0, sizeof(r));
    r.f = f;
    r.sink = sink;
    r.priv = priv;
    r.x = ox;
    r.y = oy;
    r.width_pending = !f->is_cff2;
    r.ops_left = CFF_T2_OPS_BUDGET;
    r.seac_depth = seac_depth;
    r.fr[0].code = *cs;
    r.fr[0].pos = 0;
    r.depth = 0;

    for (;;) {
        if (r.ops_left-- == 0) {
            return false; /* budget: cyclic subrs become failures */
        }
        cff_frame *fr = &r.fr[r.depth];
        if (fr->pos >= fr->code.len) {
            /* This frame is exhausted. */
            if (r.depth == 0) {
                return t2_close_contour(&r); /* glyph complete
                                (CFF2 end-of-string; CFF1 tolerance
                                for missing endchar) — the implicit
                                contour close rides the exit */
            }
            r.depth--; /* subr return without an explicit return op */
            continue;
        }
        const cff_rng *code = &fr->code;
        fdk_u32 b0 = code->p[fr->pos++];
        fdk_u32 op = b0;

        /* ---- operand encodings (5177 §2.2; CFF2 identical) ---- */
        if (b0 >= 32 && b0 <= 246) {
            if (!cff_push(&r, (double)((int)b0 - 139))) {
                return false;
            }
            continue;
        }
        if (b0 >= 247 && b0 <= 250) {
            fdk_u32 b1 = 0;
            if (!cff_u8(code, fr->pos, &b1)) {
                return false;
            }
            fr->pos++;
            if (!cff_push(&r, (double)((int)(b0 - 247) * 256 +
                                       (int)b1 + 108))) {
                return false;
            }
            continue;
        }
        if (b0 >= 251 && b0 <= 254) { /* negative two-byte integer */
            fdk_u32 b1 = 0;
            if (!cff_u8(code, fr->pos, &b1)) {
                return false;
            }
            fr->pos++;
            if (!cff_push(&r, (double)(-((int)(b0 - 251) * 256) -
                                       (int)b1 - 108))) {
                return false;
            }
            continue;
        }
        if (b0 == 28) { /* shortint */
            fdk_i32 v = 0;
            if (!cff_i16(code, fr->pos, &v)) {
                return false;
            }
            fr->pos += 2;
            if (!cff_push(&r, (double)v)) {
                return false;
            }
            continue;
        }
        if (b0 == 255) { /* 32-bit 16.16 Fixed */
            fdk_i32 v = 0;
            if (!cff_i32(code, fr->pos, &v)) {
                return false;
            }
            fr->pos += 4;
            if (!cff_push(&r, (fdk_f64)v / 65536.0)) {
                return false;
            }
            continue;
        }

        /* ---- one-byte operators ---- */
        switch (op) {
        case 1:   /* hstem */
        case 3:   /* vstem */
        case 18:  /* hstemhm */
        case 23: { /* vstemhm — hint pairs, parsed and skipped.
                       Operator numbers are 5177 Table 3: 18 hstemhm,
                       19 hintmask, 20 cntrmask, 21 rmoveto, 22 hmoveto,
                       23 vstemhm — NOT the Type1-era numbering an
                       implementation unfamiliar with the spec reaches
                       for (the class of bug the Noto/URW probes
                       caught: 12 operand bytes then 0x12 is six stem
                       pairs + hstemhm, not a 12-arg rmoveto). */
            cff_eat_width(&r, 0);
            if ((r.n % 2) != 0 || r.n / 2 > CFF_T2_STEMS_MAX - r.stems) {
                return false;
            }
            r.stems += r.n / 2;
            r.n = 0;
            continue;
        }
        case 19:  /* hintmask */
        case 20: { /* cntrmask — mask bytes skipped as data. The
                       stack may carry trailing stem pairs (5177
                       §4.3: hstemhm/vstemhm + hintmask share one
                       stack-clearing sequence). */
            cff_eat_width(&r, 0);
            if ((r.n % 2) != 0) {
                return false; /* trailing stem args must pair up */
            }
            r.stems += r.n / 2;
            r.n = 0;
            if (!t2_skip_mask(&r)) {
                return false;
            }
            continue;
        }
        case 4: { /* vmoveto: dy */
            cff_eat_width(&r, 1);
            if (r.n != 1) {
                return false;
            }
            fdk_f64 dy = r.s[0];
            r.n = 0;
            if (!t2_moveto(&r, 0, dy)) {
                return false;
            }
            continue;
        }
        case 22: { /* hmoveto: dx */
            cff_eat_width(&r, 1);
            if (r.n != 1) {
                return false;
            }
            fdk_f64 dx = r.s[0];
            r.n = 0;
            if (!t2_moveto(&r, dx, 0)) {
                return false;
            }
            continue;
        }
        case 21: { /* rmoveto: dx dy */
            cff_eat_width(&r, 0);
            if (r.n != 2) {
                return false;
            }
            fdk_f64 dx = r.s[0], dy = r.s[1];
            r.n = 0;
            if (!t2_moveto(&r, dx, dy)) {
                return false;
            }
            continue;
        }
        case 5: { /* rlineto: {dx dy}+ */
            if (r.n < 2 || (r.n % 2) != 0) {
                return false;
            }
            for (int i = 0; i + 1 < r.n; i += 2) {
                if (!t2_lineto(&r, r.s[i], r.s[i + 1])) {
                    return false;
                }
            }
            r.n = 0;
            continue;
        }
        case 6:   /* hlineto */
        case 7: { /* vlineto — one line per arg, axes alternate */
            if (r.n < 1) {
                return false;
            }
            for (int i = 0; i < r.n; i++) {
                bool horiz = ((op == 6) ? (i % 2 == 0) : (i % 2 != 0));
                if (!t2_lineto(&r, horiz ? r.s[i] : 0,
                               horiz ? 0 : r.s[i])) {
                    return false;
                }
            }
            r.n = 0;
            continue;
        }
        case 8: { /* rrcurveto: {dxa dya dxb dyb dxc dyc}+ */
            if (r.n < 6 || (r.n % 6) != 0) {
                return false;
            }
            for (int i = 0; i + 5 < r.n; i += 6) {
                if (!t2_curveto(&r, r.s[i], r.s[i + 1], r.s[i + 2],
                                r.s[i + 3], r.s[i + 4], r.s[i + 5])) {
                    return false;
                }
            }
            r.n = 0;
            continue;
        }
        case 24: { /* rcurveline: {6 args}+ then dxd dyd */
            if (r.n < 8 || (r.n % 6) != 2) {
                return false;
            }
            int i = 0;
            for (; i + 5 < r.n - 2; i += 6) {
                if (!t2_curveto(&r, r.s[i], r.s[i + 1], r.s[i + 2],
                                r.s[i + 3], r.s[i + 4], r.s[i + 5])) {
                    return false;
                }
            }
            if (!t2_lineto(&r, r.s[i], r.s[i + 1])) {
                return false;
            }
            r.n = 0;
            continue;
        }
        case 25: { /* rlinecurve: {dx dy}+ then 6 curve args —
                      n = 2k + 6 (k >= 1): any EVEN count >= 8,
                      NOT n = 6k + 2 (Noto's gid 174 carries three
                      lines before its final curve and pins this) */
            if (r.n < 8 || (r.n % 2) != 0) {
                return false;
            }
            int i = 0;
            for (; i + 1 < r.n - 6; i += 2) {
                if (!t2_lineto(&r, r.s[i], r.s[i + 1])) {
                    return false;
                }
            }
            if (!t2_curveto(&r, r.s[i], r.s[i + 1], r.s[i + 2],
                            r.s[i + 3], r.s[i + 4], r.s[i + 5])) {
                return false;
            }
            r.n = 0;
            continue;
        }
        case 26: { /* vvcurveto: dx1? {dya dxb dyb dyc}+ */
            if (r.n < 4 || (r.n % 4) > 1) {
                return false;
            }
            bool odd = (r.n % 4) == 1;
            fdk_f64 lead = odd ? r.s[0] : 0;
            int i = odd ? 1 : 0;
            bool first = true;
            for (; i + 3 < r.n; i += 4) {
                fdk_f64 l2 = first ? lead : 0;
                first = false;
                if (!t2_curveto(&r, l2, r.s[i], r.s[i + 1], r.s[i + 2],
                                0, r.s[i + 3])) {
                    return false;
                }
            }
            r.n = 0;
            continue;
        }
        case 27: { /* hhcurveto: dy1? {dxa dxb dyb dxc}+ */
            if (r.n < 4 || (r.n % 4) > 1) {
                return false;
            }
            bool odd = (r.n % 4) == 1;
            fdk_f64 lead = odd ? r.s[0] : 0;
            int i = odd ? 1 : 0;
            bool first = true;
            for (; i + 3 < r.n; i += 4) {
                fdk_f64 l2 = first ? lead : 0;
                first = false;
                if (!t2_curveto(&r, r.s[i], l2, r.s[i + 1], r.s[i + 2],
                                r.s[i + 3], 0)) {
                    return false;
                }
            }
            r.n = 0;
            continue;
        }
        case 30:   /* vhcurveto */
        case 31: { /* hvcurveto — 4-arg groups with alternating
                     tangent constraints; the optional trailing arg
                     frees the final end point along the operator's
                     own axis (x for hv, y for vh). */
            if (r.n < 4 || r.n % 4 > 1) {
                return false;
            }
            int groups = r.n / 4;
            bool extra = (r.n % 4) == 1;
            const double *s = r.s;
            size_t ai = 0;
            for (int g = 0; g < groups; g++) {
                /* First curve of vh starts vertical (c1 dx = 0) and
                 * ends horizontal (p dy = 0); hv mirrors; then the
                 * constraints alternate group by group. */
                bool start_v = (op == 30) ? (g % 2 == 0) : (g % 2 != 0);
                double a = s[ai], b = s[ai + 1], c = s[ai + 2],
                       d = s[ai + 3];
                ai += 4;
                fdk_f64 d1x, d1y, d2x, d2y, d3x, d3y;
                if (start_v) {
                    d1x = 0;  d1y = a;
                    d2x = b;  d2y = c;
                    d3x = d;  d3y = 0;
                } else {
                    d1x = a;  d1y = 0;
                    d2x = b;  d2y = c;
                    d3x = 0;  d3y = d;
                }
                if (extra && g == groups - 1) {
                    /* 5177 §4.1: "The last curve (the odd argument
                     * case) need not end horizontal/vertical" — the
                     * trailing delta relaxes the LAST group's end
                     * constraint along ITS axis: a v-start group
                     * ends horizontal (y pinned), so the trailing
                     * moves y; an h-start group ends vertical (x
                     * pinned), so it moves x. The axis follows the
                     * GROUP, never the operator — a two-group
                     * hvcurveto's last group is v-start, and the
                     * trailing is a dy there (C059's 'A' pins this
                     * live: the serif flex). */
                    fdk_f64 adj = s[r.n - 1];
                    if (start_v) {
                        d3y += adj;
                    } else {
                        d3x += adj;
                    }
                }
                if (!t2_curveto(&r, d1x, d1y, d2x, d2y, d3x, d3y)) {
                    return false;
                }
            }
            r.n = 0;
            continue;
        }
        case 10: { /* callsubr — LOCAL subrs of the current Private */
            if (r.n < 1) {
                return false;
            }
            fdk_i64 arg = (fdk_i64)r.s[--r.n];
            if (!t2_callsubr(&r, &r.priv->subrs, arg)) {
                return false;
            }
            continue;
        }
        case 29: { /* callgsubr — global subrs, own bias */
            if (r.n < 1) {
                return false;
            }
            fdk_i64 arg = (fdk_i64)r.s[--r.n];
            if (!t2_callsubr(&r, &r.f->gsubrs, arg)) {
                return false;
            }
            continue;
        }
        case 11: { /* return */
            if (r.depth == 0) {
                return t2_close_contour(
                    &r); /* top-level return ends the glyph */
            }
            r.depth--;
            continue;
        }
        case 14: { /* endchar (CFF1) */
            if (!t2_endchar(&r)) {
                return false;
            }
            return true; /* endchar always finishes the glyph (the
                            contour was closed inside t2_endchar,
                            before any seac composition) */
        }
        case 15: { /* CFF2 only: vsindex (records nothing — the
                     * default instance never applies deltas) */
            if (!f->is_cff2 || r.n < 1) {
                return false;
            }
            r.n--;
            continue;
        }
        case 16: { /* CFF2 only: blend — default instance == bases */
            if (!f->is_cff2) {
                return false;
            }
            if (r.n < 1) {
                return false;
            }
            fdk_i64 n = (fdk_i64)r.s[--r.n];
            if (n <= 0 || (fdk_i64)r.n < n) {
                return false;
            }
            /* Remaining stack: n bases then n*k deltas; k derives
             * from the count. Default coords (all zero): the result
             * is exactly the base operands — drop the deltas. */
            fdk_i64 rest = r.n - (int)n;
            if (rest % n != 0) {
                return false;
            }
            r.n -= (int)rest;
            continue;
        }
        case 12: { /* escape — two-byte operator space */
            fdk_u32 b1 = 0;
            if (!cff_u8(code, fr->pos, &b1)) {
                return false;
            }
            fr->pos++;
            switch (b1) {
            case 0: /* dotsection: a no-op since ATM (Appendix C) */
                r.n = 0;
                continue;
            case 3: { /* and */
                if (r.n < 2) return false;
                double a = r.s[--r.n], b = r.s[--r.n];
                if (!cff_push(&r, (a != 0 && b != 0) ? 1.0 : 0.0)) {
                    return false;
                }
                continue;
            }
            case 4: { /* or */
                if (r.n < 2) return false;
                double a = r.s[--r.n], b = r.s[--r.n];
                if (!cff_push(&r, (a != 0 || b != 0) ? 1.0 : 0.0)) {
                    return false;
                }
                continue;
            }
            case 5: { /* not */
                if (r.n < 1) return false;
                double a = r.s[--r.n];
                if (!cff_push(&r, a == 0 ? 1.0 : 0.0)) {
                    return false;
                }
                continue;
            }
            case 9: { /* abs */
                if (r.n < 1) return false;
                r.s[r.n - 1] = fabs(r.s[r.n - 1]);
                continue;
            }
            case 10: { /* add */
                if (r.n < 2) return false;
                double a = r.s[--r.n];
                r.s[r.n - 1] += a;
                continue;
            }
            case 11: { /* sub */
                if (r.n < 2) return false;
                double a = r.s[--r.n];
                r.s[r.n - 1] -= a;
                continue;
            }
            case 12: { /* div */
                if (r.n < 2) return false;
                double a = r.s[--r.n];
                if (a == 0.0) return false;
                r.s[r.n - 1] /= a;
                continue;
            }
            case 14: { /* neg */
                if (r.n < 1) return false;
                r.s[r.n - 1] = -r.s[r.n - 1];
                continue;
            }
            case 15: { /* eq */
                if (r.n < 2) return false;
                double a = r.s[--r.n], b = r.s[--r.n];
                if (!cff_push(&r, a == b ? 1.0 : 0.0)) {
                    return false;
                }
                continue;
            }
            case 18: { /* drop */
                if (r.n < 1) return false;
                r.n--;
                continue;
            }
            case 20: { /* put: v i — transient[i] = v */
                if (r.n < 2) return false;
                fdk_i64 i = (fdk_i64)r.s[--r.n];
                double v = r.s[--r.n];
                if (i < 0 || i >= CFF_T2_TRANSIENT) return false;
                r.t[i] = v;
                continue;
            }
            case 21: { /* get: i — push transient[i] */
                if (r.n < 1) return false;
                fdk_i64 i = (fdk_i64)r.s[--r.n];
                if (i < 0 || i >= CFF_T2_TRANSIENT) return false;
                if (!cff_push(&r, r.t[i])) {
                    return false;
                }
                continue;
            }
            case 22: { /* ifelse: v1 v2 v3 v4 */
                if (r.n < 4) return false;
                double v4 = r.s[--r.n], v3 = r.s[--r.n];
                double v2 = r.s[--r.n], v1 = r.s[--r.n];
                if (!cff_push(&r, v1 <= v2 ? v3 : v4)) {
                    return false;
                }
                continue;
            }
            case 23: { /* random — deterministic LCG (spec: fonts
                         * must not depend on the sequence; any value
                         * in [0, n) is conformant) */
                if (r.n < 1) return false;
                double n = r.s[r.n - 1];
                r.lcg = r.lcg * 6364136223846793005ULL +
                        1442695040888963407ULL;
                fdk_i64 v = (fdk_i64)((r.lcg >> 33) & 0x7FFFFFFFull);
                r.s[r.n - 1] = n > 0 ? fmod((double)v, n) : 0.0;
                continue;
            }
            case 24: { /* mul */
                if (r.n < 2) return false;
                double a = r.s[--r.n];
                r.s[r.n - 1] *= a;
                continue;
            }
            case 26: { /* sqrt */
                if (r.n < 1) return false;
                if (r.s[r.n - 1] < 0) return false;
                r.s[r.n - 1] = sqrt(r.s[r.n - 1]);
                continue;
            }
            case 27: { /* dup */
                if (r.n < 1) return false;
                if (!cff_push(&r, r.s[r.n - 1])) {
                    return false;
                }
                continue;
            }
            case 28: { /* exch */
                if (r.n < 2) return false;
                double a = r.s[r.n - 1];
                r.s[r.n - 1] = r.s[r.n - 2];
                r.s[r.n - 2] = a;
                continue;
            }
            case 29: { /* index */
                if (r.n < 1) return false;
                fdk_i64 i = (fdk_i64)r.s[r.n - 1];
                if (i < 0 || i >= r.n - 1) return false;
                r.s[r.n - 1] = r.s[r.n - 2 - i];
                continue;
            }
            case 30: { /* roll: i j — rotate the top i elements
                          up (toward the top) by j positions */
                if (r.n < 2) return false;
                fdk_i64 j = (fdk_i64)r.s[--r.n];
                fdk_i64 i = (fdk_i64)r.s[--r.n];
                if (i < 0 || i > r.n) return false;
                if (i > 1 && j != 0) {
                    double tmp[CFF_T2_STACK_MAX];
                    fdk_i64 mod = j % i;
                    if (mod < 0) mod += i;
                    for (fdk_i64 k = 0; k < i; k++) {
                        fdk_i64 src = ((k - mod) % i + i) % i;
                        tmp[(size_t)k] =
                            r.s[(size_t)(r.n - i) + (size_t)src];
                    }
                    for (fdk_i64 k = 0; k < i; k++) {
                        r.s[(size_t)(r.n - i) + (size_t)k] = tmp[k];
                    }
                }
                continue;
            }
            case 34: /* hflex */
                if (!t2_hflex(&r)) return false;
                continue;
            case 35: /* flex — 12 35 per 5177 §4.3 (NOT 36: the
                        four flex ops are 34 hflex, 35 flex,
                        36 hflex1, 37 flex1) */
                if (!t2_flex(&r)) return false;
                continue;
            case 36: /* hflex1 */
                if (!t2_hflex1(&r)) return false;
                continue;
            case 37: /* flex1 */
                if (!t2_flex1(&r)) return false;
                continue;
            case 16: { /* callothersubr (deprecated, Appendix C):
                         consume [args..., n, subr#] — hint-era use
                         only; no results are produced. */
                if (r.n < 2) return false;
                fdk_i64 subr = (fdk_i64)r.s[--r.n];
                fdk_i64 n = (fdk_i64)r.s[--r.n];
                if (n < 0 || (fdk_i64)r.n < n) return false;
                r.n -= (int)n;
                (void)subr;
                continue;
            }
            case 17: /* pop (deprecated): pushes a placeholder result */
                if (!cff_push(&r, 0.0)) {
                    return false;
                }
                continue;
            default:
                return false; /* undefined escape operator */
            }
        }
        default:
            /* 0, 2, 9, 13, 17 and (CFF1) 15/16 are undefined. */
            return false;
        }
    }
}

/* =====================================================================
 * 9. Public module surface (declared in text_internal.h)
 * ===================================================================== */

/* The Private DICT serving a glyph: FDSelect picks the Font DICT for
 * CID-keyed/CFF2 fonts; everything else uses privs[0]. */
static const cff_private *cff_private_for_gid(const fdk_cff_font *f,
                                              size_t gid) {
    if (f->n_privs == 0) {
        return NULL;
    }
    int fd = 0;
    if (f->fdarray.count > 1 && f->has_fdselect) {
        fd = cff_fdselect_lookup(f, gid);
        if (fd < 0 || fd >= f->n_privs) {
            fd = 0; /* malformed FDSelect: fall back to FD 0 */
        }
    }
    return &f->privs[fd < f->n_privs ? fd : 0];
}

/* The Font DICT walk: one cff_topdump per Font DICT, collecting its
 * Private [size, offset]. */
typedef struct {
    fdk_cff_font *f;
    size_t priv_off, priv_size;
    bool have;
} cff_fddump;

static bool cff_fd_cb(const cff_args *a, fdk_u32 op, bool escaped,
                      void *user) {
    cff_fddump *d = user;
    if (!escaped && op == 18 && a->n >= 2) { /* Private in a Font DICT */
        d->priv_size = (size_t)(fdk_i64)cff_arg(a, 0);
        d->priv_off = (size_t)(fdk_i64)cff_arg(a, 1);
        d->have = true;
    }
    return true; /* FontName (12 38) and friends: metadata */
}

/* Forward (parse failure paths free through it). */
void fdk__cff_destroy(fdk_cff_font *f);

fdk_cff_font *fdk__cff_parse(const fdk_u8 *file, size_t file_size,
                             size_t face_off, size_t table_off,
                             size_t table_len, int is_cff2,
                             int units_per_em) {
    (void)face_off; /* offsets are all table-relative per spec */
    if (file == NULL || table_off > file_size ||
        table_len > file_size - table_off || table_len < 2) {
        return NULL;
    }
    if (units_per_em < 16 || units_per_em > 16384) {
        return NULL; /* OpenType head sanity, enforced early */
    }

    fdk_cff_font *f = fdk_alloc(sizeof(fdk_cff_font));
    if (f == NULL) {
        return NULL;
    }
    memset(f, 0, sizeof(*f));
    f->table.p = file + table_off;
    f->table.len = table_len;
    f->charset_kind = 0xFFFFFFFFu; /* offset until proven predefined */

    /* Header: major(1) minor(1) hdrSize(1) then CFF1 offSize(1) or
     * CFF2 topDictLength(2). The major version is authoritative. */
    fdk_u32 major = 0, hdr_size = 0;
    if (!cff_u8(&f->table, 0, &major) ||
        !cff_u8(&f->table, 2, &hdr_size)) {
        fdk_free(f);
        return NULL;
    }
    if (major == 2) {
        f->is_cff2 = 1;
        if (!is_cff2) {
            /* 'CFF ' table carrying CFF2 data: treat as malformed. */
            fdk_free(f);
            return NULL;
        }
    } else if (major == 1) {
        if (is_cff2) {
            fdk_free(f);
            return NULL;
        }
    } else {
        fdk_free(f);
        return NULL;
    }
    if (hdr_size < 4 || (size_t)hdr_size > table_len) {
        fdk_free(f);
        return NULL;
    }

    /* The fixed INDEX sequence after the header (CFF spec §4):
     * CFF1: Name, Top DICT, String, Global Subr. CFF2: Top DICT,
     * Global Subr. Each INDEX's own validated extent advances the
     * walk. */
    size_t pos = hdr_size;
    cff_rng idx;
    cff_index top;
    memset(&top, 0, sizeof(top));
    if (!cff_sub(&f->table, pos, table_len - pos, &idx)) {
        fdk_free(f);
        return NULL;
    }
    if (!f->is_cff2) { /* Name INDEX (kept for shape diagnostics) */
        if (!cff_index_parse(&idx, 0, &f->name_index)) {
            fdk_free(f);
            return NULL;
        }
        pos += f->name_index.extent;
        if (pos > table_len ||
            !cff_sub(&f->table, pos, table_len - pos, &idx)) {
            fdk_free(f);
            return NULL;
        }
    }
    { /* Top DICT INDEX */
        if (!cff_index_parse(&idx, f->is_cff2, &top)) {
            fdk_free(f);
            return NULL;
        }
        cff_rng topdict;
        if (top.count < 1 || !cff_index_item(&top, 0, &topdict)) {
            fdk_free(f);
            return NULL;
        }
        cff_topdump td;
        memset(&td, 0, sizeof(td));
        td.f = f;
        td.depth = 0;
        if (!cff_dict_walk(&topdict, cff_top_cb, &td) ||
            td.charstrings_off == 0) {
            fdk_free(f);
            return NULL;
        }
        f->is_cid = td.saw_ros;
        if (td.have_fontmatrix) {
            fdk_f64 upem = (fdk_f64)units_per_em;
            f->mx[0] = td.fontmatrix[0] * upem;
            f->mx[1] = td.fontmatrix[1] * upem;
            f->mx[2] = td.fontmatrix[2] * upem;
            f->mx[3] = td.fontmatrix[3] * upem;
            f->mx[4] = td.fontmatrix[4] * upem;
            f->mx[5] = td.fontmatrix[5] * upem;
        } else {
            /* Default FontMatrix 0.001 scaled by upem = identity. */
            f->mx[0] = 1.0; f->mx[1] = 0.0;
            f->mx[2] = 0.0; f->mx[3] = 1.0;
            f->mx[4] = 0.0; f->mx[5] = 0.0;
        }

        /* CharStrings INDEX (table-relative). */
        if (!cff_sub(&f->table, td.charstrings_off,
                     table_len - td.charstrings_off, &idx) ||
            !cff_index_parse(&idx, f->is_cff2, &f->charstrings) ||
            f->charstrings.count == 0) {
            fdk_free(f);
            return NULL;
        }

        /* charset (CFF1; CFF2 has none). */
        if (!f->is_cff2) {
            if (td.charset_op <= 2) {
                f->charset_kind = (fdk_u32)td.charset_op;
            } else if (td.charset_op >= table_len) {
                fdk_free(f);
                return NULL;
            } else {
                f->charset_off = td.charset_op;
            }
        }

        /* Font DICTs / Private DICTs. */
        if (td.fdarray_off != 0) {
            if (td.fdarray_off >= table_len) {
                fdk_free(f);
                return NULL;
            }
            if (!cff_sub(&f->table, td.fdarray_off,
                         table_len - td.fdarray_off, &idx) ||
                !cff_index_parse(&idx, f->is_cff2, &f->fdarray) ||
                f->fdarray.count == 0) {
                fdk_free(f);
                return NULL;
            }
            f->privs = fdk_alloc_array(f->fdarray.count,
                                       sizeof(cff_private));
            if (f->privs == NULL) {
                fdk_free(f);
                return NULL;
            }
            f->n_privs = (int)f->fdarray.count;
            for (size_t i = 0; i < f->fdarray.count; i++) {
                cff_rng fd;
                if (!cff_index_item(&f->fdarray, i, &fd)) {
                    fdk__cff_destroy(f);
                    return NULL;
                }
                cff_fddump dd;
                memset(&dd, 0, sizeof(dd));
                dd.f = f;
                if (!cff_dict_walk(&fd, cff_fd_cb, &dd)) {
                    fdk__cff_destroy(f);
                    return NULL;
                }
                if (dd.have) {
                    if (dd.priv_off >= table_len ||
                        dd.priv_size > table_len - dd.priv_off ||
                        !cff_private_parse(f, dd.priv_off, dd.priv_size,
                                           &f->privs[i])) {
                        fdk__cff_destroy(f);
                        return NULL;
                    }
                }
            }
            if (td.have_fdselect) {
                if (td.fdselect_off >= table_len) {
                    fdk__cff_destroy(f);
                    return NULL;
                }
                if (!cff_sub(&f->table, td.fdselect_off,
                             table_len - td.fdselect_off, &f->fdselect)) {
                    fdk__cff_destroy(f);
                    return NULL;
                }
                f->has_fdselect = true;
                if (!cff_fdselect_validate(f)) {
                    fdk__cff_destroy(f);
                    return NULL;
                }
            } else if (f->fdarray.count > 1) {
                /* Multiple FDs with no FDSelect: CID fonts must map
                 * glyphs to FDs; CFF2 says FDSelect is REQUIRED when
                 * FDArray has more than one Font DICT. */
                fdk__cff_destroy(f);
                return NULL;
            }
        } else {
            /* Plain CFF1: the Top DICT's own Private (CFF2 requires
             * an FDArray, so a CFF2 without one is malformed). */
            if (f->is_cff2) {
                fdk_free(f);
                return NULL;
            }
            if (td.have_private && td.private_off < table_len &&
                td.private_size <= table_len - td.private_off) {
                f->privs = fdk_alloc_array(1, sizeof(cff_private));
                if (f->privs == NULL) {
                    fdk_free(f);
                    return NULL;
                }
                f->n_privs = 1;
                if (!cff_private_parse(f, td.private_off, td.private_size,
                                       &f->privs[0])) {
                    fdk_free(f->privs);
                    f->privs = NULL;
                    f->n_privs = 0;
                    fdk_free(f);
                    return NULL;
                }
            }
        }

        /* CID-keyed fonts carry ROS and must route through an
         * FDArray (their Private DICTs live in Font DICTs). */
        if (f->is_cid && td.fdarray_off == 0) {
            fdk_free(f);
            return NULL;
        }
    }

    /* String INDEX (CFF1) then Global Subrs INDEX — both follow the
     * Top DICT INDEX at pos (already advanced past Name). */
    pos += top.extent;
    if (pos > table_len) {
        fdk__cff_destroy(f);
        return NULL;
    }
    if (!f->is_cff2) {
        if (pos == table_len) {
            fdk__cff_destroy(f);
            return NULL; /* CFF1 requires the (possibly empty) slot */
        }
        if (!cff_sub(&f->table, pos, table_len - pos, &idx) ||
            !cff_index_parse(&idx, 0, &f->strings)) {
            fdk__cff_destroy(f);
            return NULL;
        }
        pos += f->strings.extent;
    }
    if (pos < table_len) { /* Global Subrs INDEX (empty slot is legal) */
        if (!cff_sub(&f->table, pos, table_len - pos, &idx) ||
            !cff_index_parse(&idx, f->is_cff2, &f->gsubrs)) {
            fdk__cff_destroy(f);
            return NULL;
        }
    }
    return f;
}

void fdk__cff_destroy(fdk_cff_font *f) {
    if (f == NULL) {
        return;
    }
    fdk_free(f->privs);
    fdk_free(f->gid_to_sid);
    fdk_free(f);
}

int fdk__cff_glyph_count(const fdk_cff_font *f) {
    return f == NULL ? 0 : (int)f->charstrings.count;
}

int fdk__cff_glyph_shape(fdk_cff_font *f, int gid, stbtt_vertex **out) {
    if (out == NULL) {
        return -1;
    }
    *out = NULL;
    if (f == NULL || gid < 0 || (size_t)gid >= f->charstrings.count) {
        return -1;
    }
    cff_rng cs;
    if (!cff_index_item(&f->charstrings, (size_t)gid, &cs)) {
        return -1;
    }
    cff_sink sink;
    memset(&sink, 0, sizeof(sink));
    const cff_private *priv = cff_private_for_gid(f, (size_t)gid);
    if (priv == NULL) {
        return -1; /* no Private DICT at all: subrs-less fonts still
                      carry one; absence is malformed */
    }
    if (!cff_interpret(f, &cs, priv, 0.0, 0.0, &sink, 0)) {
        fdk_free(sink.v);
        return -1;
    }
    *out = sink.v; /* NULL iff the glyph has no ink (space) */
    return sink.nv;
}

bool fdk__cff_glyph_box(fdk_cff_font *f, int gid, int *x0, int *y0,
                        int *x1, int *y1) {
    if (f == NULL || gid < 0 || (size_t)gid >= f->charstrings.count) {
        return false;
    }
    cff_rng cs;
    if (!cff_index_item(&f->charstrings, (size_t)gid, &cs)) {
        return false;
    }
    cff_sink sink;
    memset(&sink, 0, sizeof(sink));
    sink.dry = 1;
    const cff_private *priv = cff_private_for_gid(f, (size_t)gid);
    if (priv == NULL) {
        return false;
    }
    if (!cff_interpret(f, &cs, priv, 0.0, 0.0, &sink, 0) ||
        !sink.has_ink) {
        return false; /* empty or malformed: no box */
    }
    if (x0) *x0 = (int)floor(sink.bx0);
    if (y0) *y0 = (int)floor(sink.by0);
    if (x1) *x1 = (int)ceil(sink.bx1);
    if (y1) *y1 = (int)ceil(sink.by1);
    return true;
}
