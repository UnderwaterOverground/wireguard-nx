#!/usr/bin/env bash
# End-to-end tests against the local tailnet (run setup.sh first). Builds
# wgnx-host for Linux and runs it on the compose network next to peer1.
set -uo pipefail
cd "$(dirname "$0")"
PEER_IP=100.64.0.1
SELF=wgnx-e2e
fails=0
pass() { printf '\033[32mPASS\033[0m %s\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$1"; fails=$((fails + 1)); }

docker compose build -q wgnx-build >/dev/null 2>&1
docker compose run --rm -e CARGO_PROFILE_FLAG=--release -e RUST_PROFILE_DIR=release wgnx-build >/dev/null 2>&1 \
  || { echo "build failed"; exit 1; }
wgnx() { docker compose run --rm -T wgnx "$@" --control https://headscale --authkey "$(cat state/authkey)" \
  --hostname "$SELF" --state /state/host/e2e.state --port 41641; }
peer() { docker compose exec -T peer "$@"; }

# 1. Outbound TCP+UDP echo, upgrading to a direct path.
if wgnx up --echo-test "$PEER_IP" --expect-direct >/tmp/wgnx-e2e-1.log 2>&1; then pass "echo + direct path"; else fail "echo + direct path (see /tmp/wgnx-e2e-1.log)"; fi

# 2. Same with direct UDP blocked: must work over DERP.
peer iptables -I INPUT -s 172.30.0.30 -p udp -j DROP
peer iptables -I OUTPUT -d 172.30.0.30 -p udp -j DROP
if wgnx up --echo-test "$PEER_IP" >/tmp/wgnx-e2e-2.log 2>&1; then pass "echo via DERP only"; else fail "echo via DERP only (see /tmp/wgnx-e2e-2.log)"; fi
peer iptables -D INPUT -s 172.30.0.30 -p udp -j DROP
peer iptables -D OUTPUT -d 172.30.0.30 -p udp -j DROP

# 3. Inbound: the stock client pings us (disco, then ICMP through WireGuard).
docker compose run --rm -T -d --name wgnx-e2e-bg wgnx up --control https://headscale \
  --hostname "$SELF" --state /state/host/e2e.state --port 41641 >/dev/null
sleep 5
SELF_IP=$(peer tailscale status --json | python3 -c "import json,sys; print(next(p['TailscaleIPs'][0] for p in json.load(sys.stdin)['Peer'].values() if p['HostName']=='$SELF'))")
if peer tailscale ping -c 5 "$SELF_IP" | grep -q "via 172.30.0.30"; then pass "inbound disco ping (direct)"; else fail "inbound disco ping"; fi
if peer ping -c 5 -W 2 "$SELF_IP" >/dev/null; then pass "inbound ICMP"; else fail "inbound ICMP"; fi
docker rm -f wgnx-e2e-bg >/dev/null

# 4-6. Unmodified programs through the socket virtualization layer (the code
# the Switch MITM uses), via LD_PRELOAD.
preload() {
  docker compose run --rm -T -e WGNX_CONTROL_URL=https://headscale -e WGNX_EXTRA_ROOT=/state/certs/ca.der \
    -e WGNX_STATE=/state/host/preload.state -e WGNX_AUTHKEY="$(cat state/authkey)" -e WGNX_HOSTNAME=wgnx-preload \
    -e WGNX_PORT=41642 -e WGNX_WAIT_READY=1 -e LD_PRELOAD=/src/target/linux/libwgnx_interpose.so \
    --entrypoint sh wgnx -c "$1"
}
# Warm up once so the preload node's keys exist and peers know them.
preload "curl -s -m 10 -o /dev/null http://$PEER_IP:8080/" >/dev/null 2>&1
if preload "/src/target/linux/vsock-test $PEER_IP 172.30.0.10:443" >/tmp/wgnx-e2e-4.log 2>&1; then
  pass "socket virtualization suite (vsock-test)"
else
  fail "socket virtualization suite (see /tmp/wgnx-e2e-4.log)"
fi
if preload "curl -s -m 10 http://$PEER_IP:8080/" 2>/dev/null | grep -q "<html"; then pass "unmodified curl over the tailnet"; else fail "unmodified curl"; fi
udp=$(preload "iperf3 -c $PEER_IP -u -b 50M -l 1200 -t 4 -R -f m 2>&1 | grep receiver" 2>/dev/null)
mbps=$(echo "$udp" | awk '{for (i=1;i<=NF;i++) if ($i=="Mbits/sec") print $(i-1)}')
if awk "BEGIN{exit !(${mbps:-0} >= 47)}"; then pass "UDP 50 Mbit/s stream (got ${mbps} Mbit/s)"; else fail "UDP 50 Mbit/s stream (got ${mbps:-none})"; fi
tcp=$(preload "iperf3 -c $PEER_IP -t 3 -f m 2>&1 | grep receiver" 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="Mbits/sec") print $(i-1)}')
if awk "BEGIN{exit !(${tcp:-0} >= 100)}"; then pass "TCP bulk upload (got ${tcp} Mbit/s)"; else fail "TCP bulk upload (got ${tcp:-none})"; fi

