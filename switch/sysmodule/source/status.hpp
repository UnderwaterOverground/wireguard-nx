// What the sysmodule is doing, for wgnx:ctl clients (the overlay), plus the
// user-facing side effects of state changes: config/wireguard-nx/login.txt
// and an Ultrahand toast while a login is needed, and the pause switch.
#pragma once
#include <stratosphere.hpp>

extern "C" {
#include "wgnx.h"
}
#include "wgnx_ctl.h"

namespace wgnx {
    class Runtime;
}

namespace ams::status {

    // Once the runtime exists. Starts paused if the user left it paused.
    // `connect_notice`: a 5 s toast with our address on the first connection
    // after boot (unless the overlay turned it off).
    void Initialize(::wgnx::Runtime &rt, u32 flags, bool connect_notice);

    // A newer release (version without the "v") is out: a toast saying so.
    void AnnounceUpdate(const char *latest);
    // Little system memory was left free during boot: a toast saying so.
    void AnnounceLowMemory(u32 free_kb);

    // The engine loop ran a round (engine thread); GetStatus reports how
    // long ago, so a stuck engine shows as such in the overlay.
    void EngineRan(u64 now_ns);
    // What the engine loop is busy with outside the engine (nullptr when
    // done), named in the log if it gets stuck there.
    void EngineStep(const char *what);
    // Whether the console has an internet connection (engine thread).
    void SetOffline(bool offline);

    // Engine events (engine thread, runtime lock held).
    void OnEvent(const WgnxEvent &ev);
    // The engine's recently used peers (engine thread, every round): n is
    // how many are active, of which up to WGNX_CTL_MAX_PEERS are in peers.
    void SetActivePeers(const WgnxActivePeer *peers, size_t n);

    WgnxCtlStatus Get();
    size_t GetLoginUrl(char *out, size_t cap);
    size_t GetLastError(char *out, size_t cap);
    // Up to max of them into out; returns how many are active.
    size_t GetActivePeers(WgnxCtlPeer *out, size_t max);

    // A Taildrop transfer ended: a short toast either way.
    void AnnounceFile(bool saved, const char *name, u64 bytes, const char *from, const char *why);

    // Pauses or resumes Tailscale (persisted). Not from the engine thread.
    void SetPaused(bool paused);
    bool IsPaused();
    // Joined the tailnet (not starting, logging in or paused).
    bool IsConnected();

    // Stopped from the overlay: WGNX_CTL_STOPPING from now on, and no login
    // prompt left behind for a process that is going away.
    void SetStopping();

}
