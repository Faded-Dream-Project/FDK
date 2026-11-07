/*
 * fdk_dbus.h — Faded Dream ToolKit native D-Bus client (1.1.0)
 *
 * A D-Bus client implemented FROM THE PUBLIC SPECIFICATION in raw C:
 * unix-socket (and TCP / nonce-TCP) transport, the SASL EXTERNAL
 * handshake (ANONYMOUS as a fallback where the bus allows it), full
 * wire-format marshaling and unmarshaling of every basic and
 * container type, method calls, replies, errors, signals with match
 * rules, name ownership and watching, properties, and introspection.
 * There is NO libdbus, sd-bus, or GLib anywhere near it — FDK takes
 * no new link-time or run-time dependency for any of this (see
 * docs/DECISIONS.md, "no D-Bus library dependency").
 *
 * Everything is asynchronous and rides FDK's own event loop: the
 * connection's socket is an extension-API fd source, timeouts are
 * fdk timers, and every callback below fires from inside
 * fdk_pump_events()/fdk_run() — never from a private thread, never
 * re-entrantly from inside a call you make. The general FDK rule
 * "UI-thread only" applies to this whole header.
 *
 * Robustness posture: every byte that arrives on the socket is
 * untrusted. Message framing, type marshaling, signatures, string
 * UTF-8-ness, object paths, member/interface/bus names, alignment,
 * nesting depth, and every length field are validated before any
 * allocation of attacker-controlled size happens; the parser is
 * fuzz-tested (tests/test_dbus.c). A malformed stream kills exactly
 * one connection (logged once) — never the process.
 *
 * Degradation: if no bus is reachable, fdk_dbus_connect*() fails
 * fast (one log line, a NULL return — a local socket refusing
 * connect never blocks). Features built on D-Bus disable themselves
 * when the client is absent; nothing busy-loops.
 *
 * The value model: arguments are built and read through an owned
 * tree of fdk_dbus_value nodes (tagged by fdk_dbus_type). Six
 * convenience appenders cover the shapes desktop protocols actually
 * use; everything else goes through the tree.
 *
 * Documentation: docs/EXTENDING.md (the D-Bus chapter),
 * docs/DECISIONS.md. The wire reference is the freedesktop D-Bus
 * Specification (IMPLEMENTED from, not copied from — the license
 * posture is in docs/THIRD-PARTY.md).
 */

#ifndef FDK_DBUS_H
#define FDK_DBUS_H

#include "fdk_core.h"
#include "fdk_error.h"
#include "fdk_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Values — the argument model
 * ========================================================================*/

/* D-Bus type codes (the wire's own characters, kept as the enum's
 * values so the mapping is visible and greppable). DICT_ENTRY and
 * STRUCT arrive as distinct node kinds; array-of-dict-entry ("a{..}")
 * is an ARRAY whose elements are DICT_ENTRY nodes. */
typedef enum fdk_dbus_type {
    FDK_DBUS_TYPE_INVALID    = 0,
    FDK_DBUS_BYTE            = 'y',
    FDK_DBUS_BOOLEAN         = 'b',
    FDK_DBUS_INT16           = 'n',
    FDK_DBUS_UINT16          = 'q',
    FDK_DBUS_INT32           = 'i',
    FDK_DBUS_UINT32          = 'u',
    FDK_DBUS_INT64           = 'x',
    FDK_DBUS_UINT64          = 't',
    FDK_DBUS_DOUBLE          = 'd',
    FDK_DBUS_STRING          = 's',
    FDK_DBUS_OBJECT_PATH     = 'o',
    FDK_DBUS_SIGNATURE       = 'g',
    FDK_DBUS_VARIANT         = 'v',
    FDK_DBUS_ARRAY           = 'a',
    FDK_DBUS_STRUCT          = 'r', /* wire '(' — not a stand-alone code */
    FDK_DBUS_DICT_ENTRY      = 'e', /* wire '{' — only inside arrays    */
} fdk_dbus_type;

/* An owned, self-describing argument value. Opaque: build with the
 * constructors below, read with the accessors below, release with
 * fdk_dbus_value_free. Strings are NUL-terminated copies owned by
 * the node. A VARIANT holds exactly one child plus its signature;
 * an ARRAY holds its element signature plus 0..n children; a STRUCT
 * or DICT_ENTRY holds 0..n children (2 for a valid dict entry). */
