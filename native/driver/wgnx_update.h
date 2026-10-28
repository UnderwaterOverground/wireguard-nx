// Checks GitHub for a newer wireguard-nx release: one HTTPS request for
// github.com/<repo>/releases/latest, whose redirect names the latest tag.
// Non-blocking after the start: call wgnx_update_step until it returns
// something other than WGNX_UPDATE_PENDING, then wgnx_update_free.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where releases are published.
#define WGNX_UPDATE_REPO "UnderwaterOverground/wireguard-nx"

typedef struct WgnxUpdate WgnxUpdate;

enum {
    WGNX_UPDATE_FAILED = -1,  // wgnx_update_error says why
    WGNX_UPDATE_PENDING = 0,
    WGNX_UPDATE_CURRENT = 1,  // no newer release
    WGNX_UPDATE_NEWER = 2,    // wgnx_update_latest has its version
};

// Resolves github.com (blocking, like the driver's connects) and starts
// connecting. `current` is our version ("1.0.1"). The check gives up
// `timeout_ms` after now_ns. NULL only if out of memory.
WgnxUpdate *wgnx_update_start(const char *repo, const char *current, uint64_t now_ns, uint32_t timeout_ms);
int wgnx_update_step(WgnxUpdate *u, uint64_t now_ns);
// The latest release's version without the "v" ("1.1.0"), or "" if unknown.
const char *wgnx_update_latest(const WgnxUpdate *u);
const char *wgnx_update_error(const WgnxUpdate *u);
// Closes the connection.
void wgnx_update_free(WgnxUpdate *u);

// Compares dotted versions numerically ("1.10.0" > "1.9.2"); anything after
// the numbers ("-beta") is ignored. Returns <0, 0 or >0.
int wgnx_version_compare(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
