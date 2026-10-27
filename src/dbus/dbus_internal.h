/*
 * dbus_internal.h — internal D-Bus machinery (not part of the API)
 *
 * Shared by the src/dbus/ translation units: the growable byte
 * buffer + bounded reader the wire format is marshaled through, the
 * name/signature/path grammar validators, the connection and
 * bookkeeping struct layouts, and the cross-file internal entry
 * points. The public surface is include/fdk/fdk_dbus.h; nothing
 * here ever leaves the library.
 *
 * Security posture (one paragraph, because it shapes everything
 * below): every byte that arrives on a bus socket is UNTRUSTED.
 * The reader therefore never trusts a length field before checking
 * it against the remaining buffer size, never allocates from an
 * unchecked count, caps message size (8 MiB default), array
 * contents, string lengths, signature lengths, and nesting depth,
 * and validates UTF-8 / object-path / name grammar on parse. A
 * violation is a connection-fatal protocol error (one log line,
 * the connection dies, the process lives).
 */

#ifndef FDK_DBUS_INTERNAL_H
#define FDK_DBUS_INTERNAL_H

#include "fdk/fdk_dbus.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FDK_LOG_TAG is NOT defaulted here: log_internal.h requires it
 * pre-defined by the INCLUDING translation unit, and each dbus .c
 * sets "dbus" itself. A header-side #define would clobber a
 * legitimate caller's tag (tests/test_dbus.c tags itself
 * "dbus-test"; the unconditional define used to silently re-tag
 * every log line the test emitted AND warn about the redefinition). */

/* ---- limits (defensive; the spec's own numbers where it has them) ---- */

/* Default maximum accepted message size. The specification allows
 * up to 128 MiB; real desktop traffic is orders of magnitude below
 * this, and an attacker-controlled length field is exactly what the
 * cap exists for. Overridable for tests via FDK_DBUS_MAX_MESSAGE. */
#define FDK_DBUS_MAX_MESSAGE_DEFAULT (8u * 1024u * 1024u)
/* SASL reply lines are tiny; a longer one is a protocol error. */
#define FDK_DBUS_AUTH_LINE_MAX 2048
/* The specification's signature limit. */
#define FDK_DBUS_SIGNATURE_MAX 255
/* Nesting: the spec's ceiling is 64 for containers. We enforce the
 * same number (arrays + structs + dict entries + variants). */
#define FDK_DBUS_MAX_DEPTH 64
/* One read() never asks for more than this; buffered assembly caps
 * at max_message + this slack for the next partial frame. */
#define FDK_DBUS_READ_CHUNK (256u * 1024u)
/* How many messages one io callback will parse and deliver before
 * returning to the pump (fairness: one greedy peer must not pin the
 * UI thread forever; remaining buffered bytes are parsed on the next
 * pump — the fd stays readable, so nothing is delayed). */
#define FDK_DBUS_DISPATCH_BUDGET 64

/* ---- growable byte buffer (marshaling side) --------------------------- */

typedef struct fdk__dbuf {
    uint8_t *data;
    size_t len;
    size_t cap;
    int ok; /* sticky FDK_RESULT; a failed append fails the message */
} fdk__dbuf;

/* Grows to at least need-capacity. Returns 0 / negative fdk_result. */
int fdk__dbuf_reserve(fdk__dbuf *b, size_t extra);
/* Appends raw bytes. */
int fdk__dbuf_put(fdk__dbuf *b, const void *bytes, size_t n);
/* Pads to `alignment` (1/2/4/8) relative to the buffer start — the
 * D-Bus alignment rule, with the caller guaranteeing the buffer's
 * offset-0 corresponds to an aligned absolute position (the message
 * builder lays the header so that always holds; see
 * dbus_marshal.c's header comment). */
int fdk__dbuf_align(fdk__dbuf *b, size_t alignment);
void fdk__dbuf_reset(fdk__dbuf *b); /* keeps capacity            */
void fdk__dbuf_free(fdk__dbuf *b);  /* frees data, zeroes all    */

/* Fixed-width little-endian writers (FDK marshals little-endian,
 * per the specification's endianness rule for senders). */
