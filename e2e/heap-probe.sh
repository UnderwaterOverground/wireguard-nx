#!/usr/bin/env bash
# Memory probe (run setup.sh first): replays a Moonlight-like session through
# the interpose shim, built with the Switch's buffer limits, and prints the
# Rust heap after each phase. See native/interpose/heap_probe.c.
set -euo pipefail
cd "$(dirname "$0")"
PEER_IP=100.64.0.1
docker compose build -q wgnx-build >/dev/null 2>&1
docker compose run --rm -e CARGO_PROFILE_FLAG=--release -e RUST_PROFILE_DIR=release \
  -e WGNX_FFI_FEATURES=switch-limits wgnx-build >/dev/null
docker compose exec -T web pkill -f udp-blaster >/dev/null 2>&1 || true
docker compose exec -T -d web python3 -c "$(cat udp-blaster.py)" 9000 udp-blaster
docker compose run --rm -T -e WGNX_CONTROL_URL=https://headscale -e WGNX_EXTRA_ROOT=/state/certs/ca.der \
  -e WGNX_STATE=/state/host/preload.state -e WGNX_AUTHKEY="$(cat state/authkey)" -e WGNX_HOSTNAME=wgnx-preload \
  -e WGNX_PORT=41642 -e WGNX_WAIT_READY=1 -e LD_PRELOAD=/src/target/linux/libwgnx_interpose.so \
  --entrypoint /src/target/linux/heap-probe wgnx "$PEER_IP"