typedef struct fdk_dbus_value fdk_dbus_value;

/* Constructors (all return NULL on allocation failure or invalid
 * arguments — an invalid object path / signature / array element
 * signature is rejected, not silently coerced). The _owned forms
 * TAKE OWNERSHIP of the passed items array (not the pointed-to
 * values): freeing the node frees every child. */
fdk_dbus_value *fdk_dbus_value_new_byte(uint8_t value);
fdk_dbus_value *fdk_dbus_value_new_boolean(int value);
fdk_dbus_value *fdk_dbus_value_new_int16(int16_t value);
fdk_dbus_value *fdk_dbus_value_new_uint16(uint16_t value);
fdk_dbus_value *fdk_dbus_value_new_int32(int32_t value);
fdk_dbus_value *fdk_dbus_value_new_uint32(uint32_t value);
fdk_dbus_value *fdk_dbus_value_new_int64(int64_t value);
fdk_dbus_value *fdk_dbus_value_new_uint64(uint64_t value);
fdk_dbus_value *fdk_dbus_value_new_double(double value);
fdk_dbus_value *fdk_dbus_value_new_string(const char *value);
fdk_dbus_value *fdk_dbus_value_new_object_path(const char *value);
fdk_dbus_value *fdk_dbus_value_new_signature(const char *value);
/* A VARIANT wrapping exactly one child (the node keeps its own copy
 * of the child's signature, as the wire requires). */
fdk_dbus_value *fdk_dbus_value_new_variant(const fdk_dbus_value *child);
/* An ARRAY: elem_sig must be ONE complete type (validated); every
 * item must marshal under it (checked: each item's own signature
 * must be equivalent to elem_sig). items may be NULL when n == 0.
 * The VALUES become owned by the array (freeing it frees them);
 * the pointer array itself is copied, so caller-owned memory (a
 * stack array, for instance) stays the caller's. */
fdk_dbus_value *fdk_dbus_value_new_array(const char *elem_sig,
                                         fdk_dbus_value **items,
                                         size_t count);
/* The BYTE-ARRAY fast form: one flat allocation instead of `count`
 * byte nodes. Signature is "y" by construction; count is capped at
 * the message-size limit (8 MiB default) — bulk payloads like icons
 * and image data are exactly what this is for. Read the result with
 * fdk_dbus_value_array_flat_bytes(); fdk_dbus_value_array_item()
 * refuses it (a DEBUG line, NULL — the tree API and the flat API
 * are deliberately distinct). Marshals as one block. */
fdk_dbus_value *fdk_dbus_value_new_byte_array(const uint8_t *data,
                                              size_t count);
/* A STRUCT: any children, in order — ownership like new_array. */
fdk_dbus_value *fdk_dbus_value_new_struct(fdk_dbus_value **items,
                                          size_t count);
/* A DICT_ENTRY: exactly a key and a value (key must be a basic
 * type, per the wire). Takes OWNERSHIP of both — freeing the entry
 * frees them; do not free them separately. */
fdk_dbus_value *fdk_dbus_value_new_dict_entry(fdk_dbus_value *key,
                                              fdk_dbus_value *value);

/* Deep copy. */
fdk_dbus_value *fdk_dbus_value_clone(const fdk_dbus_value *value);

/* Releases the node and everything it owns. NULL is a no-op. */
void fdk_dbus_value_free(fdk_dbus_value *value);

/* Frees an array of owned nodes (NULL items or 0 count is a no-op;
 * the ITEMS array itself is NOT freed — match how you allocated it;
 * fdk_dbus_message_read_args()'s output is the exception: it is one
 * flat allocation, use fdk_dbus_values_free_array on it). */
void fdk_dbus_values_free(fdk_dbus_value **values, size_t count);
/* Frees read_args' output: every node AND the pointer array (which
 * the library allocated as one block). NULL/0 is a no-op. */
void fdk_dbus_values_free_array(fdk_dbus_value **values, size_t count);

/* ---- accessors -----------------------------------------------------------
 * Every accessor is type-checked: the wrong type yields a benign
 * default (0 / NULL) and one DEBUG log line — never a crash. */

