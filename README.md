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

Restore the latest stable released **Salts** SDK before configuring. The restore
step uses `Version="*"` with `--no-cache --force-evaluate`, and selects package
paths from NuGet's resolved assets. Salts 2.1.0 is the upgrade target; it is not
pinned in the build or package metadata. Salts 2.0 is the minimum supported
version because the buffer, SIMD scan, clock, thread, and random APIs now use
their `cmeta_*` names.

SaltsNet intentionally does **not** depend on SaltsUtils; keeping `SaltsNet -> Salts` one-way avoids an unnecessary utility-layer dependency.

The versioned `CMakeUserPresets.json` owns local and CI entry points. Shared
presets retain compiler and platform settings. Following
[SaltsUtils 4.2](https://github.com/qigao/salts-utils/releases/tag/v4.2.0), vcpkg runs in manifest mode
through the shared `qigao/vcpkg-cache` toolchain, with a read-only GitHub feed
and a writable local cache. The existing vcpkg baseline is retained. SaltsNet
uses the [GmSSL-backed crypto provider in Salts 2.1](https://github.com/qigao/salts/blob/v2.1.0/utils/CMakeLists.txt)
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
`Version="*"`. It sets `SALTS_ROOT`, `RE2C_ROOT`, and `RE2C_VERSION` in the current
PowerShell session. `RE2C_ROOT` selects `tools/<host RID>` from the restored
package, including when cross-compiling Android. CMake resolves re2c only from
that root, and lexer generation depends on that executable. The Docker build
base does not install a distribution re2c; derived build environments must
provide the restored package root as well.

Linux uses the same script with `linux-x64` for both RIDs, then
`linux-release-user` for configure, build, and test. Debug/ASan builds use
`win-dev-user` or `linux-dev-user` and require a matching Debug Salts SDK supplied
through `SALTS_ROOT`; the published Release SDK is not a Debug SDK.

Install SaltsNet:

```powershell
cmake --build --preset install-win-release-user --parallel
```

CI uses `ci-linux-release-user`, `ci-macos-release-user`, and
`ci-win-release-user`. Each inherits the corresponding shared compiler profile:
GCC on Linux, Homebrew GCC 15 on macOS, and MSVC with UTF-8 on Windows.
The vcpkg setup action uses the same pinned tool bootstrap as current Salts and
SaltsUtils; native package restoration still resolves the latest release each run.
Android arm64 uses `ci-android-sdk-release-user` after building host
tools. Its `LEMON_EXECUTABLE` must point to that completed host build; the
target toolchain never produces or searches for an executable to run on the host.
The iOS device and simulator builds use `ci-ios-sdk-release-user` after the
macOS host build. Host jobs run the formal CTest suites directly. Android and
iOS are compiled and linked only; device execution is separate. SDK staging remains
`stage/sdk/<RID>`, and master releases retain CI-owned immutable tags and package publication.

`BUILD_TESTING` controls all test targets, and `BUILD_EXAMPLES` controls all
examples, including the email clients. Email and MIME test directories own their
explicit test target names and sources. The old package consumer harness and
the unused `BUILD_BENCHMARKS` option have been removed; SDK installation is a
normal build/install step.

The upgrade changes build inputs and orchestration, not SaltsNet's public API.
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