int fdk__dbuf_u16(fdk__dbuf *b, uint16_t v);
int fdk__dbuf_u32(fdk__dbuf *b, uint32_t v);
int fdk__dbuf_u64(fdk__dbuf *b, uint64_t v);
int fdk__dbuf_double(fdk__dbuf *b, double v);
/* A D-Bus string-ish: length + bytes + NUL, aligned. */
int fdk__dbuf_string(fdk__dbuf *b, const char *s);
/* length BYTE + bytes + NUL (signature alignment 1). */
int fdk__dbuf_signature(fdk__dbuf *b, const char *s);

/* ---- bounded reader (unmarshaling side) ------------------------------- */

typedef struct fdk__dreader {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int depth;        /* container nesting, enforced             */
    uint8_t endian;   /* 'l' or 'B' — affects fixed-width reads  */
    int err;          /* sticky negative fdk_result             */
} fdk__dreader;

/* All readers: on overrun / misalignment / bad value the reader
 * sticks its error and returns a benign value (0) — the caller
 * checks fdk__dreader_ok() once per object. No exceptions to this
 * rule: a NULL return is reserved for allocation failure. */
uint8_t fdk__rd_u8(fdk__dreader *r);
uint16_t fdk__rd_u16(fdk__dreader *r);
uint32_t fdk__rd_u32(fdk__dreader *r);
uint64_t fdk__rd_u64(fdk__dreader *r);
double fdk__rd_double(fdk__dreader *r);
/* Reads a length-checked, NUL-free, UTF-8-validated string into a
 * fresh heap copy (NULL only on allocation failure; on any
 * validation error the reader is stuck and NULL-but-err is reported
 * through fdk__dreader_ok). */
char *fdk__rd_string(fdk__dreader *r);
/* Signature: max 255 bytes, single-type grammar NOT checked here
 * (the caller validates structurally); returns heap copy. */
char *fdk__rd_signature(fdk__dreader *r);
int fdk__rd_align(fdk__dreader *r, size_t alignment);
int fdk__rd_ok(const fdk__dreader *r);

/* ---- grammar validators (dbus_marshal.c) ------------------------------- */

/* True for the ASCII letters/digits/underscore the name grammar
 * shares. */
int fdk__dbus_name_char(unsigned char c);
/* bus name: ":unique" or dot-separated elements of
 * [A-Za-z0-9_-] (unique also allows the same set after ':').
 * Nonempty, each element nonempty. */
int fdk__dbus_valid_bus_name(const char *name);
/* interface / error name: dot-separated [A-Za-z0-9_]+ elements, no
 * element starting with a digit, at least one dot, nonempty. */
int fdk__dbus_valid_interface(const char *name);
/* member: [A-Za-z_][A-Za-z0-9_]* (the spec also allows '-' inside;
 * accepted — e.g. "Guess-Name" style x11 keys never appear here,
 * but KDE members occasionally carry one). */
int fdk__dbus_valid_member(const char *name);
/* object path: "/" or "/element[/element...]" with elements of
 * [A-Za-z0-9_]+. */
int fdk__dbus_valid_object_path(const char *path);
/* strict UTF-8 (rejects overlongs, surrogates, > U+10FFFF). */
int fdk__dbus_valid_utf8(const char *s, size_t n);

/* ---- signature walking -------------------------------------------------- */

/* A validated signature walk: parses ONE complete type starting at
 * sig[*pos] (which must be a type start). On success advances
 * *pos past the complete type and returns the number of SIGNATURE
 * CHARACTERS consumed... unless out_sig is non-NULL, in which case
 * the complete type's signature is written there (a NUL-terminated
 * copy; caller frees) and the count returned is its length.
 * Returns NULL / -1 shapes: on failure returns NULL and leaves
 * *pos untouched; `depth_in` counts container nesting already open.
 * allow_dict: dict entries are legal only directly inside arrays
 * (the caller passes 1 when parsing an array's element). */
int fdk__dbus_sig_one_complete(const char *sig, size_t *pos,
                               int depth_in, int allow_dict,
                               char **out_sig);

