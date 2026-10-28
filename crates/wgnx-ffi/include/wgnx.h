// C interface to the wgnx Rust core (crates/wgnx-ffi).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Selftest failure codes returned by wgnx_selftest().
enum wgnx_selftest_failure {
    WGNX_SELFTEST_OK = 0,
    WGNX_SELFTEST_X25519 = 1,
    WGNX_SELFTEST_AEAD_SEAL = 2,
    WGNX_SELFTEST_AEAD_OPEN = 3,
    WGNX_SELFTEST_AEAD_TAMPER = 4,
    WGNX_SELFTEST_BLAKE2S = 5,
};

const char *wgnx_version(void);
uint32_t wgnx_selftest(void);
uint64_t wgnx_bench_aead(size_t packet_len, uint32_t iterations);
uint64_t wgnx_bench_x25519(uint32_t iterations);

// Routes the core's log records to cb (level 1 error .. 5 trace; msg valid
// only during the call). max_level 0 disables logging.
typedef void (*wgnx_log_fn)(uint32_t level, const char *msg);
void wgnx_set_log_callback(wgnx_log_fn cb, uint32_t max_level);

// Rust heap usage (bytes): current and high-water mark.
void wgnx_heap_stats(size_t *current, size_t *peak);

// Live Rust heap by allocation size class as text ("<=64:12K/310 ..." =
// KB/allocations per class), NUL-terminated. Returns the length written.
size_t wgnx_heap_classes(char *buf, size_t len);

// Seeds the core's CSPRNG with 32 bytes of entropy. Call once at startup.
void wgnx_seed_rng(const uint8_t seed[32]);
// Wall-clock time (Unix seconds) used for certificate validity checks.
void wgnx_set_unix_time(uint64_t unix_secs);

// Standalone TLS 1.3 session; the caller moves bytes between it and a socket.
typedef struct WgnxTls WgnxTls;
#define WGNX_TLS_ESTABLISHED 1u
#define WGNX_TLS_PEER_CLOSED 2u
#define WGNX_TLS_FAILED 4u
WgnxTls *wgnx_tls_new(const char *server_name);
void wgnx_tls_free(WgnxTls *s);
int wgnx_tls_feed(WgnxTls *s, const uint8_t *data, size_t len);
int wgnx_tls_write(WgnxTls *s, const uint8_t *data, size_t len);
size_t wgnx_tls_take_outgoing(WgnxTls *s, uint8_t *buf, size_t cap);
size_t wgnx_tls_read(WgnxTls *s, uint8_t *buf, size_t cap);
uint32_t wgnx_tls_state(const WgnxTls *s);
const char *wgnx_tls_last_error(const WgnxTls *s);

// ---- Engine ----------------------------------------------------------------
// A complete Tailscale node. The caller owns sockets and the clock: perform the
// I/O it requests, report results back, and call wgnx_engine_timeout by
// wgnx_engine_next_deadline. Times are monotonic nanoseconds.

typedef struct WgnxEngine WgnxEngine;

typedef struct {
    const char *control_url;  // e.g. "https://controlplane.tailscale.com"
    const char *auth_key;     // may be NULL (interactive login)
    const char *hostname;
    const uint8_t *machine_key;  // 32-byte private keys, persisted by the caller
    const uint8_t *node_key;
    const uint8_t *disco_key;    // persist too (NULL = new per run; peers then reset sessions)
    const uint8_t *extra_root_der;  // optional trusted root (DER), for dev servers
    size_t extra_root_len;
} WgnxConfig;

#define WGNX_IO_CONNECT 1u  // resolve host (or parse IP literal), TCP connect to port
#define WGNX_IO_SEND 2u     // write data to connection id
#define WGNX_IO_CLOSE 3u    // close connection id
#define WGNX_IO_SEND_UDP 4u // send data as one datagram to addr from the engine's UDP socket

typedef struct {
    uint8_t family;  // 4 or 6
    uint8_t ip[16];  // IPv4 in the first 4 bytes
    uint16_t port;
} WgnxAddr;

typedef struct {
    uint32_t kind;
    uint32_t id;
    const char *host;
    uint16_t port;
    const uint8_t *data;  // valid until the next wgnx_engine_poll_io
    size_t len;
    WgnxAddr addr;        // SEND_UDP destination
} WgnxIo;

