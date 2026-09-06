# `stun_service`

`stun_service` is the minimal UDP STUN binding service built from the same
SaltsNet ICE module used by mesh ICE gathering and checks. The service owns a
bounded CNet datagram endpoint and advances it on the main thread.

## Purpose

- validate real STUN behavior without relying only on third-party servers
- provide a self-hosted STUN endpoint for mesh ICE/srflx testing
- keep the service surface small and tied to the repo-native STUN parser/builder

## Build

Standalone build:

```sh
cd ice
cmake -S . -B build-standalone \
  -DCMAKE_PREFIX_PATH="/opt/saltsnet;/opt/salts" \
  -DSaltsNet_DIR=/opt/saltsnet/lib/cmake/SaltsNet
cmake --build build-standalone --parallel --target test_stun stun_discovery stun_service
```

## Run

```sh
LD_LIBRARY_PATH=/opt/saltsnet/lib ./stun_service 0.0.0.0 3479
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

- `/opt/saltsnet/bin/stun_service`

Suggested systemd unit:

- `ice/examples/stun_service.service`