/* True when sig is 0..n COMPLETE types covering the whole string
 * (a message signature). max_types caps the count. */
int fdk__dbus_valid_signature(const char *sig, size_t max_types);

/* True when sig is EXACTLY one complete type (a variant's). */
int fdk__dbus_signature_is_single(const char *sig);

/* ---- values (dbus_types.c) ---------------------------------------------- */

/* The public node. Construction helpers shared internally. */
struct fdk_dbus_value {
    uint8_t type; /* fdk_dbus_type                              */
    union {
        uint8_t byte_;
        int boolean_; /* normalized 0/1                           */
        int16_t i16;
        uint16_t u16;
        int32_t i32;
        uint32_t u32;
        int64_t i64;
        uint64_t u64;
        double dbl;
        char *str; /* STRING / OBJECT_PATH / SIGNATURE, owned   */
        struct { /* ARRAY                                     */
            char *elem_sig; /* owned; one complete type         */
            fdk_dbus_value **items;
            size_t n;
            uint8_t *flat; /* owned; the BYTE-ARRAY fast form:
                             non-NULL only for arrays built by
                             fdk_dbus_value_new_byte_array() —
                             one flat allocation instead of n byte
                             nodes (bulk payloads: icons, image
                             data). items stays NULL there; readers
                             use fdk_dbus_value_array_flat_bytes()
                             (array_item() refuses it, loudly).   */
        } array;
        struct { /* VARIANT                                   */
            char *sig; /* owned; exactly one complete type      */
            fdk_dbus_value *child;
        } variant;
        struct { /* STRUCT / DICT_ENTRY                       */
            fdk_dbus_value **items;
            size_t n;
        } structure;
    } v;
};

/* Marshals `value` into `b` (dbus_marshal.c). Depth is the caller's
 * open-container count for enforcement. The value is TRUSTED (it
 * was built through validated constructors or parsed through the
 * bounded reader); failures here are allocation failures only. */
int fdk__dbus_marshal_value(fdk__dbuf *b, const fdk_dbus_value *value,
                            int depth);

/* Unmarshals ONE value of the type described by the single-type
 * signature `sig` from `r` (dbus_marshal.c). Returns NULL with
 * r->err set on violation, NULL with err clean on OOM (the caller
 * distinguishes through fdk__dreader_ok). */
fdk_dbus_value *fdk__dbus_unmarshal_value(fdk__dreader *r, const char *sig);

/* ---- messages (dbus_message.c) ------------------------------------------ */

struct fdk_dbus_message {
    uint8_t kind;    /* FDK_DBUS_MESSAGE_*                       */
    uint8_t flags;
    uint8_t endian;  /* 'l' when we built it; as received        */
    uint32_t serial;      /* 0 until the connection assigns one  */
    uint32_t reply_serial;/* 0 when absent                        */
    char *path;           /* owned, or NULL                       */
    char *interface;      /* owned, or NULL                       */
    char *member;         /* owned, or NULL                       */
    char *error_name;     /* owned, or NULL                       */
    char *destination;    /* owned, or NULL                       */
    char *sender;         /* owned, or NULL (received only)       */
    char *signature;      /* owned; "" when bodyless             */
    uint8_t *body;        /* owned, or NULL                       */
    size_t body_len;
    /* Builder state while the app appends arguments. */
    fdk__dbuf body_buf;
    char *sig_buf;   /* owned, grows with appends                */
    size_t sig_len;
    size_t sig_cap;
    int building;    /* 1 while appendable                       */
};

/* Frames `m` (with its send serial) into `out` (little-endian).
 * Only valid for messages built by the constructors (building done)
 * — received messages are re-serialized never. */
int fdk__dbus_message_serialize(const fdk_dbus_message *m, uint32_t serial,
                                fdk__dbuf *out);

/* Parses ONE complete message from data[0..len). Outcomes:
 *   - a message: *consumed = its total byte length (caller drops
 *     that many bytes from the buffer), *oom = 0.
 *   - NULL, *consumed == 0, *oom == 0: not enough bytes buffered
 *     yet (or framing incomplete) — read more, try again.
 *   - NULL, *consumed < 0: definitive protocol violation; the
 *     absolute value is a suggested byte count to drop, and the
 *     connection must die (the violation is logged by the caller).
 *   - NULL, *consumed == 0, *oom == 1: allocation failure (the
 *     connection should die — memory is gone). */