#define WGNX_EVENT_LOG 0u
#define WGNX_EVENT_LOGIN_URL 1u
#define WGNX_EVENT_AUTHORIZED 2u
#define WGNX_EVENT_ADDRESSES 3u
#define WGNX_EVENT_PEERS 4u
#define WGNX_EVENT_HOME_DERP 5u
#define WGNX_EVENT_ERROR 6u
#define WGNX_EVENT_ENDPOINTS 7u  // our advertised direct endpoints (text)
#define WGNX_EVENT_PEER_PATH 8u  // value 1 = direct, 0 = DERP; text describes it
// Our node key expired and was replaced: save wgnx_engine_node_key() as the
// state file's node= key (no text; the key is never put in an event).
#define WGNX_EVENT_NODE_KEY 9u
// The control server's map stream: value 1 = up, 0 = lost (it retries).
#define WGNX_EVENT_CONTROL 10u

typedef struct {
    uint32_t kind;
    uint32_t value;    // peer count, DERP region, ...
    const char *text;  // valid until the next wgnx_engine_poll_event
} WgnxEvent;

WgnxEngine *wgnx_engine_new(const WgnxConfig *cfg, uint64_t now_ns, uint64_t unix_secs);
void wgnx_engine_free(WgnxEngine *e);
bool wgnx_engine_poll_io(WgnxEngine *e, WgnxIo *out);
bool wgnx_engine_poll_event(WgnxEngine *e, WgnxEvent *out);
void wgnx_engine_connected(WgnxEngine *e, uint32_t id, uint64_t now_ns);
void wgnx_engine_data(WgnxEngine *e, uint32_t id, const uint8_t *data, size_t len, uint64_t now_ns);
void wgnx_engine_closed(WgnxEngine *e, uint32_t id, uint64_t now_ns);
// A datagram arrived on the engine's UDP socket.
void wgnx_engine_udp(WgnxEngine *e, const WgnxAddr *src, const uint8_t *data, size_t len, uint64_t now_ns);
// Local UDP endpoints (interface address + bound port) to advertise to peers.
void wgnx_engine_set_local_endpoints(WgnxEngine *e, const WgnxAddr *eps, size_t n, uint64_t now_ns);
void wgnx_engine_timeout(WgnxEngine *e, uint64_t now_ns);
// The network came back (offline -> online): control and DERP retry now
// instead of waiting out their backoff. Leaves connections that are up alone.
void wgnx_engine_network_up(WgnxEngine *e, uint64_t now_ns);
uint64_t wgnx_engine_next_deadline(WgnxEngine *e);

// ---- Overlay sockets (tailnet side) -------------------------------------------
// Non-blocking. Ids are > 0; errors are negative WGNX_E* codes.

#define WGNX_EAGAIN (-1)
#define WGNX_ENOTCONN (-2)
#define WGNX_EBADF (-3)
#define WGNX_EADDRINUSE (-4)
#define WGNX_ECONNREFUSED (-5)
#define WGNX_ECONNRESET (-6)
#define WGNX_EINVAL (-7)
#define WGNX_ENOBUFS (-8)

#define WGNX_READABLE 1u
#define WGNX_WRITABLE 2u
#define WGNX_HUP 4u

