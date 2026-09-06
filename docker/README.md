# Docker Build Base

`Dockerfile` at the repo root is now a reusable Linux build base.

It installs:

- Debian Bookworm build essentials
- Ninja
- CMake
- `vcpkg`
- common native dependencies used by this repo

For convenience, the image installs `vcpkg` under `/opt/vcpkg` and symlinks the
binary into `/usr/local/bin/vcpkg`.

It does **not**:

- copy this repository into the image
- run `vcpkg install` for a specific manifest
- configure or build SaltsNet
- install SaltsNet into `/usr/local`
- export project-specific preset variables such as `VCPKG_ROOT`

## Build The Base Image

```bash
docker build -t saltsnet-build-base:bookworm .
```

## Build A Derived Project Image

Use `docker/Dockerfile.build-example` as the starting point.

Example:

```bash
docker build \
  -f docker/Dockerfile.build-example \
  --build-arg SALTS_SDK_IMAGE=salts-sdk:bookworm \
  -t saltsnet-project-build .
```

Both derived Dockerfiles require a compatible `salts-sdk:bookworm` image with
Salts installed at `/opt/salts/release`. Override `SALTS_SDK_IMAGE` when the
image uses another repository or tag. They fail before configuration when the
required `SaltsConfig.cmake` package is absent.

## Typical Derived Steps

1. `FROM` a Salts SDK image
2. `COPY vcpkg.json` and run `vcpkg install`
3. `COPY . .`
4. run `cmake --preset linux-dev-user` or `linux-release-user`
5. run `cmake --build --preset ...`
6. optionally run `ctest --preset ...`

If a project relies on `CMakeUserPresets.json` for paths such as `VCPKG_ROOT`,
set those paths there. The base image should not own that policy.

## Why This Split Exists

The expensive and stable layers belong in the base image:

- compiler toolchain
- CMake
- Ninja
- `vcpkg`
- system packages

Project-specific layers belong in derived images:

- source tree
- manifest dependencies
- configure/build/test/install steps

## SaltsNet SDK For Downstream Projects

Use `docker/Dockerfile.sdk` when another project, such as `mqtt`, should
consume SaltsNet via its installed CMake package.

The SDK image installs SaltsNet into `/opt/saltsnet/release`.

Example downstream Dockerfile:

```dockerfile
ARG SALTSNET_SDK_IMAGE=saltsnet-sdk:bookworm

FROM ${SALTSNET_SDK_IMAGE} AS mqtt-build

WORKDIR /src/mqtt
COPY . ./

RUN cmake --preset linux-release-user \
    && cmake --build --preset linux-release-user

FROM debian:bookworm AS mqtt-runtime
COPY --from=mqtt-build /src/mqtt/build/bin/mqtt /app/mqtt
COPY --from=mqtt-build /opt/saltsnet/release/lib /opt/saltsnet/release/lib
ENV LD_LIBRARY_PATH=/opt/saltsnet/release/lib
CMD ["/app/mqtt"]
```

Example downstream CMake:

```cmake
find_package(SaltsNet CONFIG REQUIRED)
target_link_libraries(mqtt PRIVATE SaltsNet::SNMP)
```

The repo also ships `docker/Dockerfile.mqtt-example` as a copyable downstream template.

## Recommended Build Chain

1. Build the toolchain base, then use it in the Salts repository to produce a
   `salts-sdk:bookworm` image containing `/opt/salts/release`:

```bash
docker build -t saltsnet-build-base:bookworm .
```

2. Build the SaltsNet SDK image from that Salts SDK:

```bash
docker build \
  -f docker/Dockerfile.sdk \
  --build-arg SALTS_SDK_IMAGE=salts-sdk:bookworm \
  -t saltsnet-sdk:bookworm .
```

3. In the downstream `mqtt` project, build against the installed SaltsNet SDK:

```bash
docker build \
  -f Dockerfile \
  --build-arg SALTSNET_SDK_IMAGE=saltsnet-sdk:bookworm \
  -t mqtt-build .
```

4. Run the final `mqtt-runtime` image, which contains the built `mqtt` binary
   and required SaltsNet shared libraries, but no compiler, CMake, or `vcpkg`.
