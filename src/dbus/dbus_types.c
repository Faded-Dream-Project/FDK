/*
 * dbus_types.c — the fdk_dbus_value tree (1.1.0)
 *
 * Constructors, cloning, and freeing of the public argument model.
 * Every constructor VALIDATES what the wire would validate anyway
 * (object-path / signature grammar, dict keys basic, variant holds
 * one child) so that marshaling a constructed tree can trust it —
 * the marshaler's job is byte layout, not re-validation. The
 * accessors are type-checked with a benign default + one DEBUG line
 * per call site category, never a crash: a mismatched read against
 * data the BUS sent is survivable by policy (default + log), and a
 * mismatched read against app-side data is a bug we name loudly.
 */

#define FDK_LOG_TAG "dbus"

#include "dbus/dbus_internal.h"

#include <string.h>

/* ---- shared small helpers ----------------------------------------------- */

static char *dup_str(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *copy = fdk_alloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

static fdk_dbus_value *value_new(uint8_t type) {
    fdk_dbus_value *v = fdk_alloc(sizeof(*v));
    if (v == NULL) {
        return NULL;
    }
    v->type = type;
    memset(&v->v, 0, sizeof(v->v));
    return v;
}

/* ---- constructors -------------------------------------------------------- */

fdk_dbus_value *fdk_dbus_value_new_byte(uint8_t value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_BYTE);
    if (v != NULL) {
        v->v.byte_ = value;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_boolean(int value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_BOOLEAN);
    if (v != NULL) {
        v->v.boolean_ = value ? 1 : 0;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_int16(int16_t value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_INT16);
    if (v != NULL) {
        v->v.i16 = value;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_uint16(uint16_t value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_UINT16);
    if (v != NULL) {
        v->v.u16 = value;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_int32(int32_t value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_INT32);
    if (v != NULL) {
        v->v.i32 = value;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_uint32(uint32_t value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_UINT32);
    if (v != NULL) {
        v->v.u32 = value;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_int64(int64_t value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_INT64);
    if (v != NULL) {
        v->v.i64 = value;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_uint64(uint64_t value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_UINT64);
    if (v != NULL) {
        v->v.u64 = value;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_double(double value) {
    fdk_dbus_value *v = value_new(FDK_DBUS_DOUBLE);
    if (v != NULL) {
        v->v.dbl = value;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_string(const char *value) {
    if (value == NULL) {
        return NULL;
    }
    /* The wire requires valid UTF-8 with no embedded NUL; reject at
     * construction so marshaling can trust the tree. */
    if (!fdk__dbus_valid_utf8(value, strlen(value))) {
        FDK_WARN("fdk_dbus_value_new_string: not valid UTF-8 — rejected");
        return NULL;
    }
    fdk_dbus_value *v = value_new(FDK_DBUS_STRING);
    if (v == NULL) {
        return NULL;
    }
    v->v.str = dup_str(value);
    if (v->v.str == NULL) {
        fdk_free(v);
        return NULL;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_object_path(const char *value) {
    if (value == NULL || !fdk__dbus_valid_object_path(value)) {
        if (value != NULL) {
            FDK_WARN("fdk_dbus_value_new_object_path: '%s' is not a valid "
                     "object path — rejected",
                     value);
        }
        return NULL;
    }
    fdk_dbus_value *v = value_new(FDK_DBUS_OBJECT_PATH);
    if (v == NULL) {
        return NULL;
    }
    v->v.str = dup_str(value);
    if (v->v.str == NULL) {
        fdk_free(v);
        return NULL;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_signature(const char *value) {
    if (value == NULL || *value == '\0' ||
        strlen(value) > FDK_DBUS_SIGNATURE_MAX ||
        !fdk__dbus_valid_signature(value, FDK_DBUS_SIGNATURE_MAX)) {
        if (value != NULL) {
            FDK_WARN("fdk_dbus_value_new_signature: invalid signature — "
                     "rejected");
        }
        return NULL;
    }
    fdk_dbus_value *v = value_new(FDK_DBUS_SIGNATURE);
    if (v == NULL) {
        return NULL;
    }
    v->v.str = dup_str(value);
    if (v->v.str == NULL) {
        fdk_free(v);
        return NULL;
    }
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_variant(const fdk_dbus_value *child) {
    if (child == NULL) {
        return NULL;
    }
    fdk_dbus_value *inner = fdk_dbus_value_clone(child);
    if (inner == NULL) {
        return NULL;
    }
    char *sig = fdk_dbus_value_signature(child);
    if (sig == NULL) {
        fdk_dbus_value_free(inner);
        return NULL;
    }
    if (!fdk__dbus_signature_is_single(sig)) {
        /* Cannot happen for a constructed/parsed node (its own
         * signature is one complete type by construction) — but a
         * future node kind or an app-side hand-build would make it
         * possible; reject rather than marshal nonsense. */
        FDK_WARN("fdk_dbus_value_new_variant: child signature '%s' is not "
                 "a single complete type — rejected",
                 sig);
        fdk_free(sig);
        fdk_dbus_value_free(inner);
        return NULL;
    }
    fdk_dbus_value *v = value_new(FDK_DBUS_VARIANT);
    if (v == NULL) {
        fdk_free(sig);
        fdk_dbus_value_free(inner);
        return NULL;
    }
    v->v.variant.child = inner;
    v->v.variant.sig = sig;
    return v;
}

static int array_items_compatible(const char *elem_sig,
                                   fdk_dbus_value **items, size_t count) {
    /* Every item's own signature must equal the array's element
     * signature (the wire has no per-item tags inside an array). */
    for (size_t i = 0; i < count; i++) {
        char *is = fdk_dbus_value_signature(items[i]);
        if (is == NULL) {
            return 0;
        }
        int equal = strcmp(is, elem_sig) == 0;
        fdk_free(is);
        if (!equal) {
            return 0;
        }
    }
    return 1;
}

fdk_dbus_value *fdk_dbus_value_new_array(const char *elem_sig,
                                         fdk_dbus_value **items,
                                         size_t count) {
    if (elem_sig == NULL || !fdk__dbus_signature_is_single(elem_sig)) {
        FDK_WARN("fdk_dbus_value_new_array: element signature must be one "
                 "complete type — rejected");
        return NULL;
    }
    if (count != 0 && items == NULL) {
        return NULL;
    }
    if (count > 0 && !array_items_compatible(elem_sig, items, count)) {
        FDK_WARN("fdk_dbus_value_new_array: an item does not match the "
                 "element signature — rejected");
        return NULL;
    }
    fdk_dbus_value *v = value_new(FDK_DBUS_ARRAY);
    if (v == NULL) {
        return NULL;
    }
    v->v.array.elem_sig = dup_str(elem_sig);
    if (v->v.array.elem_sig == NULL) {
        fdk_free(v);
        return NULL;
    }
    if (count > 0) {
        v->v.array.items = fdk_alloc_array(count, sizeof(*items));
        if (v->v.array.items == NULL) {
            fdk_free(v->v.array.elem_sig);
            fdk_free(v);
            return NULL;
        }
        memcpy(v->v.array.items, items, count * sizeof(*items));
    }
    v->v.array.n = count;
    /* Ownership of the POINTED-TO values transfers to the array. */
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_byte_array(const uint8_t *data,
                                              size_t count) {
    if (count > 0 && data == NULL) {
        return NULL;
    }
    if (count > FDK_DBUS_MAX_MESSAGE_DEFAULT) {
        FDK_WARN("fdk_dbus_value_new_byte_array: %zu bytes over the %u "
                 "MiB message cap — rejected",
                 count, FDK_DBUS_MAX_MESSAGE_DEFAULT / (1024u * 1024u));
        return NULL;
    }
    fdk_dbus_value *v = value_new(FDK_DBUS_ARRAY);
    if (v == NULL) {
        return NULL;
    }
    v->v.array.elem_sig = dup_str("y");
    if (v->v.array.elem_sig == NULL) {
        fdk_free(v);
        return NULL;
    }
    if (count > 0) {
        v->v.array.flat = fdk_alloc_array(count, 1);
        if (v->v.array.flat == NULL) {
            fdk_free(v->v.array.elem_sig);
            fdk_free(v);
            return NULL;
        }
        memcpy(v->v.array.flat, data, count);
    }
    v->v.array.n = count;
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_struct(fdk_dbus_value **items,
                                          size_t count) {
    if (count != 0 && items == NULL) {
        return NULL;
    }
    fdk_dbus_value *v = value_new(FDK_DBUS_STRUCT);
    if (v == NULL) {
        return NULL;
    }
    if (count > 0) {
        v->v.structure.items = fdk_alloc_array(count, sizeof(*items));
        if (v->v.structure.items == NULL) {
            fdk_free(v);
            return NULL;
        }
        memcpy(v->v.structure.items, items, count * sizeof(*items));
    }
    v->v.structure.n = count;
    return v;
}

fdk_dbus_value *fdk_dbus_value_new_dict_entry(fdk_dbus_value *key,
                                              fdk_dbus_value *value) {
    if (key == NULL || value == NULL) {
        return NULL;
    }
    switch (key->type) {
    case FDK_DBUS_BYTE:
    case FDK_DBUS_BOOLEAN:
    case FDK_DBUS_INT16:
    case FDK_DBUS_UINT16:
    case FDK_DBUS_INT32:
    case FDK_DBUS_UINT32:
    case FDK_DBUS_INT64:
    case FDK_DBUS_UINT64:
    case FDK_DBUS_DOUBLE:
    case FDK_DBUS_STRING:
    case FDK_DBUS_OBJECT_PATH:
    case FDK_DBUS_SIGNATURE:
        break; /* basic — legal key */
    default:
        FDK_WARN("fdk_dbus_value_new_dict_entry: key must be a basic type "
                 "— rejected");
        return NULL;
    }
    fdk_dbus_value *v = value_new(FDK_DBUS_DICT_ENTRY);
    if (v == NULL) {
        return NULL;
    }
    v->v.structure.items = fdk_alloc_array(2, sizeof(fdk_dbus_value *));
    if (v->v.structure.items == NULL) {
        fdk_free(v);
        return NULL;
    }
    v->v.structure.items[0] = key;
    v->v.structure.items[1] = value;
    v->v.structure.n = 2;
    return v;
}

/* ---- clone / free -------------------------------------------------------- */

fdk_dbus_value *fdk_dbus_value_clone(const fdk_dbus_value *value) {
    if (value == NULL) {
        return NULL;
    }
    switch (value->type) {
    case FDK_DBUS_BYTE:
        return fdk_dbus_value_new_byte(value->v.byte_);
    case FDK_DBUS_BOOLEAN:
        return fdk_dbus_value_new_boolean(value->v.boolean_);
    case FDK_DBUS_INT16:
        return fdk_dbus_value_new_int16(value->v.i16);
    case FDK_DBUS_UINT16:
        return fdk_dbus_value_new_uint16(value->v.u16);
    case FDK_DBUS_INT32:
        return fdk_dbus_value_new_int32(value->v.i32);
    case FDK_DBUS_UINT32:
        return fdk_dbus_value_new_uint32(value->v.u32);
    case FDK_DBUS_INT64:
        return fdk_dbus_value_new_int64(value->v.i64);
    case FDK_DBUS_UINT64:
        return fdk_dbus_value_new_uint64(value->v.u64);
    case FDK_DBUS_DOUBLE:
        return fdk_dbus_value_new_double(value->v.dbl);
    case FDK_DBUS_STRING:
        return fdk_dbus_value_new_string(value->v.str);
    case FDK_DBUS_OBJECT_PATH:
        return fdk_dbus_value_new_object_path(value->v.str);
    case FDK_DBUS_SIGNATURE:
        return fdk_dbus_value_new_signature(value->v.str);
    case FDK_DBUS_VARIANT: {
        fdk_dbus_value *v = value_new(FDK_DBUS_VARIANT);
        if (v == NULL) {
            return NULL;
        }
        v->v.variant.child = fdk_dbus_value_clone(value->v.variant.child);
        v->v.variant.sig = dup_str(value->v.variant.sig);
        if (v->v.variant.child == NULL || v->v.variant.sig == NULL) {
            fdk_dbus_value_free(v);
            return NULL;
        }
        return v;
    }
    case FDK_DBUS_ARRAY: {
        fdk_dbus_value *v = value_new(FDK_DBUS_ARRAY);
        if (v == NULL) {
            return NULL;
        }
        v->v.array.elem_sig = dup_str(value->v.array.elem_sig);
        if (v->v.array.elem_sig == NULL) {
            fdk_free(v);
            return NULL;
        }
        if (value->v.array.flat != NULL) {
            v->v.array.flat = fdk_alloc_array(value->v.array.n, 1);
            if (v->v.array.flat == NULL) {
                fdk_free(v->v.array.elem_sig);
                fdk_free(v);
                return NULL;
            }
            memcpy(v->v.array.flat, value->v.array.flat,
                   value->v.array.n);
            v->v.array.n = value->v.array.n;
            return v;
        }
        if (value->v.array.n > 0 &&
            (v->v.array.items = fdk_alloc_array(
                 value->v.array.n, sizeof(fdk_dbus_value *))) == NULL) {
            fdk_free(v->v.array.elem_sig);
            fdk_free(v);
            return NULL;
        }
        for (size_t i = 0; i < value->v.array.n; i++) {
            v->v.array.items[i] =
                fdk_dbus_value_clone(value->v.array.items[i]);
            if (v->v.array.items[i] == NULL) {
                v->v.array.n = i; /* free what succeeded so far */
                fdk_dbus_value_free(v);
                return NULL;
            }
        }
        v->v.array.n = value->v.array.n;
        return v;
    }
    case FDK_DBUS_STRUCT:
    case FDK_DBUS_DICT_ENTRY: {
        fdk_dbus_value *v = value_new(value->type);
        if (v == NULL) {
            return NULL;
        }
        if (value->v.structure.n > 0) {
            v->v.structure.items = fdk_alloc_array(
                value->v.structure.n, sizeof(fdk_dbus_value *));
            if (v->v.structure.items == NULL) {
                fdk_free(v);
                return NULL;
            }
            for (size_t i = 0; i < value->v.structure.n; i++) {
                v->v.structure.items[i] =
                    fdk_dbus_value_clone(value->v.structure.items[i]);
                if (v->v.structure.items[i] == NULL) {
                    for (size_t k = 0; k < i; k++) {
                        fdk_dbus_value_free(v->v.structure.items[k]);
                    }
                    fdk_free(v->v.structure.items);
                    fdk_free(v);
                    return NULL;
                }
            }
        }
        v->v.structure.n = value->v.structure.n;
        return v;
    }
    default:
        return NULL;
    }
}

void fdk_dbus_value_free(fdk_dbus_value *value) {
    if (value == NULL) {
        return;
    }
    switch (value->type) {
    case FDK_DBUS_STRING:
    case FDK_DBUS_OBJECT_PATH:
    case FDK_DBUS_SIGNATURE:
        fdk_free(value->v.str);
        break;
    case FDK_DBUS_VARIANT:
        fdk_free(value->v.variant.sig);
        fdk_dbus_value_free(value->v.variant.child);
        break;
    case FDK_DBUS_ARRAY:
        fdk_free(value->v.array.elem_sig);
        if (value->v.array.flat != NULL) {
            fdk_free(value->v.array.flat);
        } else {
            for (size_t i = 0; i < value->v.array.n; i++) {
                fdk_dbus_value_free(value->v.array.items[i]);
            }
            fdk_free(value->v.array.items);
        }
        break;
    case FDK_DBUS_STRUCT:
    case FDK_DBUS_DICT_ENTRY:
        for (size_t i = 0; i < value->v.structure.n; i++) {
            fdk_dbus_value_free(value->v.structure.items[i]);
        }
        fdk_free(value->v.structure.items);
        break;
    default:
        break; /* basics carry nothing owned */
    }
    fdk_free(value);
}

void fdk_dbus_values_free(fdk_dbus_value **values, size_t count) {
    if (values == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fdk_dbus_value_free(values[i]);
    }
}

void fdk_dbus_values_free_array(fdk_dbus_value **values, size_t count) {
    if (values == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fdk_dbus_value_free(values[i]);
    }
    fdk_free(values);
}

/* ---- accessors ----------------------------------------------------------- */

static int wrong_type(const fdk_dbus_value *value, fdk_dbus_type want) {
    if (value == NULL) {
        return 1;
    }
    if (value->type != (uint8_t)want) {
        FDK_DEBUG("fdk_dbus_value accessor: type %c read as %c — returning "
                  "default",
                  (char)value->type, (char)want);
        return 1;
    }
    return 0;
}

fdk_dbus_type fdk_dbus_value_type(const fdk_dbus_value *value) {
    return (value == NULL) ? FDK_DBUS_TYPE_INVALID
                           : (fdk_dbus_type)value->type;
}

uint8_t fdk_dbus_value_byte(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_BYTE) ? 0 : value->v.byte_;
}

int fdk_dbus_value_boolean(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_BOOLEAN) ? 0 : value->v.boolean_;
}

int16_t fdk_dbus_value_int16(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_INT16) ? 0 : value->v.i16;
}

uint16_t fdk_dbus_value_uint16(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_UINT16) ? 0 : value->v.u16;
}

int32_t fdk_dbus_value_int32(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_INT32) ? 0 : value->v.i32;
}

uint32_t fdk_dbus_value_uint32(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_UINT32) ? 0 : value->v.u32;
}

int64_t fdk_dbus_value_int64(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_INT64) ? 0 : value->v.i64;
}

uint64_t fdk_dbus_value_uint64(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_UINT64) ? 0 : value->v.u64;
}

double fdk_dbus_value_double(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_DOUBLE) ? 0.0 : value->v.dbl;
}

const char *fdk_dbus_value_string(const fdk_dbus_value *value) {
    if (wrong_type(value, FDK_DBUS_STRING) &&
        wrong_type(value, FDK_DBUS_OBJECT_PATH) &&
        wrong_type(value, FDK_DBUS_SIGNATURE)) {
        return NULL;
    }
    return value->v.str;
}

const char *fdk_dbus_value_array_signature(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_ARRAY) ? NULL
                                             : value->v.array.elem_sig;
}

size_t fdk_dbus_value_array_count(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_ARRAY) ? 0 : value->v.array.n;
}

const fdk_dbus_value *fdk_dbus_value_array_item(const fdk_dbus_value *value,
                                                size_t index) {
    if (wrong_type(value, FDK_DBUS_ARRAY) || index >= value->v.array.n) {
        return NULL;
    }
    if (value->v.array.flat != NULL) {
        FDK_DEBUG("fdk_dbus_value_array_item: flat byte array — read it "
                  "with fdk_dbus_value_array_flat_bytes()");
        return NULL;
    }
    return value->v.array.items[index];
}

const uint8_t *fdk_dbus_value_array_flat_bytes(const fdk_dbus_value *value,
                                               size_t *count) {
    if (wrong_type(value, FDK_DBUS_ARRAY) || value->v.array.flat == NULL) {
        if (count != NULL) {
            *count = 0;
        }
        return NULL;
    }
    if (count != NULL) {
        *count = value->v.array.n;
    }
    return value->v.array.flat;
}

const char *fdk_dbus_value_variant_signature(const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_VARIANT) ? NULL
                                               : value->v.variant.sig;
}

const fdk_dbus_value *fdk_dbus_value_variant_value(
    const fdk_dbus_value *value) {
    return wrong_type(value, FDK_DBUS_VARIANT) ? NULL
                                               : value->v.variant.child;
}

size_t fdk_dbus_value_struct_count(const fdk_dbus_value *value) {
    if (value == NULL ||
        (value->type != FDK_DBUS_STRUCT &&
         value->type != FDK_DBUS_DICT_ENTRY)) {
        return 0;
    }
    return value->v.structure.n;
}

const fdk_dbus_value *fdk_dbus_value_struct_item(const fdk_dbus_value *value,
                                                 size_t index) {
    if (value == NULL ||
        (value->type != FDK_DBUS_STRUCT &&
         value->type != FDK_DBUS_DICT_ENTRY) ||
        index >= value->v.structure.n) {
        return NULL;
    }
    return value->v.structure.items[index];
}

const fdk_dbus_value *fdk_dbus_value_dict_key(const fdk_dbus_value *entry) {
    if (entry == NULL || entry->type != FDK_DBUS_DICT_ENTRY ||
        entry->v.structure.n != 2) {
        return NULL;
    }
    return entry->v.structure.items[0];
}

const fdk_dbus_value *fdk_dbus_value_dict_value(const fdk_dbus_value *entry) {
    if (entry == NULL || entry->type != FDK_DBUS_DICT_ENTRY ||
        entry->v.structure.n != 2) {
        return NULL;
    }
    return entry->v.structure.items[1];
}

const fdk_dbus_value *fdk_dbus_value_dict_lookup(const fdk_dbus_value *array,
                                                 const char *key) {
    if (array == NULL || array->type != FDK_DBUS_ARRAY || key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < array->v.array.n; i++) {
        const fdk_dbus_value *e = array->v.array.items[i];
        if (e == NULL || e->type != FDK_DBUS_DICT_ENTRY ||
            e->v.structure.n != 2) {
            continue;
        }
        const fdk_dbus_value *k = e->v.structure.items[0];
        if (k != NULL && k->type == FDK_DBUS_STRING &&
            strcmp(k->v.str, key) == 0) {
            return e->v.structure.items[1];
        }
    }
    return NULL;
}

/* ---- signature rendering --------------------------------------------------- */

/* Appends the value's signature chars to a growable buffer. */
struct sigbuf {
    char *buf;
    size_t len;
    size_t cap;
    int ok;
};

static void sigbuf_put(struct sigbuf *s, char c) {
    if (!s->ok) {
        return;
    }
    if (s->len + 1 >= s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        char *grown = fdk_realloc(s->buf, cap);
        if (grown == NULL) {
            s->ok = 0;
            return;
        }
        s->buf = grown;
        s->cap = cap;
    }
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}

static void value_sig(const fdk_dbus_value *v, struct sigbuf *s) {
    if (v == NULL || !s->ok) {
        return;
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
    case FDK_DBUS_STRING:
    case FDK_DBUS_OBJECT_PATH:
    case FDK_DBUS_SIGNATURE:
    case FDK_DBUS_VARIANT:
        sigbuf_put(s, (char)v->type);
        return;
    case FDK_DBUS_ARRAY: {
        sigbuf_put(s, 'a');
        const char *es = v->v.array.elem_sig;
        if (es == NULL) {
            s->ok = 0;
            return;
        }
        for (const char *p = es; *p != '\0'; p++) {
            sigbuf_put(s, *p);
        }
        return;
    }
    case FDK_DBUS_STRUCT:
    case FDK_DBUS_DICT_ENTRY: {
        sigbuf_put(s, v->type == FDK_DBUS_STRUCT ? '(' : '{');
        for (size_t i = 0; i < v->v.structure.n; i++) {
            value_sig(v->v.structure.items[i], s);
            if (!s->ok) {
                return;
            }
        }
        sigbuf_put(s, v->type == FDK_DBUS_STRUCT ? ')' : '}');
        return;
    }
    default:
        s->ok = 0; /* invalid node — refuse rather than emit junk */
    }
}

char *fdk_dbus_value_signature(const fdk_dbus_value *value) {
    if (value == NULL) {
        return NULL;
    }
    struct sigbuf s = {NULL, 0, 0, 1};
    value_sig(value, &s);
    if (!s.ok) {
        fdk_free(s.buf);
        return NULL;
    }
    if (s.buf == NULL) { /* empty is not representable here */
        return NULL;
    }
    return s.buf;
}
