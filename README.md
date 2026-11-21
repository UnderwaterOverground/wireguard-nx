# wireguard-nx

[![Release](https://img.shields.io/github/v/release/UnderwaterOverground/wireguard-nx)](https://github.com/UnderwaterOverground/wireguard-nx/releases/latest)
[![License](https://img.shields.io/badge/license-GPL--2.0%20%2F%20BSD--3-blue)](#license)
[![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white)](https://discord.gg/GjUuBEqRYb)

WireGuard for the Nintendo Switch. Runs in the background on Atmosphère and connects homebrew to your own WireGuard server, so you can use Moonlight, reach your NAS, or access your home network away from home.

No account, relay, or per-app setup. Just a standard WireGuard config.

> Unofficial project. Not affiliated with Nintendo or the WireGuard project. WireGuard and its logo are trademarks of Jason A. Donenfeld.

## Features

<img src="docs/images/connections.png" align="right" width="280" alt="WireGuard connections in Ultrahand">

- Runs at boot and reconnects after sleep.
- Works with homebrew launched through hbmenu, including supported home-screen forwarders.
- Uses regular WireGuard `.conf` files: multiple peers, pre-shared keys, hostnames and keepalives are supported.
- [AmneziaWG](#amneziawg) configs work as they are.
- Ultrahand overlay for connection status, peer handshakes, traffic, pause/start/stop and switching configs.
- Optional update notifications and [sys-ftpd](https://github.com/cathery/sys-ftpd) access over the tunnel.
- Fixed 2.5 MB memory usage.

## Requirements

- A Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere). Tested on a Mariko with Atmosphère 1.12.0 / firmware 22.5.0.
- [Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay) for the overlay and notifications (optional, but recommended).
- A WireGuard server you can add the Switch to. Unraid, UniFi, OPNsense, pfSense, Fritz!Box, wg-easy and `wg-quick` are all fine. For access away from home, the server needs to be reachable over UDP.

## Installation

**1. Download** the ZIP from the [latest release](https://github.com/UnderwaterOverground/wireguard-nx/releases/latest) (**Assets**, not “Source code”).

**2. Copy the files** to your Switch SD card. Power the Switch off first.

- **Windows:** merge the `atmosphere`, `config` and `switch` folders from the ZIP into the SD card root.
- **macOS:** don't drag the whole `atmosphere` folder over — Finder may replace the existing one. Copy `atmosphere/contents/4200000000005747` into `atmosphere/contents`, and `switch/.overlays/wireguard-nx.ovl` into `switch/.overlays`. Use **Cmd + Shift + .** to show hidden folders.

You should end up with:

```text
atmosphere/contents/4200000000005747/exefs.nsp
switch/.overlays/wireguard-nx.ovl
```

**3. Add a peer** for the Switch on your WireGuard server and save the client config it gives you in `config/wireguard-nx` on the SD card. Any name ending `.conf` works. Create the folder if needed. For example:

```ini
[Interface]
PrivateKey = <Switch private key>
Address = 10.253.0.2/32

[Peer]
# Name = gaming-pc
PublicKey = <server public key>
AllowedIPs = 10.253.0.1/32, 192.168.1.0/24
Endpoint = home.example.com:51820
PersistentKeepalive = 25
```

The Switch's private key comes from your server: adding a peer makes a new key pair, and the config it gives you already has the private key in it, so copy the file as it is. If your server asks for the Switch's *public* key instead (pfSense, plain `wg`), see [making the keys yourself](docs/SETUP.md#making-the-keys-yourself). Keep the file private: anyone with it can join your network as the Switch.

Make sure `AllowedIPs` covers the devices you want to reach. `0.0.0.0/0` routes all homebrew internet traffic through the server. `PersistentKeepalive = 25` helps keep the connection alive through NAT. The optional `# Name` comment gives the peer a name in Ultrahand and Moonlight.

**4. Boot the Switch.** Once Wi-Fi connects, you should get a connection notification. Open Ultrahand with **L + D-pad Down + R3**, then select **WireGuard** to see the tunnel address and how many peers are connected.

If it doesn't connect, check the overlay:

- **No config:** no `.conf` file in `config/wireguard-nx`. (One saved as `.conf.txt` is fine: it's renamed at boot.)
- **Config error:** the overlay shows which line failed.
- **No handshake yet:** check the endpoint, UDP port, keys and server-side peer. If you're away from home, check port forwarding.
- **No WireGuard menu:** check that `wireguard-nx.ovl` is in `switch/.overlays`.

More detail in the [setup and troubleshooting guide](docs/SETUP.md).

**Updates:** copy the files from the newer ZIP over the old ones and restart. Your configs and settings are kept.

## Moonlight

Add your PC by its WireGuard peer name or an IP reachable through the tunnel. **Using a name works better**, because Moonlight otherwise treats private IPs as local and sends packets large enough to require fragmentation.

These are the bitrates I found reliable at 60 FPS, tested against full-screen moving noise:

| Resolution | Added by name | Added by private IP |
|---|---|---|
| 720p | 25 Mbit/s | 20 Mbit/s |
| 1080p | 20 Mbit/s | Use 720p instead |

To name a peer, put `# Name = gaming-pc` under its `[Peer]` entry. If the PC is elsewhere on your server's LAN, add this anywhere in the config (the IP must be covered by `AllowedIPs`):

```ini
# Host = gaming-pc 192.168.1.50
```

Then add `gaming-pc` in Moonlight. wireguard-nx maps it to a stand-in address so Moonlight uses smaller packets. Names are written to Atmosphère's hosts file, so there needs to be one in `atmosphere/hosts` (most setups have one already). Artemis defaults to 35 Mbit/s at 1080p; lower that to 20.

## Multiple configs

Put additional `.conf` files in `config/wireguard-nx` and switch between them from Ultrahand's **Config** page. The selection is remembered after reboot.

## AmneziaWG

A config from the Amnezia app or `amneziawg-tools` works unchanged: the `Jc`/`Jmin`/`Jmax`, `S1`–`S4`, `H1`–`H4`, `I1`–`I5` and `HeaderProtectionKey` settings are read from `[Interface]` and the Switch sends junk, padded handshakes, disguised headers and, with a key, encrypted headers exactly as the server expects. Nothing else changes. A config without those keys is plain WireGuard.

AmneziaWG 3's other settings (`ContentPaddingAddition`, `RandomTrailers`, the timing keys) only change what a sender does; the Switch accepts them, accepts a server's random trailers, and sends plain-sized packets itself, which every AmneziaWG server takes. `S4` bytes ride on every packet, so the default MTU drops by `S4` unless `MTU` is set.

## Limitations

- **Homebrew only.** Games and Nintendo services don't use the tunnel. Home-screen forwarders with title IDs starting `01` need an entry in `mitm_titles` ([instructions](docs/SETUP.md#home-screen-forwarders)).
- One tunnel at a time. Server connections use IPv4; IPv6 *inside* the tunnel works.
- Tunnel DNS isn't supported. `DNS =` is ignored; `# Name` and `# Host` entries work instead.
- Devices on the Switch's current Wi-Fi network are reached directly, not through the tunnel.
- Use **Stop** in the WireGuard overlay to stop it without rebooting (close homebrew first). Disabling it in Sysmodules requires a restart.
- Tested on one Mariko Switch so far. Reports from other models are welcome.

## FAQ

**Will this affect online games or Nintendo services?** No. It only handles homebrew traffic to configured addresses, and leaves Atmosphère's Nintendo-server blocking alone.

**Will it keep working after a system update?** Minor updates (say 23.5 to 23.6) are fine. A new major version (24.0) needs a new wireguard-nx: until then it won't start, and the overlay tells you which firmware it runs on. Update Atmosphère first, as you would anyway. A new release normally follows soon after Atmosphère's, and the update notification lets you know when it's out.

**Can I run it alongside tailscale-nx?** Both can be installed, but only one can handle homebrew at a time. Disable the other in Sysmodules.

**Orange screen when waking during a Moonlight stream?** That's a [Moonlight-Switch issue](https://github.com/XITRIX/Moonlight-Switch/issues/306), also reproducible without wireguard-nx. End the stream before putting the Switch to sleep.

**Artemis won't launch twice?** Close hbmenu from HOME and reopen it. This happens without wireguard-nx too.

**Is the connection private?** WireGuard encrypts traffic between the Switch and your server. There's no account or relay. The optional update check contacts GitHub.

**Need help?** Check the [troubleshooting guide](docs/SETUP.md#troubleshooting), join [Discord](https://discord.gg/GjUuBEqRYb), or [open an issue](https://github.com/UnderwaterOverground/wireguard-nx/issues). Include `config/wireguard-nx/sysmodule.log` if reporting a bug.

## Building and credits

Build instructions are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). The build runs in Docker, and most tests don't need a Switch.

Forked from [tailscale-nx](https://github.com/UnderwaterOverground/tailscale-nx), replacing the Tailscale integration with plain WireGuard. Uses [tailscale-rs](https://github.com/tailscale/tailscale-rs), [libstratosphere](https://github.com/Atmosphere-NX/Atmosphere), [libnx](https://github.com/switchbrew/libnx), [libtesla](https://github.com/WerWolv/libtesla), [smoltcp](https://github.com/smoltcp-rs/smoltcp), [rustls](https://github.com/rustls/rustls) and [RustCrypto](https://github.com/RustCrypto). Also drew on [sys-GRID0](https://github.com/redluigi323/sys-GRID0), [ryu_ldn_nx](https://github.com/Ethiquema/ryu_ldn_nx) and [chrisbraucker's earlier wireguard-nx](https://github.com/chrisbraucker/wireguard-nx).

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party licences.

## License

The sysmodule, overlay and socket layer are GPL-2.0-or-later ([LICENSE](LICENSE)). The Rust engine under `crates/` is BSD-3-Clause ([crates/LICENSE](crates/LICENSE)).

## Found it useful?

If wireguard-nx helped you, a ⭐ on the repo is always appreciated. It's how
we know it's helping someone.
