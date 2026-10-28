#!/usr/bin/env bash
# Generates keys and .conf files under state/ and brings up the peers.
# Idempotent; re-run after `down.sh` for a fresh set of keys.
#
# Clients (all with the server as a peer unless noted):
#   client   10.7.0.2  also peer2 and the 10.123.0.0/24 subnet
#   preload  10.7.0.3  for the LD_PRELOAD shim (with the subnet)
#   nosubnet           preload's keys without the subnet
#   bg       10.7.0.4  PersistentKeepalive 3
#   psk      10.7.0.5  with a PresharedKey (badpsk: the same with a wrong one)
#   roam     10.7.0.6
#   noep     10.7.0.7  no Endpoint: the server contacts it (172.31.0.30:41649)
#   dns      10.7.0.8  Endpoint by name (wg-server-later, see run-tests.sh)
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p state
docker compose --profile peer build -q peer

if [[ ! -s state/keys.txt ]]; then
  # Plain docker run: the compose service's mounts would create the .conf
  # paths as directories before they exist.
  docker run --rm --entrypoint sh wgnx-e2e-peer -c '
    for n in server peer2 client preload bg psk roam noep dns; do
      k=$(wg genkey); echo "$n $k $(echo "$k" | wg pubkey) $(wg genpsk)"
    done' > state/keys.txt
fi
key() { awk -v n="$1" -v f="$2" '$1==n {print $f}' state/keys.txt; }  # f: 2 private, 3 public, 4 psk
SERVER=172.31.0.20
PEER2=172.31.0.21

client() {  # name address peers...
  local name=$1 addr=$2; shift 2
  {
    echo "[Interface]"
    echo "# Name = $name"
    echo "PrivateKey = $(key "$name" 2)"
    echo "Address = $addr"
    printf '%s\n' "$@"
  } > "state/$name.conf"
}
server_peer() {  # name allowed [extra lines...]
  local name=$1 allowed=$2; shift 2
  printf '\n[Peer]\n# Name = %s\nPublicKey = %s\nAllowedIPs = %s\n' "$name" "$(key "$name" 3)" "$allowed"
  printf '%s\n' "$@"
}
S="[Peer]
# Name = server
PublicKey = $(key server 3)
Endpoint = $SERVER:51820"

client client 10.7.0.2/24 "" "$S" "AllowedIPs = 10.7.0.1/32, 10.123.0.0/24" "" \
  "[Peer]" "# Name = second" "PublicKey = $(key peer2 3)" "AllowedIPs = 10.7.0.9/32" "Endpoint = $PEER2:51820"
client preload 10.7.0.3/24 "" "$S" "AllowedIPs = 10.7.0.1/32, 10.123.0.0/24"
sed 's/, 10.123.0.0\/24//' state/preload.conf > state/nosubnet.conf
client bg 10.7.0.4/24 "" "$S" "AllowedIPs = 10.7.0.1/32" "PersistentKeepalive = 3"
client psk 10.7.0.5/24 "" "$S" "AllowedIPs = 10.7.0.1/32" "PresharedKey = $(key psk 4)"
sed "s|PresharedKey = .*|PresharedKey = $(key roam 4)|" state/psk.conf > state/badpsk.conf
client roam 10.7.0.6/24 "" "$S" "AllowedIPs = 10.7.0.1/32"
client noep 10.7.0.7/24 "ListenPort = 41649" "" "[Peer]" "# Name = server" "PublicKey = $(key server 3)" "AllowedIPs = 10.7.0.1/32"
client dns 10.7.0.8/24 "" "[Peer]" "# Name = server" "PublicKey = $(key server 3)" "AllowedIPs = 10.7.0.1/32" "Endpoint = wg-server-later:51820"
printf '[Interface]\nPrivateKey = not-a-key\nAddress = 10.7.0.99/24\n' > state/bad.conf

{
  printf '[Interface]\nPrivateKey = %s\nAddress = 10.7.0.1/24\nListenPort = 51820\n' "$(key server 2)"
  server_peer client 10.7.0.2/32
  server_peer preload 10.7.0.3/32
  server_peer bg 10.7.0.4/32
  server_peer psk 10.7.0.5/32 "PresharedKey = $(key psk 4)"
  server_peer roam 10.7.0.6/32
  server_peer noep 10.7.0.7/32 "Endpoint = 172.31.0.30:41649" "PersistentKeepalive = 5"
  server_peer dns 10.7.0.8/32
} > state/wg0.conf
{
  printf '[Interface]\nPrivateKey = %s\nAddress = 10.7.0.9/24\nListenPort = 51820\n' "$(key peer2 2)"
  server_peer client 10.7.0.2/32
} > state/wg0-peer2.conf

docker compose --profile peer up -d peer peer2 web iperf
for _ in $(seq 30); do
  docker compose exec -T peer wg show wg0 >/dev/null 2>&1 && break
  sleep 1
done
echo "server: 10.7.0.1 at $SERVER:51820; peer2: 10.7.0.9 at $PEER2:51820"
echo "confs:  e2e/state/{client,preload,bg,psk,roam,noep,dns}.conf"
