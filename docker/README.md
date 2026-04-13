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
- configure or build TurboNet
- install TurboNet into `/usr/local`
- export project-specific preset variables such as `VCPKG_ROOT`

## Build The Base Image

```bash
docker build -t turbonet-build-base:bookworm .
```

## Build A Derived Project Image

Use `docker/Dockerfile.build-example` as the starting point.

Example:

```bash
docker build \
  -f docker/Dockerfile.build-example \
  --build-arg TURBONET_BUILD_BASE=turbonet-build-base:bookworm \
  -t turbonet-project-build .
```

## Compose Variant

The repo also ships `docker-compose.yml`.

Build the base image first:

```bash
docker compose build build-base
```

Then build the derived project image:

```bash
docker compose build project-build
```

This keeps the expensive toolchain layer in `build-base`, while `project-build`
reuses it through `FROM turbonet-build-base:bookworm`.

To build an SDK image that installs TurboNet for downstream consumers:

```bash
docker compose build turbonet-sdk
```

## Typical Derived Steps

1. `FROM` the base image
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

## TurboNet SDK For Downstream Projects

Use `docker/Dockerfile.sdk` when another project, such as `mqtt`, should
consume TurboNet via its installed CMake package.

The SDK image installs TurboNet into `/opt/turbonet`.

Example downstream Dockerfile:

```dockerfile
ARG TURBONET_SDK_IMAGE=turbonet-sdk:bookworm

FROM ${TURBONET_SDK_IMAGE} AS mqtt-build

WORKDIR /src/mqtt
COPY . ./

RUN cmake -S . -B build -G Ninja \
    -D CMAKE_PREFIX_PATH=/opt/turbonet \
    && cmake --build build

FROM debian:bookworm AS mqtt-runtime
COPY --from=mqtt-build /src/mqtt/build/bin/mqtt /app/mqtt
COPY --from=mqtt-build /opt/turbonet/lib /opt/turbonet/lib
ENV LD_LIBRARY_PATH=/opt/turbonet/lib
CMD ["/app/mqtt"]
```

Example downstream CMake:

```cmake
find_package(TurboNet CONFIG REQUIRED)
target_link_libraries(mqtt PRIVATE TurboNet::CoroNet)
```

The repo also ships `docker/Dockerfile.mqtt-example` as a copyable downstream template.

## Recommended Build Chain

1. Build the toolchain base:

```bash
docker compose build build-base
```

2. Build the TurboNet SDK image:

```bash
docker compose build turbonet-sdk
```

3. In the downstream `mqtt` project, build against the installed TurboNet SDK:

```bash
docker build \
  -f Dockerfile \
  --build-arg TURBONET_SDK_IMAGE=turbonet-sdk:bookworm \
  -t mqtt-build .
```

4. Run the final `mqtt-runtime` image, which contains the built `mqtt` binary
   and required TurboNet shared libraries, but no compiler, CMake, or `vcpkg`.
