#!/bin/bash
# Brings up the AmneziaWG interface from /etc/amnezia/amneziawg/awg0.conf
# (awg-quick starts amneziawg-go, there being no kernel module) and runs
# TCP/UDP echo on port 7.
set -euo pipefail
awg-quick up awg0
socat TCP-LISTEN:7,fork,reuseaddr EXEC:cat &
socat UDP-RECVFROM:7,fork EXEC:cat &
awg show
exec sleep infinity
