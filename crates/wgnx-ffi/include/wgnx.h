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
// Wall-clock time (Unix seconds) used for certificate validity checks (the
// update check's TLS).
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
// A WireGuard interface configured from a wg-quick style .conf. The caller
// owns the UDP socket and the clock: perform the sends it requests, feed
// datagrams back, answer WGNX_EVENT_RESOLVE, and call wgnx_engine_timeout by
// wgnx_engine_next_deadline. Times are monotonic nanoseconds.

typedef struct WgnxEngine WgnxEngine;

typedef struct {
    const char *conf;  // the .conf text (UTF-8; need not be NUL-terminated)
    size_t conf_len;
} WgnxConfig;

typedef struct {
    uint8_t family;  // 4 or 6
    uint8_t ip[16];  // IPv4 in the first 4 bytes
    uint16_t port;
} WgnxAddr;

typedef struct {
    uint8_t family;       // 4 or 6
    uint8_t len;          // prefix length
    uint8_t ip[16];       // IPv4 in the first 4 bytes
    uint8_t reserved[6];
} WgnxPrefix;             // 24 bytes

#define WGNX_IO_SEND_UDP 1u  // send data as one datagram to addr from the engine's UDP socket

typedef struct {
    uint32_t kind;
    uint32_t reserved;
    const uint8_t *data;  // valid until the next wgnx_engine_poll_io
    size_t len;
    WgnxAddr addr;
} WgnxIo;

#define WGNX_EVENT_LOG 0u
#define WGNX_EVENT_ADDRESSES 1u  // text "10.7.0.2/24 ..."; value = count. Once per reload.
#define WGNX_EVENT_PEERS 2u      // value = configured peers. Once per reload.
#define WGNX_EVENT_ERROR 3u
// value = peer index, text = its Endpoint's host name, port = the port:
// resolve it and call wgnx_engine_set_peer_endpoint. Repeated every 30 s
// while the peer stays silent (the C driver answers it itself unless a
// resolver hook takes over; see wgnx_driver.h).
#define WGNX_EVENT_RESOLVE 4u
#define WGNX_EVENT_HANDSHAKE 5u  // value = peer index, text = its name: a handshake completed
#define WGNX_EVENT_PEER_LOST 6u  // value = peer index, text = its name: no answer for 15 s; re-handshaking

typedef struct {
    uint32_t kind;
    uint32_t value;
    uint16_t port;     // RESOLVE
    uint16_t reserved;
    const char *text;  // valid until the next wgnx_engine_poll_event
} WgnxEvent;

// An interface with no configuration yet (no address, no peers).
WgnxEngine *wgnx_engine_new(uint64_t now_ns, uint64_t unix_secs);
// Parses and applies a .conf. On failure nothing changes and the reason
// ("line 12: PublicKey: not valid base64") is in err[err_cap] (may be NULL/0).
// Sessions start over; overlay socket ids stay valid.
bool wgnx_engine_reload(WgnxEngine *e, const WgnxConfig *cfg, uint64_t now_ns, char *err, size_t err_cap);
void wgnx_engine_free(WgnxEngine *e);
bool wgnx_engine_configured(WgnxEngine *e);
bool wgnx_engine_poll_io(WgnxEngine *e, WgnxIo *out);
bool wgnx_engine_poll_event(WgnxEngine *e, WgnxEvent *out);
// A datagram arrived on the engine's UDP socket.
void wgnx_engine_udp(WgnxEngine *e, const WgnxAddr *src, const uint8_t *data, size_t len, uint64_t now_ns);
void wgnx_engine_timeout(WgnxEngine *e, uint64_t now_ns);
uint64_t wgnx_engine_next_deadline(WgnxEngine *e);
// The network came back or changed (Wi-Fi after a wake, a new address):
// host names are resolved again and every reachable peer is contacted now.
void wgnx_engine_network_up(WgnxEngine *e, uint64_t now_ns);
// Answer to WGNX_EVENT_RESOLVE (any time; also overrides an endpoint).
void wgnx_engine_set_peer_endpoint(WgnxEngine *e, uint32_t peer, const WgnxAddr *addr, uint64_t now_ns);
// [Interface] ListenPort, 0 if none: bind the UDP socket there.
uint16_t wgnx_engine_listen_port(WgnxEngine *e);
uint16_t wgnx_engine_mtu(WgnxEngine *e);
// Our [Interface] addresses with prefix lengths. Returns how many there are.
size_t wgnx_engine_addresses(WgnxEngine *e, WgnxPrefix *out, size_t cap);
// Destinations that belong to the tunnel, for the socket layer: our own
// addresses (/32, /128), then every peer's AllowedIPs, each once. Fixed
// between reloads. Returns how many there are (can exceed cap).
size_t wgnx_engine_routes(WgnxEngine *e, WgnxPrefix *out, size_t cap);
// "a.b.c.d name\n" for this interface (if its .conf names it) and every
// peer with an IPv4 /32 AllowedIP; NUL-terminated, truncated to cap.
// Returns the full length (like snprintf).
size_t wgnx_engine_hosts(WgnxEngine *e, char *buf, size_t cap);