# 7. Access rules: the policy (headscale/policy.hujson) allows everything but
# port 7777, and our node must enforce that for inbound connections.
docker rm -f wgnx-acl >/dev/null 2>&1
docker compose run --rm -T -d --name wgnx-acl -e WGNX_CONTROL_URL=https://headscale \
  -e WGNX_EXTRA_ROOT=/state/certs/ca.der -e WGNX_STATE=/state/host/acl.state \
  -e WGNX_AUTHKEY="$(cat state/authkey)" -e WGNX_HOSTNAME=wgnx-acl -e WGNX_PORT=41645 -e WGNX_WAIT_READY=1 \
  -e LD_PRELOAD=/src/target/linux/libwgnx_interpose.so --entrypoint python3 wgnx -c '
import socket, threading
socket.socket().close()  # starts the engine (the shim starts it on first use)
def serve(port):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port)); s.listen(4)
    while True: s.accept()[0].close()
for p in (7000, 7001, 7777): threading.Thread(target=serve, args=(p,), daemon=True).start()
threading.Event().wait()' >/dev/null
ACL_IP=""
for _ in $(seq 30); do
  ACL_IP=$(peer tailscale status --json 2>/dev/null | python3 -c "import json,sys; print(next((p['TailscaleIPs'][0] for p in json.load(sys.stdin)['Peer'].values() if p['HostName']=='wgnx-acl' and p.get('Online')), ''))")
  [[ -n "$ACL_IP" ]] && peer nc -z -w 3 "$ACL_IP" 7000 2>/dev/null && break
  sleep 1
done
# 7000 and 7001: two listeners both reachable (so 7777 failing is the rules).
if [[ -n "$ACL_IP" ]] && peer nc -z -w 3 "$ACL_IP" 7000 2>/dev/null && peer nc -z -w 3 "$ACL_IP" 7001 2>/dev/null &&
   ! peer nc -z -w 3 "$ACL_IP" 7777 2>/dev/null; then
  pass "access rules enforced for inbound connections"
else
  fail "access rules enforced for inbound connections (ip ${ACL_IP:-none})"
fi
docker rm -f wgnx-acl >/dev/null 2>&1