int32_t wgnx_net_tcp_connect(WgnxEngine *e, const WgnxAddr *dst);
int32_t wgnx_net_tcp_listen(WgnxEngine *e, uint16_t port, uint32_t backlog);
int32_t wgnx_net_tcp_accept(WgnxEngine *e, int32_t id, WgnxAddr *peer);
int32_t wgnx_net_send(WgnxEngine *e, int32_t id, const uint8_t *data, size_t len);
int32_t wgnx_net_recv(WgnxEngine *e, int32_t id, uint8_t *buf, size_t len);
int32_t wgnx_net_shutdown(WgnxEngine *e, int32_t id);
int32_t wgnx_net_udp_bind(WgnxEngine *e, uint16_t port);
int32_t wgnx_net_udp_sendto(WgnxEngine *e, int32_t id, const uint8_t *data, size_t len, const WgnxAddr *dst);
int32_t wgnx_net_udp_recvfrom(WgnxEngine *e, int32_t id, uint8_t *buf, size_t len, WgnxAddr *src);
uint32_t wgnx_net_readiness(WgnxEngine *e, int32_t id);
// Writes "a.b.c.d name\n" for this node and every peer (MagicDNS names, no
// trailing dot), NUL-terminated and truncated to cap. Returns the full length.
size_t wgnx_engine_hosts(WgnxEngine *e, char *buf, size_t cap);
// Peers we sent to in the last 45 s, most recent first: up to max of them
// into out. Returns how many there are (can be more than max).
typedef struct {
    char name[32];          // first label of its name, NUL-terminated
    uint8_t direct;         // 1: confirmed direct path; 0: relayed (DERP)
    uint8_t reserved;
    uint16_t latency_ms;    // the path's round trip, direct or relayed (0 if unknown)
    uint32_t relay_region;  // when relayed: the DERP region (0 if unknown)
} WgnxActivePeer;
size_t wgnx_engine_active_peers(WgnxEngine *e, WgnxActivePeer *out, size_t max);
// Our current private node key (32 bytes), for the state file.
void wgnx_engine_node_key(WgnxEngine *e, uint8_t out[32]);
// Bytes of overlay socket buffers in use (TCP buffers, queued datagrams).
size_t wgnx_net_buffer_usage(WgnxEngine *e);
// Most bytes of overlay socket buffers allowed at once (new TCP connections
// get smaller buffers as it fills; past it they fail with WGNX_ENOBUFS).
void wgnx_net_set_budget(WgnxEngine *e, size_t bytes);
void wgnx_net_close(WgnxEngine *e, int32_t id);
int32_t wgnx_net_local_addr(WgnxEngine *e, int32_t id, WgnxAddr *out);
int32_t wgnx_net_peer_addr(WgnxEngine *e, int32_t id, WgnxAddr *out);

// Taildrop receive (the peer API's PUT /v0/put/<name>). After enabling, the
// driver polls for events: OFFER (answer with accept, or finish with an
// error status), then reads the body with wgnx_taildrop_read until COMPLETE
// (then finish with 200 once the file is saved) or ABORTED (discard it; no
// finish needed). The engine never touches files.
#define WGNX_DROP_OFFER 1u
#define WGNX_DROP_COMPLETE 2u
#define WGNX_DROP_ABORTED 3u

typedef struct {
    uint32_t kind;
    uint32_t id;
    uint64_t size;     // OFFER: announced size (UINT64_MAX = unknown); COMPLETE: bytes received
    const char *name;  // OFFER: validated file name (safe in any directory)
    const char *text;  // OFFER: sending device; ABORTED: why
} WgnxDropEvent;      // strings valid until the next wgnx_taildrop_poll

// Listens on `port` of our tailnet addresses and advertises it. 0 or -1.
int32_t wgnx_taildrop_enable(WgnxEngine *e, uint16_t port);
bool wgnx_taildrop_poll(WgnxEngine *e, WgnxDropEvent *out);
void wgnx_taildrop_accept(WgnxEngine *e, uint32_t id, uint64_t now_ns);
// Body bytes into buf; 0 when none are available yet or the transfer ended.
size_t wgnx_taildrop_read(WgnxEngine *e, uint32_t id, uint8_t *buf, size_t len, uint64_t now_ns);
// Ends a transfer: 200 when saved, else an HTTP error status (e.g. 507 no
// space, 500 write failed) and a message the sender shows.
void wgnx_taildrop_finish(WgnxEngine *e, uint32_t id, uint16_t status, const char *msg, uint64_t now_ns);

// Subnet routes (like `tailscale up --accept-routes`; off by default).
typedef struct {
    uint8_t family;  // 4 or 6
    uint8_t len;     // prefix length
    uint8_t ip[16];  // IPv4 uses the first 4 bytes
} WgnxPrefix;
void wgnx_engine_set_accept_routes(WgnxEngine *e, bool on);
// Copies up to cap routes in use into out; returns how many there are.
size_t wgnx_engine_routes(WgnxEngine *e, WgnxPrefix *out, size_t cap);

// Must be provided by the embedding program on no_std targets. Called with a
// (non NUL-terminated) panic message; must not return.
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void wgnx_platform_panic(const uint8_t *msg, size_t len);

#ifdef __cplusplus
}
#endif