fdk_dbus_type fdk_dbus_value_type(const fdk_dbus_value *value);
uint8_t       fdk_dbus_value_byte(const fdk_dbus_value *value);
int           fdk_dbus_value_boolean(const fdk_dbus_value *value);
int16_t       fdk_dbus_value_int16(const fdk_dbus_value *value);
uint16_t      fdk_dbus_value_uint16(const fdk_dbus_value *value);
int32_t       fdk_dbus_value_int32(const fdk_dbus_value *value);
uint32_t      fdk_dbus_value_uint32(const fdk_dbus_value *value);
int64_t       fdk_dbus_value_int64(const fdk_dbus_value *value);
uint64_t      fdk_dbus_value_uint64(const fdk_dbus_value *value);
double        fdk_dbus_value_double(const fdk_dbus_value *value);

/* STRING / OBJECT_PATH / SIGNATURE (borrowed, lives with the node;
 * NULL for a wrong type or a NULL node). */
const char *fdk_dbus_value_string(const fdk_dbus_value *value);

/* ARRAY: the element signature (borrowed) and items. */
const char *fdk_dbus_value_array_signature(const fdk_dbus_value *value);
size_t      fdk_dbus_value_array_count(const fdk_dbus_value *value);
const fdk_dbus_value *fdk_dbus_value_array_item(const fdk_dbus_value *value,
                                                size_t index);
/* ARRAY, the flat form: the byte block behind a byte array built
 * with fdk_dbus_value_new_byte_array() (borrowed, lives with the
 * node); NULL with *count 0 for every other array — the reader a
 * bulk payload deserves, instead of n array_item() hops. */
const uint8_t *fdk_dbus_value_array_flat_bytes(const fdk_dbus_value *value,
                                               size_t *count);

/* VARIANT: its signature (borrowed) and its single child. */
const char *fdk_dbus_value_variant_signature(const fdk_dbus_value *value);
const fdk_dbus_value *fdk_dbus_value_variant_value(const fdk_dbus_value *value);

/* STRUCT / DICT_ENTRY: field count and items. */
size_t      fdk_dbus_value_struct_count(const fdk_dbus_value *value);
const fdk_dbus_value *fdk_dbus_value_struct_item(const fdk_dbus_value *value,
                                                 size_t index);
const fdk_dbus_value *fdk_dbus_value_dict_key(const fdk_dbus_value *entry);
const fdk_dbus_value *fdk_dbus_value_dict_value(const fdk_dbus_value *entry);

/* Looks up `key` in an ARRAY OF DICT ENTRIES whose keys are
 * string-typed (the "a{sv}" shape every desktop protocol leans on).
 * Returns the entry's VALUE node, or NULL when absent / wrong shape.
 * This is the one-liner the portal Settings, tray hints, and
 * notification hints all reduce to. */
const fdk_dbus_value *fdk_dbus_value_dict_lookup(const fdk_dbus_value *array,
                                                 const char *key);

/* Renders the value's signature into a freshly allocated
 * NUL-terminated string (caller frees with free()). The signature of
 * a whole message argument list is the concatenation over its values.
 * NULL on allocation failure. */
char *fdk_dbus_value_signature(const fdk_dbus_value *value);

/* ==========================================================================
 * Messages
 * ========================================================================*/

/* Message kinds (the wire's own numbers). */
enum {
    FDK_DBUS_MESSAGE_METHOD_CALL   = 1,
    FDK_DBUS_MESSAGE_METHOD_RETURN = 2,
    FDK_DBUS_MESSAGE_ERROR         = 3,
    FDK_DBUS_MESSAGE_SIGNAL        = 4,
};

/* Header flags (the wire's bits). */
#define FDK_DBUS_FLAG_NO_REPLY_EXPECTED            0x01u
#define FDK_DBUS_FLAG_NO_AUTO_START                0x02u
#define FDK_DBUS_FLAG_ALLOW_INTERACTIVE_AUTHORIZATION 0x04u

typedef struct fdk_dbus_message fdk_dbus_message;

/* Message constructors. Everything is copied; the message owns its
 * fields. Names are VALIDATED (bus/member/interface shape, object
 * path shape — see the specification's grammar): an invalid name
 * fails with NULL, because the bus would only reject the message
 * later and the bug is easier to fix at the call site.
 *
 *   method_call: destination may be NULL (bus-daemon-targeted calls
 *                set it; broadcast is not a thing for calls).
 *                interface/member required, path required.
 *   signal:      path/interface/member required, destination usually
 *                NULL (the bus broadcasts).
 *   method_return / error: built FROM the call being answered — the
 *                reply serial and (error) name come from it. */