# 8. Node key expiry: a fresh node key replaces the expired one (keeping the
# node's identity and address). With a reusable auth key that happens on its
# own while running; without one, the server answers with a login URL.
hs() { docker compose exec -T headscale headscale "$@" 2>/dev/null; }
node_id() { hs nodes list -o json | python3 -c "import json,sys; print(next((n['id'] for n in json.load(sys.stdin) if n.get('given_name')=='$1'), ''))"; }
wait_log() {  # container, pattern, seconds[, count]
  for _ in $(seq "$3"); do
    [[ $(docker logs "$1" 2>&1 | grep -c "$2") -ge ${4:-1} ]] && return 0
    sleep 1
  done
  return 1
}
EXP=wgnx-e2e-expiry
docker run --rm -v "$PWD/state:/s" alpine rm -f /s/host/expiry-e2e.state
old_id=$(node_id "$EXP"); [[ -n "$old_id" ]] && hs nodes delete -i "$old_id" --force >/dev/null
expiry_client() {  # extra args...
  docker compose run --rm -T -d --name "$EXP" wgnx up --control https://headscale --hostname "$EXP" \
    --state /state/host/expiry-e2e.state --port 41644 "$@" >/dev/null
}
expiry_client --authkey "$(cat state/authkey)"
if wait_log "$EXP" Authorized 30; then
  id=$(node_id "$EXP")
  key1=$(grep node= state/host/expiry-e2e.state)
  hs nodes expire -i "$id" >/dev/null
  if wait_log "$EXP" NodeKeyChanged 40 && wait_log "$EXP" Authorized 30 2 &&
     [[ "$(grep node= state/host/expiry-e2e.state)" != "$key1" ]] && [[ "$(node_id "$EXP")" == "$id" ]]; then
    pass "expired key replaced while running (auth key)"
  else
    fail "expired key replaced while running (see docker logs)"
  fi
  docker rm -f "$EXP" >/dev/null
  key2=$(grep node= state/host/expiry-e2e.state)
  hs nodes expire -i "$id" >/dev/null
  expiry_client
  if wait_log "$EXP" LoginUrl 30; then
    req=$(docker logs "$EXP" 2>&1 | grep -o "hskey-authreq-[A-Za-z0-9_-]*" | tail -1)
    hs nodes register --user wgnx --key "$req" >/dev/null
    if wait_log "$EXP" Authorized 30 && [[ "$(grep node= state/host/expiry-e2e.state)" != "$key2" ]] &&
       [[ "$(node_id "$EXP")" == "$id" ]]; then
      pass "expired key replaced at startup via login URL"
    else
      fail "expired key replaced at startup via login URL"
    fi
  else
    fail "expired key at startup: no login URL"
  fi
  hs nodes delete -i "$id" --force >/dev/null
else
  fail "node key expiry: initial registration"
fi
docker rm -f "$EXP" >/dev/null 2>&1

# 9. Taildrop: the stock client sends files with `tailscale file cp`.
DROP=wgnx-drop
docker rm -f "$DROP" >/dev/null 2>&1
docker compose run --rm -T -d --name "$DROP" -e WGNX_CONTROL_URL=https://headscale \
  -e WGNX_EXTRA_ROOT=/state/certs/ca.der -e WGNX_STATE=/state/host/drop.state \
  -e WGNX_AUTHKEY="$(cat state/authkey)" -e WGNX_HOSTNAME="$DROP" -e WGNX_PORT=41646 -e WGNX_WAIT_READY=1 \
  -e WGNX_TAILDROP_DIR=/tmp/Taildrop -e LD_PRELOAD=/src/target/linux/libwgnx_interpose.so \
  --entrypoint python3 wgnx -c 'import socket, threading; socket.socket().close(); threading.Event().wait()' >/dev/null
for _ in $(seq 30); do
  peer tailscale file cp --targets 2>/dev/null | grep -q "$DROP" && break
  sleep 1
done
sha() { awk '{print $1}'; }
peer sh -c 'head -c 3000000 /dev/urandom > /tmp/td.bin && echo small > /tmp/td2.txt'
want=$(peer sha256sum /tmp/td.bin | sha)
if peer tailscale file cp /tmp/td.bin "$DROP:" && peer tailscale file cp /tmp/td.bin "$DROP:" &&
   [[ "$(docker exec "$DROP" sha256sum /tmp/Taildrop/td.bin | sha)" == "$want" ]] &&
   [[ "$(docker exec "$DROP" sha256sum "/tmp/Taildrop/td (1).bin" | sha)" == "$want" ]]; then
  pass "Taildrop receive (3 MB, twice: no overwrite)"