#define WGNX_PEER_SESSION 1u         // a handshake in the last 180 s
#define WGNX_PEER_ENDPOINT_NAME 2u   // Endpoint= is a host name ...
#define WGNX_PEER_RESOLVING 4u       // ... not resolved yet (nothing to send to)
#define WGNX_PEER_ROAMED 8u          // endpoint differs from the configured one (learned from traffic)
#define WGNX_PEER_KEEPALIVE 16u      // PersistentKeepalive set
#define WGNX_PEER_PSK 32u            // PresharedKey set
#define WGNX_PEER_DEFAULT_ROUTE 64u  // AllowedIPs has 0.0.0.0/0 or ::/0
typedef struct {
    char name[32];              // NUL-terminated; from "# Name = ..." or "peerN"
    uint8_t ipv4[4];            // first IPv4 /32 AllowedIP, else 0.0.0.0
    uint8_t endpoint_ip[4];     // current endpoint, 0.0.0.0 if none
    uint16_t endpoint_port;
    uint16_t keepalive_s;       // PersistentKeepalive, 0 = off
    uint32_t last_handshake_s;  // seconds ago; UINT32_MAX = never
    uint32_t last_rx_s;         // seconds since the last authenticated packet; UINT32_MAX = never
    uint32_t flags;             // WGNX_PEER_*
    uint64_t rx_bytes;          // wire bytes (like `wg show ... transfer`)
    uint64_t tx_bytes;
    uint32_t allowed_ips;       // prefix count
    uint32_t reserved;
} WgnxPeerInfo;                 // 80 bytes
#ifdef __cplusplus
static_assert(sizeof(WgnxPeerInfo) == 80, "WgnxPeerInfo layout");
static_assert(sizeof(WgnxPrefix) == 24, "WgnxPrefix layout");
#endif
// Every configured peer, in .conf order, up to max of them into out.
// Returns how many there are (can exceed max).
size_t wgnx_engine_peers(WgnxEngine *e, WgnxPeerInfo *out, size_t max);

// ---- Overlay sockets (tunnel side) ------------------------------------------
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
// Bytes of overlay socket buffers in use (TCP buffers, queued datagrams).
size_t wgnx_net_buffer_usage(WgnxEngine *e);
// Most bytes of overlay socket buffers allowed at once (new TCP connections
// get smaller buffers as it fills; past it they fail with WGNX_ENOBUFS).
void wgnx_net_set_budget(WgnxEngine *e, size_t bytes);
void wgnx_net_close(WgnxEngine *e, int32_t id);
int32_t wgnx_net_local_addr(WgnxEngine *e, int32_t id, WgnxAddr *out);
int32_t wgnx_net_peer_addr(WgnxEngine *e, int32_t id, WgnxAddr *out);

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