fdk_dbus_message *fdk__dbus_message_parse(const uint8_t *data, size_t len,
                                          size_t *consumed, int *oom);

/* ---- SASL (dbus_auth.c) -------------------------------------------------- */

/* The client half of the EXTERNAL-first handshake, as a pure state
 * machine over server LINES (unit-testable without a socket; the
 * connection drives it with real bytes).
 *
 * Usage: create with the uid to offer; feed each server line (no
 * CRLF); call fdk__dbus_auth_step for the next client chunk to
 * write (may be NULL — wait for another line); check
 * fdk__dbus_auth_done / _failed. */
typedef struct fdk__dbus_auth {
    int state; /* 0 need-send-first, 1 await-reply, 2 done, 3 failed */
    int tried_anonymous;
    char pending[FDK_DBUS_AUTH_LINE_MAX];
    size_t pending_len;
    int failed;
    int done;
    char guid[32 + 1]; /* the OK line's server GUID, hex          */
} fdk__dbus_auth;

/* Prepares the initial client bytes ("\0AUTH EXTERNAL <hex>\r\n").
 * Returns a heap copy the caller frees, or NULL on OOM. */
char *fdk__dbus_auth_start(uint32_t uid, size_t *out_len);
/* One server line → one client chunk (heap copy, *out_len; NULL
 * chunk = send nothing more). */
char *fdk__dbus_auth_step(fdk__dbus_auth *a, const char *line,
                          size_t *out_len);

/* ---- addresses (dbus_address.c) ------------------------------------------ */

typedef struct fdk__dbus_addr {
    /* Resolved socket endpoint; family is AF_UNIX / AF_INET /
     * AF_INET6, sun_path / ip:port follow. */
    int family;
    char unix_path[108];      /* AF_UNIX (filesystem or abstract)  */
    int abstract_socket;      /* unix_path is an abstract name     */
    uint32_t ip[4];           /* AF_INET: ip[0]; AF_INET6: all 4   */
    uint16_t port;
    char nonce_file[256];     /* nonce-tcp: hex nonce file         */
    int has_nonce;
} fdk__dbus_addr;

/* Resolves `address` (one or more ';'-separated transport specs)
 * into up to *max_out endpoints, tried in order. Returns the count
 * (0: unresolvable — logged once by the caller), or -1 on OOM. */
int fdk__dbus_resolve_addresses(const char *address, fdk__dbus_addr *out,
                                int max_out);

/* The address a bus resolves to (fdk_dbus_bus_address's core).
 * Returns a heap string or NULL. */
char *fdk__dbus_bus_address(fdk_dbus_bus bus);

/* Connects to the first working endpoint. Returns the fd (non-
 * blocking, CLOEXEC) or -1 (errno-style reason logged once by the
 * caller through FDK_INFO). */
int fdk__dbus_connect_first(const fdk__dbus_addr *addrs, int count,
                            struct fdk__dbus_addr *chosen);

/* Reads a nonce-tcp endpoint's nonce file into out (hex → raw).
 * Returns the byte count (0 when the endpoint has no nonce), or
 * -1 on an unreadable/ill-formed file. */
int fdk__dbus_read_nonce(const struct fdk__dbus_addr *a,
                         unsigned char *out, size_t cap);

/* ---- connection (dbus_connection.c) -------------------------------------- */

struct fdk_dbus_pending {
    fdk_dbus_connection *conn;
    uint32_t serial;          /* 0 while queued behind handshake   */
    fdk_dbus_message *queued; /* message waiting for the flush     */
    uint32_t timeout_ms;
    fdk_timer *timer;
    fdk_dbus_reply_fn fn;
    void *user;
    int done;
    int firing; /* its callback is on the stack RIGHT NOW — teardown
                   marks done and defers the free to the fire loop
                   (the extfd/timer deferred-free discipline)        */
};