fdk_dbus_message *fdk_dbus_message_new_method_call(const char *destination,
                                                   const char *path,
                                                   const char *interface,
                                                   const char *member);
fdk_dbus_message *fdk_dbus_message_new_signal(const char *path,
                                              const char *interface,
                                              const char *member);
fdk_dbus_message *fdk_dbus_message_new_method_return(
    const fdk_dbus_message *call);
/* error_name must be a valid D-Bus error name (dots and the error
 * grammar); text is human-readable and may be NULL. */
fdk_dbus_message *fdk_dbus_message_new_error(const fdk_dbus_message *call,
                                             const char *error_name,
                                             const char *text);

void fdk_dbus_message_free(fdk_dbus_message *message);

/* Sets the header flags (NO_REPLY_EXPECTED for fire-and-forget
 * calls, NO_AUTO_START to never spawn services, ...). */
void fdk_dbus_message_set_flags(fdk_dbus_message *message, uint8_t flags);
uint8_t fdk_dbus_message_flags(const fdk_dbus_message *message);

/* Appending arguments. The generic form appends one complete value
 * (deep-copied); the convenience forms build the common shapes.
 * Appending after the message was handed to a connection is refused
 * (FDK_ERR_INVALID_STATE). Values are validated as they are
 * appended — an append that fails leaves the message exactly as it
 * was (all-or-nothing per call). */
fdk_result fdk_dbus_message_append(fdk_dbus_message *message,
                                   const fdk_dbus_value *value);
fdk_result fdk_dbus_message_append_string(fdk_dbus_message *message,
                                          const char *value);
fdk_result fdk_dbus_message_append_object_path(fdk_dbus_message *message,
                                               const char *value);
fdk_result fdk_dbus_message_append_byte(fdk_dbus_message *message,
                                        uint8_t value);
fdk_result fdk_dbus_message_append_boolean(fdk_dbus_message *message,
                                           int value);
fdk_result fdk_dbus_message_append_int32(fdk_dbus_message *message,
                                         int32_t value);
fdk_result fdk_dbus_message_append_uint32(fdk_dbus_message *message,
                                          uint32_t value);
fdk_result fdk_dbus_message_append_int64(fdk_dbus_message *message,
                                         int64_t value);
fdk_result fdk_dbus_message_append_double(fdk_dbus_message *message,
                                          double value);
/* The two variant shapes desktop protocols exchange constantly. */
fdk_result fdk_dbus_message_append_variant_string(fdk_dbus_message *message,
                                                  const char *value);
fdk_result fdk_dbus_message_append_variant_uint32(fdk_dbus_message *message,
                                                  uint32_t value);

/* ---- reading --------------------------------------------------------------

 * All message metadata is BORROWED (valid until the message is
 * freed; a message handed to a callback is valid for the callback's
 * duration only). reply_serial is 0 when absent. serial is the
 * connection-assigned send serial (0 for never-sent / received
 * messages use the sender's serial). */
int fdk_dbus_message_kind(const fdk_dbus_message *message);
uint32_t fdk_dbus_message_serial(const fdk_dbus_message *message);
uint32_t fdk_dbus_message_reply_serial(const fdk_dbus_message *message);
const char *fdk_dbus_message_path(const fdk_dbus_message *message);
const char *fdk_dbus_message_interface(const fdk_dbus_message *message);
const char *fdk_dbus_message_member(const fdk_dbus_message *message);
const char *fdk_dbus_message_error_name(const fdk_dbus_message *message);
const char *fdk_dbus_message_destination(const fdk_dbus_message *message);
const char *fdk_dbus_message_sender(const fdk_dbus_message *message);
const char *fdk_dbus_message_signature(const fdk_dbus_message *message);

/* True for an ERROR-kind message (check error_name). */
int fdk_dbus_message_is_error(const fdk_dbus_message *message);

/* Parses the message's arguments into a FLAT, OWNED array of values
 * (the body parsed against the signature; empty body → 0 values and
 * *out == NULL). The array AND the nodes are released together with
 * fdk_dbus_values_free_array. A body that does not parse under its
 * signature fails with FDK_ERR_INVALID_ARGUMENT (this is the
 * untrusted-input path; received messages were already validated at
 * framing time, so failure here means an internal defect, logged). */
