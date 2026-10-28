// Portable (POSIX sockets + poll) driver for the wgnx engine. Runs unchanged
// on macOS/Linux for testing and on Horizon via libnx's BSD socket layer.
#pragma once

#include <poll.h>
#include <stdint.h>

#include "wgnx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WgnxDriver WgnxDriver;

// Called for every engine status event.
typedef void (*wgnx_event_fn)(void *ctx, const WgnxEvent *ev);

WgnxDriver *wgnx_driver_new(WgnxEngine *engine, wgnx_event_fn on_event, void *ctx);
void wgnx_driver_free(WgnxDriver *d);

// Opens the engine's UDP socket on `port` (0 = any) for direct peer paths,
// and advertises the local endpoint. Returns 0, or -1 on failure (the engine
// then works over DERP only).
int wgnx_driver_open_udp(WgnxDriver *d, uint16_t port);

// The last place the driver entered (a static string, NULL before any): if
// the thread driving it seems stuck, this names where it went last.
const char *wgnx_driver_step(void);

// Re-detects the local IP and re-advertises endpoints (call after network
// changes).
void wgnx_driver_refresh_endpoints(WgnxDriver *d);

// Closes every socket (the engine sees its connections drop and will
// reconnect), e.g. before the console sleeps. Resume reopens the UDP socket
// (on the same port if possible); the engine re-dials when next pumped.
void wgnx_driver_suspend(WgnxDriver *d);
int wgnx_driver_resume(WgnxDriver *d);

// Optional address rewrites for development, "host:port=ip:port,..." (e.g.
// reaching a container hostname through a published port). Copied.
void wgnx_driver_set_connect_map(WgnxDriver *d, const char *map);

// Where Taildrop puts received files. Called on whichever thread drives the
// engine, one file at a time. Failures return an HTTP status for the sender
// (e.g. 507 no space, 500 write error) with a message in why[why_cap].
typedef struct {
    void *ctx;
    // A peer offers `name` (validated: safe in any directory) of `size`
    // bytes (UINT64_MAX = unknown) from device `from`: 0 to take it (the
    // file is then open for writing), or a status to refuse.
    int (*open)(void *ctx, const char *name, uint64_t size, const char *from, char *why, size_t why_cap);
    // Appends data: 0, or a status (the transfer is then cancelled).
    int (*write)(void *ctx, const uint8_t *data, size_t len, char *why, size_t why_cap);
    // ok: everything arrived; save the file (0, or a status). !ok: discard
    // what was written (return value ignored).
    int (*close)(void *ctx, int ok, char *why, size_t why_cap);
} WgnxDropSink;

// Receives Taildrop files on `port` into `sink` (copied). Returns 0, or -1.
int wgnx_driver_set_taildrop(WgnxDriver *d, const WgnxDropSink *sink, uint16_t port);

// Whether the UDP socket is read a burst per call (Switch only): 1 yes, 0
// not known yet, -1 no (*bsd_errno: the socket service's errno if a call
// failed, else 0).
int wgnx_driver_batched_receive(WgnxDriver *d, int *bsd_errno);

// Most file descriptors the driver polls (connections + its UDP socket).
#define WGNX_DRIVER_MAX_FDS 33

// Split form of wgnx_driver_run_once, for callers that poll extra fds or
// must not hold a lock while blocked (see native/runtime):
//   n = wgnx_driver_prepare(d, fds, cap, max_wait, &wait);  // fills fds[0..n)
//   poll(fds, n + extra, wait);
//   wgnx_driver_dispatch(d, fds);                           // reads fds[0..n)
int wgnx_driver_prepare(WgnxDriver *d, struct pollfd *fds, int cap, int max_wait_ms, int *wait_ms);
void wgnx_driver_dispatch(WgnxDriver *d, const struct pollfd *fds);

// Optional, between poll and dispatch, on the thread that dispatches: reads
// datagrams the poll reported into the driver's own buffers. It touches no
// engine state, so a caller that serializes engine access can run it
// without that lock and hold the lock for less time. The dispatch after it
// then hands over only those (up to 16) and reads no more: a busy socket
// stays readable, and the next round's prefetch reads the rest.
void wgnx_driver_prefetch(WgnxDriver *d, const struct pollfd *fds);

// Performs engine I/O queued since the last call (e.g. after overlay socket
// operations) without waiting.
void wgnx_driver_pump(WgnxDriver *d);

// After wgnx_driver_prepare: whether a poll it set up, which waits until
// `until_ns` at the latest, no longer covers everything (a connection came
// or went or now waits to send, or the engine's next deadline moved
// earlier). Callers that wake a sleeping poll after engine calls only need
// to when this is true.
int wgnx_driver_poll_stale(WgnxDriver *d, uint64_t until_ns);

// Performs pending engine I/O, waits up to max_wait_ms for sockets or the
// engine's next deadline, and dispatches results. Returns 0, or -1 on a
// fatal driver error.
int wgnx_driver_run_once(WgnxDriver *d, int max_wait_ms);

// Monotonic clock in nanoseconds, as fed to the engine.
uint64_t wgnx_driver_now_ns(void);

#ifdef __cplusplus
}
#endif