else
  fail "Taildrop receive (see docker logs $DROP)"
fi
# Unknown length (from stdin): sent chunked.
if peer sh -c "cat /tmp/td2.txt | tailscale file cp --name=streamed.txt - $DROP:" &&
   [[ "$(docker exec "$DROP" cat /tmp/Taildrop/streamed.txt)" == "small" ]] &&
   ! docker exec "$DROP" sh -c 'ls /tmp/Taildrop | grep -q partial'; then
  pass "Taildrop receive of unknown length"
else
  fail "Taildrop receive of unknown length"
fi
docker rm -f "$DROP" >/dev/null 2>&1

# 10. Subnet routes: peer1 serves 10.123.0.0/24 (its web server answers on
# 10.123.0.1, an address only it has), which we use only when accepting routes.
hs() { docker compose exec -T headscale headscale "$@" 2>/dev/null; }
peer sh -c 'ip addr show dev lo | grep -q 10.123.0.1 || ip addr add 10.123.0.1/32 dev lo'
peer tailscale set --advertise-routes=10.123.0.0/24
peer_id=$(hs nodes list -o json | python3 -c "import json,sys; print(next(n['id'] for n in json.load(sys.stdin) if n.get('given_name')=='peer1'))")
hs nodes approve-routes -i "$peer_id" -r 10.123.0.0/24 >/dev/null
routed() {  # WGNX_ACCEPT_ROUTES value
  docker compose run --rm -T -e WGNX_CONTROL_URL=https://headscale -e WGNX_EXTRA_ROOT=/state/certs/ca.der \
    -e WGNX_STATE=/state/host/routes.state -e WGNX_AUTHKEY="$(cat state/authkey)" -e WGNX_HOSTNAME=wgnx-routes \
    -e WGNX_PORT=41647 -e WGNX_WAIT_READY=1 -e WGNX_ACCEPT_ROUTES="$1" -e LD_PRELOAD=/src/target/linux/libwgnx_interpose.so \
    --entrypoint sh wgnx -c "curl -s -m 8 http://10.123.0.1:8080/"
}
if routed 1 2>/dev/null | grep -q "<html" && ! routed 0 2>/dev/null | grep -q "<html"; then
  pass "subnet routes (used only when accepted)"
else
  fail "subnet routes"
fi
peer tailscale set --advertise-routes=
peer ip addr del 10.123.0.1/32 dev lo

# 11. Losing the control server: the engine reports the map stream gone and
# back (what turns the overlay's "Connected" into "Reconnecting...").
CTL=wgnx-e2e-control
docker rm -f "$CTL" >/dev/null 2>&1
docker run --rm -v "$PWD/state:/s" alpine rm -f /s/host/control-e2e.state
docker compose run --rm -T -d --name "$CTL" wgnx up --control https://headscale --authkey "$(cat state/authkey)" \
  --hostname "$CTL" --state /state/host/control-e2e.state --port 41648 >/dev/null
if wait_log "$CTL" "Control(true)" 30; then
  docker compose restart headscale >/dev/null 2>&1
  if wait_log "$CTL" "Control(false)" 30 && wait_log "$CTL" "Control(true)" 60 2; then
    pass "control server lost and regained"
  else
    fail "control server lost and regained (see docker logs $CTL)"
  fi
else
  fail "control server: no map stream"
fi
id=$(node_id "$CTL"); [[ -n "$id" ]] && hs nodes delete -i "$id" --force >/dev/null
docker rm -f "$CTL" >/dev/null 2>&1

[[ $fails -eq 0 ]] && echo "all e2e tests passed" || { echo "$fails e2e test(s) failed"; exit 1; }
