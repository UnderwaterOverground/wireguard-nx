// wgnx:ctl, the wireguard-nx sysmodule's control service (status, peers,
// pause, config, stop). Shared by the sysmodule and its clients (the overlay).
//
//   cmd 0  GetStatus()                -> WgnxCtlStatus (raw)
//   cmd 1  SetPaused(u8 paused)
//   cmd 2  GetLastError(out buffer)   -> u32 length (0: none)
//   cmd 3  Stop()                     -> u32 WGNX_CTL_STOP_*, u32 detail.
//          Ends the process unless that could disturb the console or an
//          app; the reply says why not. On WGNX_CTL_STOP_OK the process
//          exits shortly after replying: close the session and wait for it
//          to be gone before starting it again.
//   cmd 4  GetPeers(out buffer)       -> u32 count. Fills the buffer with up
//          to WGNX_CTL_MAX_PEERS WgnxPeerInfo (fewer if it's smaller): every
//          peer of the active .conf, in its order. The count is how many
//          there are, which can be more than were written.
//   cmd 5  GetConfName(out buffer)    -> u32 length: the active .conf's file
//          name ("wg0.conf"), "" if none could be loaded.
//   cmd 6  SetConf(in buffer name)    -> u32 WGNX_CTL_CONF_*: loads another
//          .conf from config/wireguard-nx and makes it the default.
#pragma once
#include <stdint.h>

#include "wgnx.h"

#define WGNX_CTL_SERVICE "wgnx:ctl"
#define WGNX_CTL_VERSION 1
#define WGNX_CTL_MAX_PEERS 32
// Free system memory below this is a warning: the next module to start
// (Nintendo's included) can fail to, and the console crashes (2001-0132).
#define WGNX_CTL_LOW_SYSTEM_FREE_KB 512
// Longest .conf file name (with ".conf"), including the NUL.
#define WGNX_CTL_CONF_NAME_MAX 64

enum {
    WGNX_CTL_STARTING = 0,    // waiting for the network / starting up
    WGNX_CTL_CONNECTING = 1,  // configured, no handshake with any peer yet
    WGNX_CTL_CONNECTED = 2,   // a peer handshake completed (peers_up says how many are fresh)
    WGNX_CTL_PAUSED = 3,      // switched off by the user (survives reboots)
    WGNX_CTL_STOPPING = 4,    // stopped from the overlay; exiting (or idle until restart)
};

enum {
    WGNX_CTL_STOP_OK = 0,        // exiting now
    WGNX_CTL_STOP_HOMEBREW = 1,  // refused: homebrew has sockets through us (detail: sessions)
    WGNX_CTL_STOP_BUSY = 2,      // refused: a sleep or wake, or setup, is under way; try again
    WGNX_CTL_STOP_FAILED = 3,    // couldn't finish safely: stays idle until the console restarts
};

enum {
    WGNX_CTL_CONF_OK = 0,
    WGNX_CTL_CONF_NOT_FOUND = 1,  // no such file in config/wireguard-nx
    WGNX_CTL_CONF_INVALID = 2,    // it doesn't parse; GetLastError says where
    WGNX_CTL_CONF_BUSY = 3,       // stopping, or the engine didn't answer; try again
};

enum {
    WGNX_CTL_FLAG_MITM = 1,        // homebrew sockets reach the tunnel
    WGNX_CTL_FLAG_HOSTS = 2,       // peer names in Atmosphere's hosts file
    WGNX_CTL_FLAG_OFFLINE = 4,     // the console has no internet connection
    WGNX_CTL_FLAG_CONF_ERROR = 8,  // the .conf didn't load (GetLastError says why); nothing is configured
    WGNX_CTL_FLAG_CONF_MISSING = 16,  // with CONF_ERROR: there was no .conf to load (GetLastError says what to do)
};

typedef struct {
    uint32_t version;    // WGNX_CTL_VERSION
    uint32_t state;      // WGNX_CTL_*
    uint8_t ipv4[4];     // our tunnel address, 0.0.0.0 if none
    uint32_t peers;      // configured
    uint32_t peers_up;   // with a handshake in the last 3 minutes
    uint32_t flags;      // WGNX_CTL_FLAG_*
    uint32_t memory_kb;       // what wireguard-nx takes (fixed), 0 if unknown
    uint32_t system_free_kb;  // free system memory, which sysmodules share
    // Seconds since the engine loop last ran a round (it runs at least once
    // a second), 0 below 3 s or while paused or asleep: anything more means
    // it's stuck.
    uint32_t engine_stalled_s;
    uint32_t reserved[7];
} WgnxCtlStatus;

#ifdef __cplusplus
static_assert(sizeof(WgnxCtlStatus) == 64, "WgnxCtlStatus layout");
#endif
