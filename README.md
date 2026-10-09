# SaltsNet

**Protocol and network tooling built on the Salts C11 systems foundation.**

SaltsNet extends [Salts](https://github.com/qigao/salts) with reusable networking and protocol components while preserving the same explicit ownership, bounded progress, lifecycle, and error semantics.

Transport/session primitives come from `Salts::CNet`; protocol type metadata comes from `Salts::CMeta`. SaltsNet does not create a second hidden networking runtime, event loop, or compatibility layer.

**Tags:** C11 · networking · protocols · ICE · STUN · TURN · SNMP · LDAP · SMTP · IMAP · proxy

## Built on Salts

SaltsNet inherits the following foundations instead of reimplementing them:

- **CNet** for transport/session ownership, TLS, polling, connection progress, and deterministic shutdown.
- **CMeta** for protocol enums, descriptors, and shared typed metadata.
- **NativeIO / Platform** for native asynchronous I/O and platform abstraction.
- **CFlow** when a caller explicitly needs dataflow, demand propagation, scheduling, or higher-level composition.

The default model stays caller-driven: the owner that creates a networking component is responsible for progressing the underlying CNet runtime and for completing deterministic shutdown.

## Role in the ecosystem

```text
Salts
  ├── salts-utils        DataBind, utilities, parsers, QueryVM, crypto, FS/process
  └── salts-net          protocol/network extensions

salts-net
  └── feeds higher-level transports and adapters used by CHTTP,
      TurboFlow, Flowie, and other ecosystem projects
```

SaltsNet is the **network extension layer**. It is intentionally separate from HTTP application services, storage, workflow engines, and product-specific session state.

## Modules

| CMake target | Responsibility |
| --- | --- |
| `SaltsNet::ICE` | STUN, TURN, and ICE |
| `SaltsNet::SNMP` | SNMP client and protocol codec |
| `SaltsNet::LDAP` | LDAP client |
| `SaltsNet::Email` | SMTP, POP3, and IMAP clients |
| `SaltsNet::MimeParser` | MIME parsing |
| `SaltsNet::UriParser` | URI parsing component |
| `SaltsNet::LB` | CNet connection load balancing |
| `SaltsNet::TCPProxy` | TCP/TLS proxying |

The URI parser is a concrete package component, but it is not a top-level ecosystem layer or architectural pillar.

## Runtime model

SaltsNet keeps CNet's explicit caller-driven ownership model:

- the creating thread/runtime owner progresses `cnet_poll()`;
- shutdown is explicit and deterministic;
- request/admission state is bounded;
- no secondary event loop is created behind the caller's back;
- protocol metadata reuses CMeta instead of defining a parallel reflection/type system.

Use CFlow only when the application genuinely needs graph composition, demand propagation, structured scheduling, or another CFlow execution surface.

## Build and test

The native build and packaging qualification declare floating **prerelease**
ranges using `Salts.Native Version="2.3.0-*"` and
`SaltsUtils.Native Version="4.3.0-*"`. Because the NuGet feed also contains
SHA-qualified Linux-only verification snapshots (which can sort *above*
`rc.1` or `rc.2`), the CI restore dynamically selects the newest **official numeric RC tag**, or
the **stable release** when available, from GitHub Releases. It
passes these selected identities into the floating MSBuild projects only for
qualification, so no fixed RC number is committed. The restore uses
`--no-cache --force-evaluate`, selects SDK roots from NuGet's actual
`project.assets.json`, verifies the chosen release identities, and
**fails fast** if the requested prerelease
or target/host SDK is unavailable. It does not fall back to Salts 2.2 or
SaltsUtils 4.2. Rebuild all linked native dependencies against the same
ABI-qualified candidate; do not mix prerelease and old stable binaries.

SaltsNet currently links directly to Salts/CNet, while SaltsUtils is
resolved and validated as an explicit prerequisite for the shared 2.3/4.3
SDK qualification. Protocol-layer business logic remains in SaltsNet.

The versioned `CMakeUserPresets.json` owns local and CI entry points. Shared
presets retain compiler and platform settings. Following
[SaltsUtils 4.3 prerelease](https://github.com/qigao/salts-utils/releases/tag/v4.3.0-rc.1), vcpkg runs in manifest mode
through the shared `qigao/vcpkg-cache` toolchain, with a read-only GitHub feed
and a writable local cache. The existing vcpkg baseline is retained. SaltsNet
uses the [GmSSL-backed crypto provider in Salts](https://github.com/qigao/salts)
through its public CMeta APIs; it has no direct OpenSSL/BoringSSL dependency.
Runtime libraries are resolved through the selected preset's environment.

Prerequisites: PowerShell 7, .NET SDK 8, CMake 3.25 or newer, Ninja, a C/C++ toolchain, vcpkg,
and the [shared cache checkout](https://github.com/qigao/vcpkg-cache).
Set `PROJECT_ROOT` (the parent of `external/pkgs`), `VCPKG_ROOT`, and
`GITHUB_TOKEN` with `read:packages`. Windows uses the shared cache at
`%LOCALAPPDATA%/qigao/vcpkg-cache`; Linux reads `VCPKG_CACHE_REPOSITORY_ROOT`
from the environment and requires Mono for the NuGet binary cache client.

Windows Release example, from PowerShell in an x64 `VsDevCmd.bat` environment:

```powershell
./cmake/ci/restore-native-sdk.ps1 -SaltsRid windows-x64 -HostRid windows-x64 -Local
cmake --preset win-release-user
cmake --build --preset win-release-user --parallel
ctest --preset win-release-user
```

The restore command obtains `Qigao.Re2c.Binary` from the GitHub Packages feed
in [cmake/vcpkg-cache.nuget.config](cmake/vcpkg-cache.nuget.config), also using
`Version="*"`. It sets `SALTS_ROOT`, `SALTS_UTILS_ROOT`, their resolved versions and host
SDK roots, plus `RE2C_ROOT` and `RE2C_VERSION`, in the current
PowerShell session. `RE2C_ROOT` selects `tools/<host RID>` from the restored
package, including when cross-compiling Android. CMake resolves re2c only from
that root, and lexer generation depends on that executable. The Docker build
base does not install a distribution re2c; derived build environments must
provide the restored package root as well.

Linux uses the same script with `linux-x64` for both RIDs, then
`linux-release-user` for configure, build, and test. Debug/ASan builds use
`win-dev-user` or `linux-dev-user` and require a matching Debug Salts SDK supplied
through `SALTS_ROOT` and `SALTS_UTILS_ROOT`; the prerelease Release SDKs
are not Debug SDKs.

Install SaltsNet:

```powershell
cmake --build --preset install-win-release-user --parallel
```

CI uses `ci-linux-release-user`, `ci-macos-release-user`, and
`ci-win-release-user`. Each inherits the corresponding shared compiler profile:
GCC on Linux, Homebrew GCC 15 on macOS, and MSVC with UTF-8 on Windows.
The vcpkg setup action uses the same pinned tool bootstrap as current Salts and
SaltsUtils; native package restoration re-evaluates the two official release channels
(RC or stable) on every run. CI uses platform-separated ccache objects (including
MSVC), read-only shared vcpkg binary caching, and cached NuGet package payloads.
Android arm64 uses `ci-android-sdk-release-user` after building host
tools. Its `LEMON_EXECUTABLE` must point to that completed host build; the
target toolchain never produces or searches for an executable to run on the host.
The iOS device build uses `ci-ios-sdk-release-user` after the macOS host
build. Native Linux arm64 executes `ci-linux-arm64-release-user` and host
CTest on an ARM64 runner. Both Salts 2.3 and SaltsUtils 4.3 published
prereleases ship `linux-arm64`, but **not** `ios-simulator-arm64`; the
six-RID matrix follows the actual published SDK set. Host jobs execute
CTest; Android and iOS are compiled and linked only. SDK staging remains
`stage/sdk/<RID>`, and master releases retain CI-owned immutable tags and package publication.

`BUILD_TESTING` controls all test targets, and `BUILD_EXAMPLES` controls all
examples, including the email clients. Email and MIME test directories own their
explicit test target names and sources. The old package consumer harness and
the unused `BUILD_BENCHMARKS` option have been removed; SDK installation is a
normal build/install step.

The current prerequisite bump changes build inputs and orchestration, not
SaltsNet's public API. A separate integration milestone will adopt CNet 2.3
Server/Client strategies in LB, TCP proxy, and protocol consumers as needed.
Reconfigure and rebuild consumers together with the selected SDK to avoid
mixing headers and runtime versions. Local restoration stays under ignored
`stage/nuget` and does not overwrite installed SDKs. To undo the build migration,
revert its configuration changes and rebuild with the previous matching SDK;
automatic fallback to an older package is intentionally unsupported.

Android device testing and LLDB notes are documented in [tools/android-test.md](tools/android-test.md).

## Using SaltsNet from CMake

SaltsNet's package config resolves the installed Salts SDK from `SALTS_ROOT`. Consumers link only the component targets they use; Salts dependencies propagate through those CMake targets.

```cmake
find_package(SaltsNet CONFIG REQUIRED)

add_executable(net_tool main.c)
target_link_libraries(net_tool PRIVATE
  SaltsNet::TCPProxy
  SaltsNet::SNMP)
```

When configuring a consumer, add the SaltsNet install prefix to `CMAKE_PREFIX_PATH` and keep `SALTS_ROOT` pointed at the matching Salts profile.

```powershell
$env:SALTS_ROOT = 'C:/projects/cpp/external/pkgs/salts/release'
cmake -S . -B build `
  -DCMAKE_PREFIX_PATH='C:/projects/cpp/external/pkgs/saltsnet/release'
cmake --build build
```

## Migration from TurboNet / CoroNet

The repository no longer provides CoroNet or `TurboNet::*` compatibility aliases.

Migration rules:

- replace `TurboNet::*` link targets with the corresponding `SaltsNet::*` targets;
- move CoroNet socket/context/coroutine use to Salts CNet's explicit client/listener/session handles and `cnet_poll()`;
- keep one runtime owner;
- preserve CNet's bounded admission, cancellation, drain, and shutdown semantics;
- use CMeta for protocol enums and type descriptions;
- introduce CFlow only at call sites that actually require graph-style orchestration.

There is no silent fallback to the old naming or runtime model. Residual legacy references should fail during configuration or compilation.

## Design rules

- Build on Salts primitives instead of wrapping them in another opaque runtime.
- Keep networking ownership and shutdown explicit.
- Keep protocol tooling separate from product/session/control-plane semantics.
- Preserve bounded request state and deterministic failure behavior.
- Reuse CMeta semantics across protocol boundaries.
- Keep dependency direction one-way: `SaltsNet -> Salts`.

---

**Salts provides the network/runtime semantics. SaltsNet turns them into reusable protocol tools.**


GitHub Packages policy: consumers must restore `Salts.Native` explicitly as latest; `SaltsNet.Native` does not embed versioned dependency metadata.