fdk_result fdk_dbus_message_read_args(const fdk_dbus_message *message,
                                      fdk_dbus_value ***out,
                                      size_t *out_count);

/* ==========================================================================
 * Connections
 * ========================================================================*/

typedef struct fdk_dbus_connection fdk_dbus_connection;

/* Which well-known bus to reach for. */
typedef enum fdk_dbus_bus {
    FDK_DBUS_SESSION_BUS = 1,
    FDK_DBUS_SYSTEM_BUS  = 2,
} fdk_dbus_bus;

/* Fired exactly once per connection: when the SASL handshake and
 * the mandatory Hello have completed (ok nonzero — the connection
 * is usable, its unique name is set), or when connecting failed at
 * any point (ok zero; `error` names the stage, borrowed). A bus
 * that is absent makes fdk_dbus_connect*() itself return NULL —
 * this callback reports failures that need the socket to observe. */
typedef void (*fdk_dbus_ready_fn)(fdk_dbus_connection *connection,
                                  int ok, const char *error, void *user);

/* Resolution order (session): the FDK_DBUS_ADDRESS override env
 * variable (tests and exotic setups), DBUS_SESSION_BUS_ADDRESS,
 * $XDG_RUNTIME_DIR/bus. System: FDK_DBUS_ADDRESS,
 * DBUS_SYSTEM_BUS_ADDRESS, /var/run/dbus/system_bus_socket (then
 * /run/dbus/system_bus_socket). Multiple ';'-separated addresses
 * are tried in order; each may be unix:path=/..., unix:abstract=...,
 * unix:dir=/..., tcp:host=...,port=..., family=..., or nonce-tcp:
 * (percent-escaped values supported, per the specification).
 *
 * Returns the connection handle IMMEDIATELY (the handshake runs on
 * the event loop) or NULL when no address resolves to a reachable
 * socket right away (one INFO log line — never a retry storm: a
 * caller that wants a bus later calls again itself). Messages
 * handed to a not-yet-ready connection are queued and flushed, in
 * order, once it is. NULL for a NULL context or a bad address. */
fdk_dbus_connection *fdk_dbus_connect(fdk_context *ctx, fdk_dbus_bus bus,
                                      fdk_dbus_ready_fn on_ready,
                                      void *user);

/* Connects to an explicit address string (same grammar as above;
 * this is how the AT-SPI bus — whose address is published as a
 * property on the session bus — is reached). */
fdk_dbus_connection *fdk_dbus_connect_address(fdk_context *ctx,
                                              const char *address,
                                              fdk_dbus_ready_fn on_ready,
                                              void *user);

/* The address a bus would resolve to right now (NULL when none
 * does): session or system, with the same precedence as
 * fdk_dbus_connect(). Diagnostic and testing surface — the string
 * is freshly allocated; free it with free(). */
char *fdk_dbus_bus_address(fdk_dbus_bus bus);

/* Fired once when the connection DIES after it was ready (bus
 * crash, protocol violation, socket error) — after pending calls
 * received their NULL replies and before watchers report names
 * vanished. An app that wants a resilient bus presence re-connects
 * here. Not fired for a connection that failed before ready (the
 * ready callback's ok=0 covers that), nor on an app-called close(). */
typedef void (*fdk_dbus_death_fn)(fdk_dbus_connection *connection,
                                  void *user);

void fdk_dbus_connection_set_death_handler(fdk_dbus_connection *connection,
                                           fdk_dbus_death_fn fn,
                                           void *user);

/* Teardown: closes the socket, cancels every pending call (their
 * callbacks see a NULL reply), drops subscriptions and watchers,
 * and frees the handle. Safe from inside any callback of the
 * connection (the free is deferred until the callback returns —
 * the same discipline as fdk_ext_loop_remove_fd inside an fd
 * source). NULL is a no-op. Idempotent. */
void fdk_dbus_connection_close(fdk_dbus_connection *connection);

fdk_context *fdk_dbus_connection_context(fdk_dbus_connection *connection);
/* Nonzero once the handshake + Hello completed and the connection
 * has not died since. */
int fdk_dbus_connection_ready(const fdk_dbus_connection *connection);
/* The bus-assigned unique name (":1.42"), or NULL before ready. */
const char *fdk_dbus_connection_unique_name(
    const fdk_dbus_connection *connection);