struct fdk_dbus_subscription {
    fdk_dbus_connection *conn;
    char *rule;
    fdk_dbus_signal_fn fn;
    void *user;
    int dead;   /* freed (or being freed): never fires again        */
    int firing; /* its callback is on the stack — free deferred     */
    /* Properties-changed bridge state (NULL for plain rules). */
    char *pc_destination;
    char *pc_interface;
    fdk_dbus_props_changed_fn pc_fn;
    void *pc_user;
};

struct fdk_dbus_name_watcher {
    fdk_dbus_connection *conn;
    char *name;
    char *owner;         /* current owner (NULL = unowned)          */
    int owner_known;     /* tri-state: unknown until the first fire */
    fdk_dbus_name_event_fn fn;
    void *user;
    int dead;
    int firing;          /* its callback is on the stack            */
    fdk_dbus_pending *initial; /* GetNameOwner in flight (canceled
                                  at watcher free)                  */
};

/* Refcounted AddMatch bookkeeping (identical rule strings shared
 * by several subscriptions install once, drop when the last frees). */
struct fdk_dbus_match_ref {
    char *rule;
    int refs;
};

struct fdk_dbus_connection {
    fdk_context *ctx;
    char *address;

    int fd;
    fdk_ext_fd_source *io;
    fdk_timer *handshake_timer; /* bounds the whole connect phase */

    fdk__dbus_auth auth;
    int auth_started;

    /* CONNECTING → AUTH → HELLO_WAIT → READY; DEAD after. */
    int state;
    uint32_t next_serial;
    char *unique_name;

    fdk__dbuf out;
    uint8_t *in;
    size_t in_len;
    size_t in_cap;

    fdk_dbus_pending **pending;
    size_t pending_count, pending_cap;

    fdk_dbus_message **deferred; /* pre-ready sends, in order      */
    size_t deferred_count, deferred_cap;

    struct fdk_dbus_match_ref *matches;
    size_t match_count, match_cap;

    fdk_dbus_subscription **subs;
    size_t sub_count, sub_cap;

    fdk_dbus_name_watcher **watchers;
    size_t watcher_count, watcher_cap;

    fdk_dbus_method_fn method_fn;
    void *method_user;

    fdk_dbus_owned_name_fn name_fn;
    void *name_user;

    fdk_dbus_ready_fn ready_fn;
    void *ready_user;

    /* Connection-death notification (after pendings fired NULL,
     * before watchers fire vanished). */
    fdk_dbus_death_fn death_fn;
    void *death_user;

    int in_callback; /* >0 while any callback of ours is running  */
    int dead;        /* torn down; the HANDLE stays valid (a zombie)
                        until the app calls close() — every public
                        entry point answers dead connections with a
                        clean error, never a dangling dereference   */
    int close_pending; /* close() reached inside a callback: the
                          outermost pump-facing wrapper frees it the
                          moment in_callback hits zero             */
    int reported_dead; /* the one-line death log happened         */
    size_t max_message;

    /* The endpoint actually connected to (nonce-tcp needs it). */
    struct fdk__dbus_addr endpoint;
};

/* Context registry (context_internal.h holds the fields): register
 * at creation, unregister at free. */
fdk_result fdk__dbus_conn_register(fdk_dbus_connection *c);
void fdk__dbus_conn_unregister(fdk_dbus_connection *c);

/* Shutdown hook called by fdk_shutdown BEFORE the timer/extfd
 * sweeps (our teardown removes our own sources and timers; after
 * those sweeps the records would be freed already). Closes every
 * surviving connection (teardown, not delivery — pending callbacks
 * do NOT fire; the documented shutdown posture). */
void fdk__dbus_shutdown(fdk_context *ctx);

/* Match-rule client-side evaluation (dbus_connection.c): does
 * `rule` match the received signal message? Fail-open on keys the
 * evaluator does not implement. */
int fdk__dbus_rule_matches(const char *rule, fdk_dbus_message *m);

/* The wire test needs the marshal/unmarshal pair; exposed through
 * this header only (tests include src/ internal headers already —
 * precedent: window_internal.h). */

#endif /* FDK_DBUS_INTERNAL_H */
