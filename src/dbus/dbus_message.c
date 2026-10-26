/*
 * dbus_message.c — message construction, framing, and the
 * untrusted-input parser (1.1.0)
 *
 * Two halves:
 *
 *   BUILD: the constructors validate every name against the wire's
 *   grammar (a bad destination fails HERE, not at the bus), appends
 *   build the body + signature together (all-or-nothing per append),
 *   and fdk__dbus_message_serialize frames the fixed header, the
 *   field array, and the body exactly as the reference
 *   implementation lays them out (pinned byte-for-byte in
 *   tests/test_dbus.c against the captured libdbus frames).
 *
 *   PARSE: fdk__dbus_message_parse accepts bytes from the socket.
 *   It validates endianness, version, kind, flags, the field array
 *   (codes, variant types, one-instance-per-code, per-kind required
 *   fields), name grammars, signature well-formedness, total size
 *   against the connection's cap, and — before returning the
 *   message — the BODY against the signature (a full unmarshal
 *   whose tree is discarded; read_args re-parses on demand, so a
 *   validated message can never fail read_args except on OOM).
 *
 * Received messages carry the body copy and parsed metadata; the
 * raw framing bytes are not kept.
 */

#define FDK_LOG_TAG "dbus"

#include "dbus/dbus_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- construction --------------------------------------------------------- */

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