/* The address actually connected to (borrowed; diagnostics). */
const char *fdk_dbus_connection_address(
    const fdk_dbus_connection *connection);

/* ==========================================================================
 * Method calls
 * ========================================================================*/

typedef struct fdk_dbus_pending fdk_dbus_pending;

/* Receives a reply. `reply` is:
 *   - a METHOD_RETURN (arguments via fdk_dbus_message_read_args), or
 *   - an ERROR message (fdk_dbus_message_is_error() — error_name),
 *   - or NULL: the call timed out, the connection died, or the call
 *     was canceled after the reply arrived late.
 * The reply is borrowed (valid for the callback's duration).
 * Replies are delivered AT MOST ONCE. */
typedef void (*fdk_dbus_reply_fn)(fdk_dbus_connection *connection,
                                  fdk_dbus_message *reply, void *user);

#define FDK_DBUS_TIMEOUT_DEFAULT 0u        /* 25 s                     */
#define FDK_DBUS_TIMEOUT_NONE    0xFFFFFFFFu /* wait for the peer      */

/* Sends `call` and arranges the reply. The MESSAGE is consumed on
 * success (owned by the connection; freed after framing) and LEFT
 * WITH THE CALLER on failure (NULL return: not connected / bad
 * arguments / out of memory). timeout_ms: FDK_DBUS_TIMEOUT_DEFAULT,
 * milliseconds, or FDK_DBUS_TIMEOUT_NONE. on_reply may be NULL
 * (fire-and-forget with the reply still consumed and dropped). */
fdk_dbus_pending *fdk_dbus_connection_call(fdk_dbus_connection *connection,
                                          fdk_dbus_message *call,
                                          uint32_t timeout_ms,
                                          fdk_dbus_reply_fn on_reply,
                                          void *user);

/* Sends a message with no bookkeeping (replies to calls you were
 * served, signals, error replies). Consumed on FDK_OK, kept by the
 * caller otherwise. */
fdk_result fdk_dbus_connection_send(fdk_dbus_connection *connection,
                                    fdk_dbus_message *message);

/* Drops a pending call: the reply callback will NOT fire, the
 * timeout timer is removed, and a late reply is discarded. NULL is
 * a no-op; canceling an already-completed call is a no-op. */
void fdk_dbus_pending_cancel(fdk_dbus_pending *pending);

/* The serial the call went out with (0 while still queued behind
 * the handshake) — the correlation key, for diagnostics. */
uint32_t fdk_dbus_pending_serial(const fdk_dbus_pending *pending);

/* ==========================================================================
 * Signals (match rules)
 * ========================================================================*/

typedef struct fdk_dbus_subscription fdk_dbus_subscription;

typedef void (*fdk_dbus_signal_fn)(fdk_dbus_connection *connection,
                                   fdk_dbus_message *signal,
                                   void *user);

/* Builds a match rule string (freshly allocated; free() it): NULL
 * parts are omitted, `type` is always emitted ("signal" when NULL).
 * Example: fdk_dbus_match_rule("signal", "org.freedesktop.DBus",
 *             NULL, "NameOwnerChanged", NULL,
 *             "org.kde.StatusNotifierWatcher")
 *   → "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.kde.StatusNotifierWatcher'"
 */
char *fdk_dbus_match_rule(const char *type, const char *sender,
                          const char *interface, const char *member,
                          const char *path, const char *arg0);

/* Subscribes to a match rule (the AddMatch call is issued for you;
 * identical rules are refcounted). The callback receives signals
 * that match the rule — matched CLIENT-SIDE over type, sender,
 * interface, member, path, destination, arg0, path_namespace, and
 * argN string equality; keys the matcher does not understand make
 * a rule fail OPEN (deliver) rather than silently drop — write
 * precise rules and this never matters. The sender a well-known
 * name carries is the UNIQUE name the bus rewrites into the
 * message header — subscriptions match on the header value; see
 * fdk_dbus_connection_watch_name for name-stable signal matching.
 * Only usable once the connection is ready (NULL return otherwise).
 * Freeing the subscription issues RemoveMatch (refcounted). */
fdk_dbus_subscription *fdk_dbus_connection_subscribe(
    fdk_dbus_connection *connection, const char *match_rule,
    fdk_dbus_signal_fn on_signal, void *user);

void fdk_dbus_subscription_free(fdk_dbus_subscription *subscription);

