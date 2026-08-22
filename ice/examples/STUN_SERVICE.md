# `stun_service`

`stun_service` is the minimal UDP STUN binding service built from the same
`turbo_webrtc/ice` module used by mesh ICE gathering and checks.

## Purpose

- validate real STUN behavior without relying only on third-party servers
- provide a self-hosted STUN endpoint for mesh ICE/srflx testing
- keep the service surface small and tied to the repo-native STUN parser/builder

## Build

Standalone build:

```sh
cd ice
cmake -S . -B build-standalone \
  -DCMAKE_PREFIX_PATH="/opt/turbonet;/opt/vcpkg/packages/libuv_x64-linux;/opt/vcpkg/packages/kcp_x64-linux;/opt/vcpkg/packages/quickjs-ng_x64-linux;/opt/vcpkg/packages/c-ares_x64-linux;/opt/vcpkg/packages/cjson_x64-linux;/opt/vcpkg/packages/aklomp-base64_x64-linux;/opt/vcpkg/packages/stb_x64-linux;/opt/vcpkg/packages/zstd_x64-linux;/opt/vcpkg/packages/openssl_x64-linux" \
  -DTurboNet_DIR=/opt/turbonet/lib/cmake/TurboNet
cmake --build build-standalone --parallel --target test_stun stun_discovery stun_service
```

## Run

```sh
LD_LIBRARY_PATH=/opt/turbonet/lib ./stun_service 0.0.0.0 3479
```

Arguments:

- first arg: bind host, default `0.0.0.0`
- second arg: bind port, default `3478`

## Validate

Public STUN:

```sh
./stun_discovery stun.cloudflare.com 3478
```

Self-hosted STUN:

```sh
./stun_discovery 161.97.65.129 3479
```

## EU Deployment

Current EU endpoint:

- `161.97.65.129:3479`

Installed binary:

- `/opt/turbonet/bin/stun_service`

Suggested systemd unit:

- [stun_service.service](C:/projects/cpp/turbonet/turbonet/ice/examples/stun_service.service)
