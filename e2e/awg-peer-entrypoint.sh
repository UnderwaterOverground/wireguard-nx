#!/bin/bash
# Brings up the AmneziaWG interfaces from /etc/amnezia/amneziawg/awg{0,1}.conf
# (awg-quick starts amneziawg-go, there being no kernel module) and runs
# TCP/UDP echo on port 7 and an iperf3 server.
set -euo pipefail
awg-quick up awg0
[[ -s /etc/amnezia/amneziawg/awg1.conf ]] && awg-quick up awg1
socat TCP-LISTEN:7,fork,reuseaddr EXEC:cat &
socat UDP-RECVFROM:7,fork EXEC:cat &
iperf3 -s -D
awg show
exec sleep infinity