/* ==========================================================================
 * Name ownership and watching
 * ========================================================================*/

#define FDK_DBUS_NAME_ALLOW_REPLACEMENT 0x1u
#define FDK_DBUS_NAME_REPLACE_EXISTING  0x2u
#define FDK_DBUS_NAME_DO_NOT_QUEUE      0x4u

/* Reply codes from RequestName (the specification's numbers). */
enum {
    FDK_DBUS_REQUEST_NAME_PRIMARY_OWNER = 1,
    FDK_DBUS_REQUEST_NAME_IN_QUEUE      = 2,
    FDK_DBUS_REQUEST_NAME_EXISTS        = 3,
    FDK_DBUS_REQUEST_NAME_ALREADY_OWNER = 4,
};

/* Requests a well-known name. The pending's reply carries one
 * UINT32 argument — the code above (read it with read_args, or use
 * the FDK_DBUS_REQUEST_NAME_* constants against
 * fdk_dbus_value_uint32 on the first argument). NameAcquired /
 * NameLost signals (the bus sends them unicast, no match rule
 * needed) are routed to the callback installed with
 * fdk_dbus_connection_set_name_events(). */
fdk_dbus_pending *fdk_dbus_connection_request_name(
    fdk_dbus_connection *connection, const char *name, uint32_t flags,
    fdk_dbus_reply_fn on_reply, void *user);

typedef void (*fdk_dbus_owned_name_fn)(fdk_dbus_connection *connection,
                                       const char *name, int owned,
                                       void *user);

/* Installs (or, with fn == NULL, removes) the receiver for
 * NameAcquired/NameLost on names this connection owns. Fires once
 * per acquisition/loss; `owned` is 1 or 0. */
void fdk_dbus_connection_set_name_events(fdk_dbus_connection *connection,
                                         fdk_dbus_owned_name_fn fn,
                                         void *user);

typedef struct fdk_dbus_name_watcher fdk_dbus_name_watcher;

/* Fired when a well-known name's owner appears or disappears:
 * `owner` is the new unique name (":1.7") or NULL when it vanished.
 * The watcher fires once immediately with the current state, then
 * on every change; transitions only (no spurious repeats), and
 * races between the initial GetNameOwner and a NameOwnerChanged
 * are reconciled by comparing owners — you see each real
 * transition exactly once. The bus daemon's own name is always
 * reported as owned by "org.freedesktop.DBus". */
typedef void (*fdk_dbus_name_event_fn)(fdk_dbus_connection *connection,
                                       const char *name, const char *owner,
                                       void *user);

fdk_dbus_name_watcher *fdk_dbus_connection_watch_name(
    fdk_dbus_connection *connection, const char *well_known_name,
    fdk_dbus_name_event_fn on_event, void *user);

void fdk_dbus_name_watcher_free(fdk_dbus_name_watcher *watcher);

/* ==========================================================================
 * Properties (org.freedesktop.DBus.Properties)
 * ========================================================================*/

/* Get: the reply's first argument is the property value as a
 * VARIANT; errors arrive as ordinary ERROR replies
 * (org.freedesktop.DBus.Error.InvalidArgs for unknown properties). */
fdk_dbus_pending *fdk_dbus_connection_get_property(
    fdk_dbus_connection *connection, const char *destination,
    const char *path, const char *interface, const char *property,
    fdk_dbus_reply_fn on_reply, void *user);

/* Set: value is marshaled as a VARIANT automatically (give the
 * INNER value, not a variant node). */
fdk_dbus_pending *fdk_dbus_connection_set_property(
    fdk_dbus_connection *connection, const char *destination,
    const char *path, const char *interface, const char *property,
    const fdk_dbus_value *value, fdk_dbus_reply_fn on_reply, void *user);

typedef void (*fdk_dbus_props_changed_fn)(
    fdk_dbus_connection *connection, const char *interface,
    const fdk_dbus_value *changed_properties, /* ARRAY of DICT_ENTRY */
    size_t invalidated_count, const char *const *invalidated, void *user);

/* Subscribes to PropertiesChanged for one destination+interface
 * (match rule + typed delivery; the changed dict is the raw ARRAY
 * value — fdk_dbus_value_dict_lookup is the reader). Frees like a
 * subscription.
 *
 * Matching note: a WELL-KNOWN destination is not used as the rule's
 * sender (the bus rewrites it into the owner's unique name in the
 * signal's header — such a rule would never fire); the rule scopes
 * on interface+member+arg0 (the interface that changed), which
 * delivers changes from any peer for that interface. A UNIQUE-name
 * destination (":1.x") additionally scopes the sender. */
