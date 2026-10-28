#!/bin/bash
# Brings up the WireGuard interface from /etc/wireguard/wg0.conf, adds the
# "subnet" address the route tests reach (10.123.0.1), and runs TCP/UDP echo.
set -euo pipefail
wg-quick up wg0
ip addr add 10.123.0.1/32 dev lo || true
socat TCP-LISTEN:7,fork,reuseaddr EXEC:cat &
socat UDP-RECVFROM:7,fork EXEC:cat &
wg show
exec sleep infinity
