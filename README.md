# SaltsNet

**Protocol and network tooling built on the Salts C11 systems foundation.**

SaltsNet extends [Salts](https://github.com/qigao/salts) with reusable networking and protocol components while preserving the same explicit ownership, bounded progress, lifecycle, and error semantics.

Transport/session primitives come from `Salts::CNet`; protocol type metadata comes from `Salts::CMeta`. SaltsNet does not create a second hidden networking runtime, event loop, or compatibility layer.

**Tags:** C11 · networking · protocols · ICE · STUN · TURN · SNMP · LDAP · SMTP · IMAP · proxy · QUIC

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
| `SaltsNet::LSQUIC` | Optional CNet/LSQUIC adaptation |

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

Install a Salts SDK that matches the selected build profile and set `SALTS_ROOT`.

Windows Release example:

```powershell
$env:SALTS_ROOT = 'C:/projects/cpp/external/pkgs/salts/release'
cmake --preset win-release-user
cmake --build --preset win-release-user --parallel
ctest --preset win-release-user
```

Validate the installed package boundary:

```powershell
ctest --preset win-release-user -L package
```

Install SaltsNet:

```powershell
cmake --build --preset install-win-release-user --parallel
```

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
