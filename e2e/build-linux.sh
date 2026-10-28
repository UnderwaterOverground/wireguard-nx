#!/usr/bin/env bash
# Builds the Linux artifacts used by e2e tests (runs inside the rust:1 image):
# wgnx-host, the LD_PRELOAD shim and the vsock test client.
set -euo pipefail
cd /src
export CARGO_TARGET_DIR=/src/target/linux
cargo build -q -p wgnx-host -p wgnx-ffi ${CARGO_PROFILE_FLAG:-} ${WGNX_FFI_FEATURES:+--features wgnx-ffi/$WGNX_FFI_FEATURES}
out=/src/target/linux
inc="-Icrates/wgnx-ffi/include -Inative/driver -Inative/runtime -Inative/vsock"
cc -c -fPIC -O1 -g $inc native/driver/wgnx_driver.c -o $out/wgnx_driver.o
cc -c -fPIC -O1 -g $inc native/driver/wgnx_dns.c -o $out/wgnx_dns.o
cc -c -fPIC -O1 -g $inc native/driver/wgnx_drop.c -o $out/wgnx_drop.o
cc -c -fPIC -O1 -g -DWGNX_BUILD_UNIX=$(date +%s) $inc native/driver/wgnx_app.c -o $out/wgnx_app.o
c++ -std=c++17 -shared -fPIC -O1 -g -Wall $inc -o $out/libwgnx_interpose.so \
  native/vsock/vsock.cpp native/runtime/runtime.cpp native/interpose/interpose.cpp \
  $out/wgnx_driver.o $out/wgnx_app.o $out/wgnx_dns.o $out/wgnx_drop.o $out/${RUST_PROFILE_DIR:-debug}/libwgnx_ffi.a -lpthread -ldl -lm
cc -O1 -g -Wall -o $out/vsock-test native/interpose/vsock_test.c
cc -O1 -g -Wall -o $out/heap-probe native/interpose/heap_probe.c -ldl
# A fixed path for the compose service, whichever profile was built.
mkdir -p $out/bin && cp $out/${RUST_PROFILE_DIR:-debug}/wgnx-host $out/bin/wgnx-host
echo "built ($RUST_PROFILE_DIR): $out/bin/wgnx-host, $out/libwgnx_interpose.so, $out/vsock-test"
