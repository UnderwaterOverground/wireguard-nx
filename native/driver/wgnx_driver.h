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

// Opens the engine's UDP socket on `port` (0 = any). Returns 0, or -1 on
// failure (the engine then cannot talk to anyone).
int wgnx_driver_open_udp(WgnxDriver *d, uint16_t port);
// The port the UDP socket is bound to (0 if none).
uint16_t wgnx_driver_udp_port(const WgnxDriver *d);

// The last place the driver entered (a static string, NULL before any): if
// the thread driving it seems stuck, this names where it went last.
const char *wgnx_driver_step(void);

// Closes the UDP socket, e.g. before the console sleeps. Resume reopens it
// (on the same port if possible); call wgnx_engine_network_up afterwards
// so peers are contacted again at once.
void wgnx_driver_suspend(WgnxDriver *d);
int wgnx_driver_resume(WgnxDriver *d);

// Whether the driver answers WGNX_EVENT_RESOLVE itself with a blocking
// wgnx_resolve_ipv4 (default: yes) before forwarding the event. Embedders
// that must not block the engine thread turn this off and resolve from the
// event on their own, then call wgnx_engine_set_peer_endpoint.
void wgnx_driver_resolve_inline(WgnxDriver *d, int on);

// Whether the UDP socket is read a burst per call (Switch only): 1 yes, 0
// not known yet, -1 no (*bsd_errno: the socket service's errno if a call
// failed, else 0).
int wgnx_driver_batched_receive(WgnxDriver *d, int *bsd_errno);

// Most file descriptors the driver polls (its UDP socket).
#define WGNX_DRIVER_MAX_FDS 1

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
// `until_ns` at the latest, no longer covers everything (the socket was
// reopened, or the engine's next deadline moved earlier). Callers that wake
// a sleeping poll after engine calls only need to when this is true.
int wgnx_driver_poll_stale(WgnxDriver *d, uint64_t until_ns);

// Performs pending engine I/O, waits up to max_wait_ms for the socket or
// the engine's next deadline, and dispatches results. Returns 0, or -1 on
// a fatal driver error.
int wgnx_driver_run_once(WgnxDriver *d, int max_wait_ms);

// Monotonic clock in nanoseconds, as fed to the engine.
uint64_t wgnx_driver_now_ns(void);

#ifdef __cplusplus
}
#endif