fdk_dbus_subscription *fdk_dbus_connection_subscribe_properties_changed(
    fdk_dbus_connection *connection, const char *destination,
    const char *interface, fdk_dbus_props_changed_fn on_changed,
    void *user);

/* ==========================================================================
 * Introspection (org.freedesktop.DBus.Introspectable)
 * ========================================================================*/

typedef struct fdk_dbus_introspection fdk_dbus_introspection;

/* A parsed node: its name, the interfaces it implements, and child
 * node names. One level, exactly like the protocol (children need
 * their own Introspect call). */
fdk_dbus_pending *fdk_dbus_connection_introspect(
    fdk_dbus_connection *connection, const char *destination,
    const char *path,
    void (*on_reply)(fdk_dbus_connection *connection,
                     fdk_dbus_introspection *node, const char *raw_xml,
                     void *user),
    void *user);

/* Parsed-model accessors (all borrowed from the node). */
const char *fdk_dbus_introspection_node_name(
    const fdk_dbus_introspection *node);
size_t fdk_dbus_introspection_interface_count(
    const fdk_dbus_introspection *node);
const char *fdk_dbus_introspection_interface_name(
    const fdk_dbus_introspection *node, size_t index);
/* -1 when the node does not implement the interface; else the
 * property index for the accessors below. */
int fdk_dbus_introspection_find_interface(
    const fdk_dbus_introspection *node, const char *interface_name);
size_t fdk_dbus_introspection_method_count(
    const fdk_dbus_introspection *node, int interface_index);
const char *fdk_dbus_introspection_method_name(
    const fdk_dbus_introspection *node, int interface_index, size_t index);
size_t fdk_dbus_introspection_signal_count(
    const fdk_dbus_introspection *node, int interface_index);
const char *fdk_dbus_introspection_signal_name(
    const fdk_dbus_introspection *node, int interface_index, size_t index);
size_t fdk_dbus_introspection_property_count(
    const fdk_dbus_introspection *node, int interface_index);
const char *fdk_dbus_introspection_property_name(
    const fdk_dbus_introspection *node, int interface_index, size_t index);
/* "read" / "write" / "readwrite" (borrowed; "read" for absent). */
const char *fdk_dbus_introspection_property_access(
    const fdk_dbus_introspection *node, int interface_index, size_t index);
size_t fdk_dbus_introspection_child_count(
    const fdk_dbus_introspection *node);
const char *fdk_dbus_introspection_child_name(
    const fdk_dbus_introspection *node, size_t index);

/* Parses introspection XML standalone (the same parser
 * fdk_dbus_connection_introspect feeds; a lenient hand-rolled tag
 * scanner — unknown tags and attributes are skipped, not rejected,
 * matching how every real service's XML deviates). NULL on
 * allocation failure; free with fdk_dbus_introspection_free. */
fdk_dbus_introspection *fdk_dbus_introspection_parse(const char *xml);
void fdk_dbus_introspection_free(fdk_dbus_introspection *node);

/* ==========================================================================
 * Serving method calls
 * ========================================================================*/

/* Incoming METHOD_CALL messages are delivered here (borrowed
 * message, valid for the callback only). Reply INSIDE the callback:
 * build with fdk_dbus_message_new_method_return /
 * _new_error against the call and send it with
 * fdk_dbus_connection_send(). A call that carried
 * FDK_DBUS_FLAG_NO_REPLY_EXPECTED needs no reply (check
 * fdk_dbus_message_flags). If NO handler is installed, FDK answers
 * every call with org.freedesktop.DBus.Error.UnknownMethod itself —
 * a caller polling your app never burns a timeout on silence; WITH
 * a handler installed, replying (or deliberately not) is the
 * application's business and FDK never answers for you. */
typedef void (*fdk_dbus_method_fn)(fdk_dbus_connection *connection,
                                   fdk_dbus_message *call, void *user);

void fdk_dbus_connection_set_method_handler(fdk_dbus_connection *connection,
                                            fdk_dbus_method_fn fn,
                                            void *user);

#ifdef __cplusplus
}
#endif

#endif /* FDK_DBUS_H */