static fdk_dbus_message *message_new(uint8_t kind) {
    fdk_dbus_message *m = fdk_alloc(sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    memset(m, 0, sizeof(*m));
    m->kind = kind;
    m->endian = 'l';
    m->signature = dup_str("");
    if (m->signature == NULL) {
        fdk_free(m);
        return NULL;
    }
    m->building = 1;
    return m;
}

fdk_dbus_message *fdk_dbus_message_new_method_call(const char *destination,
                                                   const char *path,
                                                   const char *interface,
                                                   const char *member) {
    if (path == NULL || member == NULL) {
        return NULL;
    }
    if (!fdk__dbus_valid_object_path(path)) {
        FDK_WARN("dbus method call: invalid object path '%s'", path);
        return NULL;
    }
    if (!fdk__dbus_valid_member(member)) {
        FDK_WARN("dbus method call: invalid member '%s'", member);
        return NULL;
    }
    if (interface != NULL && !fdk__dbus_valid_interface(interface)) {
        FDK_WARN("dbus method call: invalid interface '%s'", interface);
        return NULL;
    }
    if (destination != NULL && !fdk__dbus_valid_bus_name(destination)) {
        FDK_WARN("dbus method call: invalid destination '%s'", destination);
        return NULL;
    }
    fdk_dbus_message *m = message_new(FDK_DBUS_MESSAGE_METHOD_CALL);
    if (m == NULL) {
        return NULL;
    }
    m->path = dup_str(path);
    m->member = dup_str(member);
    m->interface = dup_str(interface);
    m->destination = dup_str(destination);
    if (m->path == NULL || m->member == NULL ||
        (interface != NULL && m->interface == NULL) ||
        (destination != NULL && m->destination == NULL)) {
        fdk_dbus_message_free(m);
        return NULL;
    }
    return m;
}

fdk_dbus_message *fdk_dbus_message_new_signal(const char *path,
                                              const char *interface,
                                              const char *member) {
    if (path == NULL || interface == NULL || member == NULL) {
        return NULL;
    }
    if (!fdk__dbus_valid_object_path(path) ||
        !fdk__dbus_valid_interface(interface) ||
        !fdk__dbus_valid_member(member)) {
        FDK_WARN("dbus signal: invalid path/interface/member");
        return NULL;
    }
    fdk_dbus_message *m = message_new(FDK_DBUS_MESSAGE_SIGNAL);
    if (m == NULL) {
        return NULL;
    }
    m->path = dup_str(path);
    m->interface = dup_str(interface);
    m->member = dup_str(member);
    if (m->path == NULL || m->interface == NULL || m->member == NULL) {
        fdk_dbus_message_free(m);
        return NULL;
    }
    return m;
}

fdk_dbus_message *fdk_dbus_message_new_method_return(
    const fdk_dbus_message *call) {
    if (call == NULL || call->kind != FDK_DBUS_MESSAGE_METHOD_CALL) {
        return NULL;
    }
    fdk_dbus_message *m = message_new(FDK_DBUS_MESSAGE_METHOD_RETURN);
    if (m == NULL) {
        return NULL;
    }
    m->reply_serial = call->serial;
    m->destination = dup_str(call->sender); /* back to the caller    */
    if (call->sender != NULL && m->destination == NULL) {
        fdk_dbus_message_free(m);
        return NULL;
    }
    return m;
}

fdk_dbus_message *fdk_dbus_message_new_error(const fdk_dbus_message *call,
                                             const char *error_name,
                                             const char *text) {
    if (call == NULL || call->kind != FDK_DBUS_MESSAGE_METHOD_CALL) {
        return NULL;
    }
    if (error_name == NULL || !fdk__dbus_valid_interface(error_name)) {
        /* error names share the interface grammar (dotted, no
         * leading digits per element) */
        FDK_WARN("dbus error reply: invalid error name '%s'",
                 error_name ? error_name : "(null)");
        return NULL;
    }
    fdk_dbus_message *m = message_new(FDK_DBUS_MESSAGE_ERROR);
    if (m == NULL) {
        return NULL;
    }
    m->reply_serial = call->serial;
    m->destination = dup_str(call->sender);
    m->error_name = dup_str(error_name);
    if (m->error_name == NULL ||
        (call->sender != NULL && m->destination == NULL)) {
        fdk_dbus_message_free(m);
        return NULL;
    }
    if (text != NULL) {
        fdk_result r = fdk_dbus_message_append_string(m, text);
        if (r != FDK_OK) {
            fdk_dbus_message_free(m);
            return NULL;
        }
    }
    return m;
}

void fdk_dbus_message_free(fdk_dbus_message *message) {
    if (message == NULL) {
        return;
    }
    fdk_free(message->path);
    fdk_free(message->interface);
    fdk_free(message->member);
    fdk_free(message->error_name);
    fdk_free(message->destination);
    fdk_free(message->sender);
    fdk_free(message->signature);
    fdk_free(message->body);
    fdk__dbuf_free(&message->body_buf);
    fdk_free(message->sig_buf);
    fdk_free(message);
}

void fdk_dbus_message_set_flags(fdk_dbus_message *message, uint8_t flags) {
    if (message != NULL) {
        message->flags = flags;
    }
}

uint8_t fdk_dbus_message_flags(const fdk_dbus_message *message) {
    return (message != NULL) ? message->flags : 0;
}

/* ---- appending -------------------------------------------------------------- */

/* Appends the value's signature chars to the message's signature
 * buffer and its bytes to the body buffer; both grow together so
 * append failures leave a consistent (though unusable-for-send)
 * message — which fdk_dbus_message_append reports and the caller
 * discards. */
static int sig_append(fdk_dbus_message *m, const char *chars, size_t n) {
    if (m->sig_len + n > FDK_DBUS_SIGNATURE_MAX) {
        return FDK_ERR_LIMIT;
    }
    if (m->sig_len + n + 1 > m->sig_cap) {
        size_t cap = m->sig_cap ? m->sig_cap * 2 : 16;
        while (cap < m->sig_len + n + 1) {
            cap *= 2;
        }
        char *grown = fdk_realloc(m->sig_buf, cap);
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        m->sig_buf = grown;
        m->sig_cap = cap;
    }
    memcpy(m->sig_buf + m->sig_len, chars, n);
    m->sig_len += n;
    m->sig_buf[m->sig_len] = '\0';
    return FDK_OK;
}

static int append_value_sig(fdk_dbus_message *m, const fdk_dbus_value *v);

static int append_value_sig_array(fdk_dbus_message *m,
                                  const fdk_dbus_value *v) {
    int r = sig_append(m, "a", 1);
    if (r != FDK_OK) {
        return r;
    }
    size_t n = strlen(v->v.array.elem_sig);
    return sig_append(m, v->v.array.elem_sig, n);
}

static int append_value_sig_struct(fdk_dbus_message *m,
                                   const fdk_dbus_value *v) {
    char open = (v->type == FDK_DBUS_STRUCT) ? '(' : '{';
    char close = (v->type == FDK_DBUS_STRUCT) ? ')' : '}';
    int r = sig_append(m, &open, 1);
    if (r != FDK_OK) {
        return r;
    }
    for (size_t i = 0; i < v->v.structure.n; i++) {
        r = append_value_sig(m, v->v.structure.items[i]);
        if (r != FDK_OK) {
            return r;
        }
    }
    return sig_append(m, &close, 1);
}

static int append_value_sig(fdk_dbus_message *m, const fdk_dbus_value *v) {
    if (v == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
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
    case FDK_DBUS_VARIANT: {
        char c = (char)v->type;
        return sig_append(m, &c, 1);
    }
    case FDK_DBUS_ARRAY:
        return append_value_sig_array(m, v);
    case FDK_DBUS_STRUCT:
    case FDK_DBUS_DICT_ENTRY:
        return append_value_sig_struct(m, v);
    default:
        return FDK_ERR_INVALID_ARGUMENT;
    }
}

fdk_result fdk_dbus_message_append(fdk_dbus_message *message,
                                   const fdk_dbus_value *value) {
    if (message == NULL || value == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!message->building) {
        return FDK_ERR_INVALID_STATE;
    }
    /* All-or-nothing: snapshot the builder state, try, roll back on
     * any failure. */
    size_t body_len0 = message->body_buf.len;
    int body_ok0 = message->body_buf.ok;
    size_t sig_len0 = message->sig_len;

    int r = fdk__dbus_marshal_value(&message->body_buf, value, 0);
    if (r != FDK_OK) {
        message->body_buf.len = body_len0;
        message->body_buf.ok = body_ok0;
        message->sig_len = sig_len0;
        if (message->sig_buf != NULL) {
            message->sig_buf[sig_len0] = '\0';
        }
        return r;
    }
    r = append_value_sig(message, value);
    if (r != FDK_OK) {
        message->body_buf.len = body_len0;
        message->body_buf.ok = body_ok0;
        message->sig_len = sig_len0;
        if (message->sig_buf != NULL) {
            message->sig_buf[sig_len0] = '\0';
        }
        return r;
    }
    return FDK_OK;
}

fdk_result fdk_dbus_message_append_string(fdk_dbus_message *message,
                                          const char *value) {
    if (value == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_string(value);
    if (v == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = fdk_dbus_message_append(message, v);
    fdk_dbus_value_free(v);
    return r;
}

fdk_result fdk_dbus_message_append_object_path(fdk_dbus_message *message,
                                               const char *value) {
    if (value == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_object_path(value);
    if (v == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = fdk_dbus_message_append(message, v);
    fdk_dbus_value_free(v);
    return r;
}

#define APPEND_BASIC(fn_name, type_code, ctor, param)                         \
    fdk_result fn_name(fdk_dbus_message *message, param value) {             \
        if (message == NULL) {                                               \
            return FDK_ERR_INVALID_ARGUMENT;                                 \
        }                                                                    \
        fdk_dbus_value *v = ctor(value);                                     \
        if (v == NULL) {                                                     \
            return FDK_ERR_OUT_OF_MEMORY;                                    \
        }                                                                    \
        fdk_result r = fdk_dbus_message_append(message, v);                  \
        fdk_dbus_value_free(v);                                              \
        return r;                                                            \
    }

APPEND_BASIC(fdk_dbus_message_append_byte, y, fdk_dbus_value_new_byte,
             uint8_t)
APPEND_BASIC(fdk_dbus_message_append_boolean, b, fdk_dbus_value_new_boolean,
             int)
APPEND_BASIC(fdk_dbus_message_append_int32, i, fdk_dbus_value_new_int32,
             int32_t)
APPEND_BASIC(fdk_dbus_message_append_uint32, u, fdk_dbus_value_new_uint32,
             uint32_t)
APPEND_BASIC(fdk_dbus_message_append_int64, x, fdk_dbus_value_new_int64,
             int64_t)
APPEND_BASIC(fdk_dbus_message_append_double, d, fdk_dbus_value_new_double,
             double)

fdk_result fdk_dbus_message_append_variant_string(fdk_dbus_message *message,
                                                  const char *value) {
    if (message == NULL || value == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_dbus_value *s = fdk_dbus_value_new_string(value);
    if (s == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(s);
    fdk_dbus_value_free(s);
    if (v == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_result r = fdk_dbus_message_append(message, v);
    fdk_dbus_value_free(v);
    return r;
}

fdk_result fdk_dbus_message_append_variant_uint32(fdk_dbus_message *message,
                                                  uint32_t value) {
    if (message == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_dbus_value *u = fdk_dbus_value_new_uint32(value);
    if (u == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(u);
    fdk_dbus_value_free(u);
    if (v == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_result r = fdk_dbus_message_append(message, v);
    fdk_dbus_value_free(v);
    return r;
}

/* ---- metadata accessors ------------------------------------------------------ */

int fdk_dbus_message_kind(const fdk_dbus_message *message) {
    return (message != NULL) ? message->kind : 0;
}

uint32_t fdk_dbus_message_serial(const fdk_dbus_message *message) {
    return (message != NULL) ? message->serial : 0;
}

uint32_t fdk_dbus_message_reply_serial(const fdk_dbus_message *message) {
    return (message != NULL) ? message->reply_serial : 0;
}

const char *fdk_dbus_message_path(const fdk_dbus_message *message) {
    return (message != NULL) ? message->path : NULL;
}

const char *fdk_dbus_message_interface(const fdk_dbus_message *message) {
    return (message != NULL) ? message->interface : NULL;
}

const char *fdk_dbus_message_member(const fdk_dbus_message *message) {
    return (message != NULL) ? message->member : NULL;
}

const char *fdk_dbus_message_error_name(const fdk_dbus_message *message) {
    return (message != NULL) ? message->error_name : NULL;
}

const char *fdk_dbus_message_destination(const fdk_dbus_message *message) {
    return (message != NULL) ? message->destination : NULL;
}

const char *fdk_dbus_message_sender(const fdk_dbus_message *message) {
    return (message != NULL) ? message->sender : NULL;
}

const char *fdk_dbus_message_signature(const fdk_dbus_message *message) {
    return (message != NULL) ? message->signature : NULL;
}

int fdk_dbus_message_is_error(const fdk_dbus_message *message) {
    return (message != NULL && message->kind == FDK_DBUS_MESSAGE_ERROR) ? 1
                                                                        : 0;
}

/* ---- read_args ---------------------------------------------------------------- */

fdk_result fdk_dbus_message_read_args(const fdk_dbus_message *message,
                                      fdk_dbus_value ***out,
                                      size_t *out_count) {
    if (message == NULL || out == NULL || out_count == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    *out_count = 0;
    if (message->body == NULL || message->body_len == 0 ||
        message->signature == NULL || message->signature[0] == '\0') {
        return FDK_OK; /* no arguments */
    }
    fdk__dreader r = {message->body,
                      message->body_len,
                      0,
                      0,
                      message->endian,
                      FDK_OK};
    /* Body-relative alignment is message-absolute alignment (the
     * body starts on an 8-boundary), so the reader works directly. */

    fdk_dbus_value **values = NULL;
    size_t count = 0;
    size_t cap = 0;
    const char *sig = message->signature;
    size_t pos = 0;
    while (sig[pos] != '\0') {
        size_t start = pos;
        if (fdk__dbus_sig_one_complete(sig, &pos, 0, 0, NULL) < 0) {
            fdk_dbus_values_free(values, count);
            FDK_ERROR("read_args: stored signature is malformed — an "
                      "internal defect (framing validation passed it)");
            return FDK_ERR_INVALID_ARGUMENT;
        }
        char one[256];
        size_t olen = pos - start;
        if (olen + 1 > sizeof(one)) {
            fdk_dbus_values_free(values, count);
            return FDK_ERR_LIMIT;
        }
        memcpy(one, sig + start, olen);
        one[olen] = '\0';
        fdk_dbus_value *v = fdk__dbus_unmarshal_value(&r, one);
        if (v == NULL) {
            fdk_dbus_values_free(values, count);
            if (!fdk__rd_ok(&r)) {
                FDK_ERROR("read_args: body does not match signature — an "
                          "internal defect (framing validation passed it)");
                return FDK_ERR_INVALID_ARGUMENT;
            }
            return FDK_ERR_OUT_OF_MEMORY;
        }
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            fdk_dbus_value **grown = fdk_realloc(values, ncap *
                                                         sizeof(*grown));
            if (grown == NULL) {
                fdk_dbus_value_free(v);
                fdk_dbus_values_free(values, count);
                return FDK_ERR_OUT_OF_MEMORY;
            }
            values = grown;
            cap = ncap;
        }
        values[count++] = v;
    }
    if (r.pos != r.len && fdk__rd_ok(&r)) {
        /* trailing garbage after the last argument */
        fdk_dbus_values_free(values, count);
        FDK_ERROR("read_args: %zu trailing bytes after the signature's "
                  "arguments — internal defect",
                  r.len - r.pos);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!fdk__rd_ok(&r)) {
        fdk_dbus_values_free(values, count);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out = values;
    *out_count = count;
    return FDK_OK;
}

/* ---- serialization -------------------------------------------------------------- */

/* One header field: an 8-aligned STRUCT(byte, VARIANT). */
static int put_field_u32(fdk__dbuf *h, uint8_t code, uint32_t v) {
    int r = fdk__dbuf_align(h, 8);
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_put(h, &code, 1);
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_put(h, &(uint8_t){1}, 1); /* variant sig length  */
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_put(h, "u", 1);
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_put(h, &(uint8_t){0}, 1); /* NUL                 */
    if (r != FDK_OK) {
        return r;
    }
    return fdk__dbuf_u32(h, v); /* u32 aligns 4; pos is 8k+8 → fine */
}

static int put_field_string(fdk__dbuf *h, uint8_t code, const char *v,
                            char sig_code) {
    int r = fdk__dbuf_align(h, 8);
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_put(h, &code, 1);
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_put(h, &(uint8_t){1}, 1);
    if (r != FDK_OK) {
        return r;
    }
    char sc[2] = {sig_code, '\0'};
    r = fdk__dbuf_put(h, sc, 1);
    if (r != FDK_OK) {
        return r;
    }
    r = fdk__dbuf_put(h, &(uint8_t){0}, 1);
    if (r != FDK_OK) {
        return r;
    }
    if (sig_code == 'g') {
        return fdk__dbuf_signature(h, v);
    }
    return fdk__dbuf_string(h, v); /* aligns 4 itself */
}

int fdk__dbus_message_serialize(const fdk_dbus_message *m, uint32_t serial,
                                fdk__dbuf *out) {
    if (m == NULL || out == NULL || serial == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk__dbuf h = {NULL, 0, 0, FDK_OK};

    /* The body the app built lives in m->body_buf (built messages);
     * messages reconstructed... always build. The signature comes
     * from sig_buf (or the empty default). */
    const uint8_t *body = m->body_buf.data;
    size_t body_len = m->body_buf.len;
    const char *sig = (m->sig_buf != NULL) ? m->sig_buf : "";

    int r;
    /* Fixed header, 16 bytes. */
    uint8_t fixed[4] = {'l', m->kind, m->flags, 1};
    r = fdk__dbuf_put(&h, fixed, 4);
    if (r != FDK_OK) {
        goto fail;
    }
    r = fdk__dbuf_u32(&h, (uint32_t)body_len);
    if (r != FDK_OK) {
        goto fail;
    }
    r = fdk__dbuf_u32(&h, serial);
    if (r != FDK_OK) {
        goto fail;
    }
    size_t array_len_pos = h.len;
    r = fdk__dbuf_u32(&h, 0); /* placeholder */
    if (r != FDK_OK) {
        goto fail;
    }

    /* Fields, fixed order (PATH, INTERFACE, MEMBER, ERROR_NAME,
     * REPLY_SERIAL, DESTINATION, SIGNATURE). */
    if (m->path != NULL) {
        r = put_field_string(&h, 1, m->path, 'o');
        if (r != FDK_OK) {
            goto fail;
        }
    }
    if (m->interface != NULL) {
        r = put_field_string(&h, 2, m->interface, 's');
        if (r != FDK_OK) {
            goto fail;
        }
    }
    if (m->member != NULL) {
        r = put_field_string(&h, 3, m->member, 's');
        if (r != FDK_OK) {
            goto fail;
        }
    }
    if (m->error_name != NULL) {
        r = put_field_string(&h, 4, m->error_name, 's');
        if (r != FDK_OK) {
            goto fail;
        }
    }
    if (m->reply_serial != 0) {
        r = put_field_u32(&h, 5, m->reply_serial);
        if (r != FDK_OK) {
            goto fail;
        }
    }
    if (m->destination != NULL) {
        r = put_field_string(&h, 6, m->destination, 's');
        if (r != FDK_OK) {
            goto fail;
        }
    }
    if (sig[0] != '\0') {
        r = put_field_string(&h, 8, sig, 'g');
        if (r != FDK_OK) {
            goto fail;
        }
    }

    /* Array length: contents start at 16. */
    size_t array_len = h.len - 16;
    h.data[array_len_pos + 0] = (uint8_t)(array_len & 0xFF);
    h.data[array_len_pos + 1] = (uint8_t)((array_len >> 8) & 0xFF);
    h.data[array_len_pos + 2] = (uint8_t)((array_len >> 16) & 0xFF);
    h.data[array_len_pos + 3] = (uint8_t)((array_len >> 24) & 0xFF);

    /* Pad to 8, then the body. */
    r = fdk__dbuf_align(&h, 8);
    if (r != FDK_OK) {
        goto fail;
    }
    r = fdk__dbuf_put(out, h.data, h.len);
    if (r != FDK_OK) {
        goto fail;
    }
    if (body_len > 0) {
        r = fdk__dbuf_put(out, body, body_len);
        if (r != FDK_OK) {
            goto fail;
        }
    }
    fdk__dbuf_free(&h);
    return FDK_OK;

fail:
    fdk__dbuf_free(&h);
    return r;
}

/* ---- parsing ------------------------------------------------------------------ */

/* Field codes. */
#define FDK_DBUS_FIELD_PATH 1
#define FDK_DBUS_FIELD_INTERFACE 2
#define FDK_DBUS_FIELD_MEMBER 3
#define FDK_DBUS_FIELD_ERROR_NAME 4
#define FDK_DBUS_FIELD_REPLY_SERIAL 5
#define FDK_DBUS_FIELD_DESTINATION 6
#define FDK_DBUS_FIELD_SENDER 7
#define FDK_DBUS_FIELD_SIGNATURE 8
#define FDK_DBUS_FIELD_UNIX_FDS 9

/* Reads the fixed header's u32s honoring endianness. */
static uint32_t peek_u32(const uint8_t *p, uint8_t endian) {
    if (endian == 'l') {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

fdk_dbus_message *fdk__dbus_message_parse(const uint8_t *data, size_t len,
                                          size_t *consumed, int *oom) {
    if (consumed == NULL || oom == NULL) {
        return NULL;
    }
    *consumed = 0;
    *oom = 0;
    if (len < 16) {
        return NULL; /* need more */
    }
    uint8_t endian = data[0];
    if (endian != 'l' && endian != 'B') {
        *consumed = (size_t)-1;
        return NULL;
    }
    uint8_t kind = data[1];
    uint8_t flags = data[2];
    uint8_t version = data[3];
    if (kind < 1 || kind > 4 || version != 1) {
        *consumed = (size_t)-1;
        return NULL;
    }
    uint32_t body_len = peek_u32(data + 4, endian);
    uint32_t serial = peek_u32(data + 8, endian);
    uint32_t array_len = peek_u32(data + 12, endian);
    if (serial == 0) {
        *consumed = (size_t)-1; /* serial 0 is invalid on the wire */
        return NULL;
    }
    /* The total must not blow past sane bounds before we buffer more. */
    if (array_len > (uint32_t)FDK_DBUS_MAX_MESSAGE_DEFAULT ||
        body_len > (uint32_t)FDK_DBUS_MAX_MESSAGE_DEFAULT) {
        *consumed = (size_t)-1;
        return NULL;
    }
    if (16 + (size_t)array_len > len) {
        return NULL; /* need more */
    }
    size_t body_start = 16 + (size_t)array_len;
    /* pad body start to 8 */
    body_start = (body_start + 7) & ~(size_t)7;
    if (body_start + (size_t)body_len > len) {
        return NULL; /* need more */
    }

    /* Walk the field array. */
    fdk__dreader r = {data, 16 + (size_t)array_len, 16, 0, endian, FDK_OK};
    fdk_dbus_message *m = message_new(kind);
    if (m == NULL) {
        *oom = 1;
        return NULL;
    }
    m->building = 0;
    m->flags = flags;
    m->serial = serial;
    m->endian = endian;
    int oom_hit = 0;

    /* seen[c] = field code c already appeared */
    int seen[16] = {0};
    while (r.pos < r.len && fdk__rd_ok(&r)) {
        if (fdk__rd_align(&r, 8) != FDK_OK) {
            break;
        }
        if (r.pos >= r.len) {
            break; /* trailing array padding */
        }
        uint8_t code = fdk__rd_u8(&r);
        char *vsig = fdk__rd_signature(&r);
        if (vsig == NULL || !fdk__rd_ok(&r)) {
            if (vsig == NULL && fdk__rd_ok(&r)) {
                oom_hit = 1; /* clean NULL = allocation failure */
            }
            fdk_free(vsig);
            break;
        }
        /* Known codes (1..9) carry a fixed single-char variant type;
         * anything else is a future/foreign field the spec says to
         * SKIP generically — its variant may hold any complete type. */
        int known = (code >= FDK_DBUS_FIELD_PATH &&
                     code <= FDK_DBUS_FIELD_UNIX_FDS);
        if (known && strlen(vsig) != 1) {
            fdk_free(vsig);
            r.err = FDK_ERR_INVALID_ARGUMENT;
            break;
        }
        char vc = vsig[0];
        size_t vsig_len = strlen(vsig);
        if (!known || seen[code]) {
            if (!known) {
                /* unknown code: skip the value generically */
                fdk_dbus_value *skip = fdk__dbus_unmarshal_value(&r, vsig);
                fdk_free(vsig);
                if (skip == NULL) {
                    if (fdk__rd_ok(&r)) {
                        oom_hit = 1;
                    }
                    break;
                }
                fdk_dbus_value_free(skip);
                continue;
            }
            fdk_free(vsig);
            r.err = FDK_ERR_INVALID_ARGUMENT; /* duplicate known code */
            break;
        }
        seen[code] = 1;
        (void)vsig_len;
        fdk_free(vsig);
        switch (code) {
        case FDK_DBUS_FIELD_PATH:
        case FDK_DBUS_FIELD_INTERFACE:
        case FDK_DBUS_FIELD_MEMBER:
        case FDK_DBUS_FIELD_ERROR_NAME:
        case FDK_DBUS_FIELD_DESTINATION:
        case FDK_DBUS_FIELD_SENDER: {
            if (vc != 's' && !(code == FDK_DBUS_FIELD_PATH && vc == 'o')) {
                r.err = FDK_ERR_INVALID_ARGUMENT;
                break;
            }
            char *s = fdk__rd_string(&r);
            if (s == NULL) {
                break;
            }
            int grammar_ok = 1;
            switch (code) {
            case FDK_DBUS_FIELD_PATH:
                grammar_ok = fdk__dbus_valid_object_path(s);
                break;
            case FDK_DBUS_FIELD_INTERFACE:
                grammar_ok = fdk__dbus_valid_interface(s);
                break;
            case FDK_DBUS_FIELD_MEMBER:
                grammar_ok = fdk__dbus_valid_member(s);
                break;
            case FDK_DBUS_FIELD_ERROR_NAME:
                grammar_ok = fdk__dbus_valid_interface(s);
                break;
            case FDK_DBUS_FIELD_DESTINATION:
            case FDK_DBUS_FIELD_SENDER:
                grammar_ok = fdk__dbus_valid_bus_name(s);
                break;
            }
            if (!grammar_ok) {
                fdk_free(s);
                r.err = FDK_ERR_INVALID_ARGUMENT;
                break;
            }
            switch (code) {
            case FDK_DBUS_FIELD_PATH:
                fdk_free(m->path);
                m->path = s;
                break;
            case FDK_DBUS_FIELD_INTERFACE:
                fdk_free(m->interface);
                m->interface = s;
                break;
            case FDK_DBUS_FIELD_MEMBER:
                fdk_free(m->member);
                m->member = s;
                break;
            case FDK_DBUS_FIELD_ERROR_NAME:
                fdk_free(m->error_name);
                m->error_name = s;
                break;
            case FDK_DBUS_FIELD_DESTINATION:
                fdk_free(m->destination);
                m->destination = s;
                break;
            default:
                fdk_free(m->sender);
                m->sender = s;
                break;
            }
            break;
        }
        case FDK_DBUS_FIELD_REPLY_SERIAL:
        case FDK_DBUS_FIELD_UNIX_FDS: {
            if (vc != 'u') {
                r.err = FDK_ERR_INVALID_ARGUMENT;
                break;
            }
            if (fdk__rd_align(&r, 4) != FDK_OK) {
                break;
            }
            uint32_t v = fdk__rd_u32(&r);
            if (!fdk__rd_ok(&r)) {
                break;
            }
            if (code == FDK_DBUS_FIELD_REPLY_SERIAL) {
                m->reply_serial = v;
            }
            /* UNIX_FDS: counted, ignored (fd passing is not
             * negotiated; any SCM_RIGHTS fds the kernel delivered
             * alongside are closed by the connection's read path). */
            break;
        }
        case FDK_DBUS_FIELD_SIGNATURE: {
            if (vc != 'g') {
                r.err = FDK_ERR_INVALID_ARGUMENT;
                break;
            }
            char *s = fdk__rd_signature(&r);
            if (s == NULL) {
                if (fdk__rd_ok(&r)) {
                    oom_hit = 1;
                }
                break;
            }
            if (!fdk__rd_ok(&r) ||
                !fdk__dbus_valid_signature(s, FDK_DBUS_SIGNATURE_MAX)) {
                fdk_free(s);
                r.err = FDK_ERR_INVALID_ARGUMENT;
                break;
            }
            fdk_free(m->signature);
            m->signature = s;
            break;
        }
        }
    }

    /* Per-kind required fields. */
    if (fdk__rd_ok(&r)) {
        switch (kind) {
        case FDK_DBUS_MESSAGE_METHOD_CALL:
            if (m->path == NULL || m->member == NULL) {
                r.err = FDK_ERR_INVALID_ARGUMENT;
            }
            break;
        case FDK_DBUS_MESSAGE_METHOD_RETURN:
            if (m->reply_serial == 0) {
                r.err = FDK_ERR_INVALID_ARGUMENT;
            }
            break;
        case FDK_DBUS_MESSAGE_ERROR:
            if (m->reply_serial == 0 || m->error_name == NULL) {
                r.err = FDK_ERR_INVALID_ARGUMENT;
            }
            break;
        case FDK_DBUS_MESSAGE_SIGNAL:
            if (m->path == NULL || m->interface == NULL || m->member == NULL) {
                r.err = FDK_ERR_INVALID_ARGUMENT;
            }
            break;
        }
    }
    if (!fdk__rd_ok(&r) || oom_hit) {
        fdk_dbus_message_free(m);
        if (oom_hit && fdk__rd_ok(&r)) {
            *oom = 1;
            return NULL;
        }
        *consumed = (size_t)-1;
        return NULL;
    }

    /* Body copy. */
    if (body_len > 0) {
        m->body = fdk_alloc(body_len);
        if (m->body == NULL) {
            fdk_dbus_message_free(m);
            *oom = 1;
            return NULL;
        }
        memcpy(m->body, data + body_start, body_len);
        m->body_len = body_len;
    }

    /* Full body validation against the signature (validate-and-
     * discard; read_args re-parses on demand — see the file header). */
    if (m->signature != NULL && m->signature[0] != '\0') {
        fdk__dreader br = {m->body, m->body_len, 0, 0, endian, FDK_OK};
        const char *sig = m->signature;
        size_t pos = 0;
        while (sig[pos] != '\0' && fdk__rd_ok(&br)) {
            size_t start = pos;
            if (fdk__dbus_sig_one_complete(sig, &pos, 0, 0, NULL) < 0) {
                br.err = FDK_ERR_INVALID_ARGUMENT;
                break;
            }
            char one[256];
            size_t olen = pos - start;
            if (olen + 1 > sizeof(one)) {
                br.err = FDK_ERR_LIMIT;
                break;
            }
            memcpy(one, sig + start, olen);
            one[olen] = '\0';
            fdk_dbus_value *v = fdk__dbus_unmarshal_value(&br, one);
            if (v == NULL) {
                if (fdk__rd_ok(&br)) {
                    oom_hit = 1;
                }
                break;
            }
            fdk_dbus_value_free(v);
        }
        if (oom_hit && fdk__rd_ok(&br)) {
            fdk_dbus_message_free(m);
            *oom = 1;
            return NULL;
        }
        if (!fdk__rd_ok(&br) || br.pos != br.len) {
            fdk_dbus_message_free(m);
            *consumed = (size_t)-1;
            return NULL;
        }
    } else if (body_len != 0) {
        /* a body with no signature field is a protocol violation */
        fdk_dbus_message_free(m);
        *consumed = (size_t)-1;
        return NULL;
    }

    *consumed = body_start + body_len;
    return m;
}
