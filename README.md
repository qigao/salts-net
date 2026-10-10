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

LB and TCP proxy callbacks run inside their owner's progress operation. Recursive
poll, stop, destroy, or listener mutation returns `SALTS_EBUSY` before changing
admission or shutdown state. Read-only port queries remain available; defer
lifecycle work until the outer call returns. This is an owner-local reentrancy
guard, not synchronization for concurrent callers. The callback regressions
exercise real TCP forwarding. SG hosting has its own explicit API below;
the remaining integration work is tracked in
[#49](https://github.com/qigao/salts-net/issues/49).

Use CFlow only when the application genuinely needs graph composition, demand propagation, structured scheduling, or another CFlow execution surface.

### CNet 2.3 protocol clients

Email (SMTP/POP3/IMAP) and LDAP keep one caller-driven TCP/TLS connection.
Each connect episode owns a CNet 2.3 Manager with one record and one connection
credit. The progress caller initializes it, reserves the observer attachment,
and connects through `cnet_manager_connect`. Admission failure retires the
reservation; a live connection retires only on its real CNet terminal. After
poll returns, manager advance recycles the attachment and destroys the drained
manager before a new episode can reuse protocol state. The client and callback
storage outlive that sequence. Close/destroy failure retains them for cleanup.
No extra thread, backend observe, connection pool or automatic replay is added.

Transport CONNECTED still does not authorize application commands: Email owns
greeting, STARTTLS and configured authentication; LDAP owns Bind results.
SMTP DATA and LDAP mutations are not retried by the transport. SNMP retains its
configured UDP request retransmission and response-ID/security validation; ICE
retains its CNet 2.3 Datagram send tags, receive demand and protocol retry rules.
CNet Manager only accepts TCP/TLS, so applying it to UDP would reject valid
SNMP traffic. This uses the existing Salts::CNet dependency and preserves public
client configuration and signatures. Rollback is internal to the transport;
finish active connections before replacing the library.

Formal regressions cover Email admission rollback and repeated connection-slot
reuse, SMTP/POP3/IMAP exchanges and TLS rejection, LDAP rejected Bind followed
by explicit Unbind/reconnect, and SNMP byte-identical bounded retransmission.
The independent installed C11/C++17 consumers link Email, LDAP and SNMP and
exercise their public lifecycles, including invalid Email authority rejection.

### Native SG TCP proxy

[`salts_tcp_proxy_sg.h`](tproxy/include/salts_tcp_proxy_sg.h) adds opt-in
1/2/4 Owner hosting to `SaltsNet::TCPProxy`. Existing `salts_tcp_proxy_*`
objects remain caller-thread-owned. The SG host owns one Salts
`native_io_sharded` and one external CNet client/manager per Owner; it reuses
the existing SOCKS5, HTTP CONNECT, authentication and forwarding implementation.

The controller calls `salts_tcp_proxy_sg_poll`. Each round dispatches a short
task to every SG Owner and joins all admitted tasks. Each task observes its
leased backend exactly once, routes the entire batch with
`cnet_sg_host_route_batch`, then advances CNet and manager retirement. Idle
polls wait in 1 ms controller intervals up to the requested timeout. This is
explicit caller-driven progress on NativeIO's existing Owner threads; it adds
no Actor, scheduler, backend, or independent progress thread.

Owner 0 owns the ingress listener. A detached accept is placed exactly once
using the CNet owner-placement policy. ROUND_ROBIN and LOWEST_PRESSURE consume
bounded credit snapshots. EXPLICIT and STRICT_KEY do not move a child to a
different Owner when the selected Owner is full. STRICT_KEY's callback runs on
ingress before protocol I/O, so its application key must already be known from
the accepted peer/application context. Selection/reservation errors reject that
child and are returned by poll; all dispatched Owners still finish their round.

Every accepted child reserves a credit in the destination's `cnet_handoff`.
Same-Owner admission directly reserves/adopts a manager attachment and never
enters the handoff queue. Remote admission publishes the detached stream and
ticket; only its final Owner takes/adopts them and begins protocol processing.
The controller always schedules all Owners again, so publication requires no
separate wake task. Successful publish transfers the accepted stream; failure
closes it and releases its reservation. A manager context hold covers both
tunnel sides. Only manager recycle, after real terminals and context retirement,
returns the credit and frees the session slot. A successful send does not retire
a tunnel, and connection failure does not replay DATA or choose another upstream.

Capacities in `salts_tcp_proxy_config_t` apply **per Owner**. Each Owner allocates
`session_capacity` protocol sessions, twice that many physical CNet connections,
and `4 * session_capacity + 1` NativeIO endpoint slots (CNet reserves two endpoint
slots per connection, plus ingress listener space). Its backend request budget
is `request_capacity + 1`; handoff queue capacity is positive and cannot exceed
session capacity. The SG task queue must be a positive power of two. Endpoint
selection counters are local to the final Owner; least-inflight is not a global
cross-Owner balancing metric. No new library dependency is introduced.

For example, initialize an SG host with per-Owner limits:

```c
#include <salts_tcp_proxy_sg.h>

salts_tcp_proxy_config_t proxy = salts_tcp_proxy_config_default();
salts_tcp_proxy_sg_config_t sg = salts_tcp_proxy_sg_config_default();
proxy.protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
proxy.session_capacity = 64;
sg.owner_count = 4;
sg.handoff_queue_capacity = 16;
sg.placement = CNET_OWNER_PLACE_LOWEST_PRESSURE;
salts_tcp_proxy_sg_t *host = NULL;
int status = salts_tcp_proxy_sg_create(&proxy, &sg, &host);
```

On success, call `salts_tcp_proxy_sg_listen(host, address, port)` and drive poll
from one controller. Check every return value; `SALTS_ENOBUFS` admission rejects
one child while retaining the host. Configuration strings/endpoint entries are
copied. Callback user data is borrowed until destroy. Access/route callbacks run
on final Owners and may overlap across Owners: shared mutable callback state
needs application synchronization. `salts_tcp_proxy_sg_current_owner` is the
callback-safe ownership query; other host APIs reject callback reentry with
`SALTS_EBUSY`. Control calls must never overlap, including port/stat queries.

Stop seals admission after the previous round has joined, drains queued
streams, closes sessions/listener, routes cancellation terminals, retires
contexts, releases host leases and shuts down SG. The configured shutdown
timeout is one budget for the whole drain. A failed stop keeps ownership for
retry; it may report an earlier progress error even when the `stopped` snapshot
is true. Destroy requires completed stop. Normally create failure leaves a null
handle; if rollback fails, its nonnull cleanup handle still requires stop/destroy.
Never discard a retained handle or reclaim callback data after failed teardown.

The design chooses explicit short SG rounds over a separate SaltsNet worker loop
to preserve one observe authority and bounded publish/drain ownership. Its cost
is per-round dispatch/join and per-Owner memory replication; no throughput or
latency improvement is claimed without benchmarks. Migration is opt-in through
the new header/API; applications can keep the existing single-Owner API or
return to it after fully draining/destroying an SG host. Rebuild consumers with
matching headers/libraries; live sessions cannot move between these models.

Formal proxy tests cover real 1/2/4 Owner forwarding, callback affinity and
reentry, fragmented/authenticated handshakes, pinned-Owner exhaustion/reuse,
lowest-pressure placement, mixed slow/refused/healthy sessions and drain credits.
The installed C11/C++17 consumers also exercise SG creation and teardown using
only installed public targets. Cross-platform SG execution and broader
protocol/dependency and performance qualification remain open.

### Native SG LB ownership

[`salts_lb_sg.h`](lb/include/salts_lb_sg.h) adds opt-in 1/2/4 Owner LB hosting.
LB SG uses one frontend ingress on Owner 0 and an explicit worker-listener
port per Owner. Applications register workers for each Owner/group they serve;
worker placement is fixed by that port, while detached frontend accepts use the
configured CNet Owner policy once. Workers remain remote business destinations,
not Owner identifiers. Group routing and SESSION/REQUEST pairing stay local to
the final Owner. Missing compatible workers retain the existing bounded wait;
there is no cross-Owner worker borrowing, request replay or automatic relocation.
This is an additive deployment choice; existing single-Owner LB ports and APIs
retain their behavior. Reverting requires draining the SG host and reconnecting
peers to the original single-Owner endpoints.

For example, configure a two-Owner LB with room for both peer roles:

```c
#include <salts_lb_sg.h>

salts_lb_config_t lb = salts_lb_config_default();
salts_lb_sg_config_t sg = salts_lb_sg_config_default();
lb.connection_capacity = 64; /* Combined workers + frontends, per Owner. */
sg.owner_count = 2;
sg.handoff_queue_capacity = 16;
salts_lb_sg_t *host = NULL;
int status = salts_lb_sg_create(&lb, &sg, &host);
```

Check creation, then open `salts_lb_sg_listen` once and
`salts_lb_sg_accept_workers(host, owner, address, port)` for each Owner. Query
the bound frontend/worker ports, connect workers to their intended Owner's port,
and drive `salts_lb_sg_poll` from one controller. All control calls, including
port/stats reads, must not overlap; callback reentry returns `SALTS_EBUSY`.
Use `salts_lb_sg_current_owner` inside route/filter/frame callbacks, and make
shared callback data safe for concurrent final Owners. Worker destination
selection (`worker_policy`) remains separate from frontend Owner placement.
The backend budget is `2 * connection_capacity + 2` endpoints and
`request_capacity + 2` requests per Owner, including both listener allowances.
Check stop/destroy errors and retain the handle for retry until teardown succeeds.

LB and TCP proxy share a private SG host implementation linked into their own
components. It owns NativeIO leases, bounded frontend handoff and controller
rounds; protocol adapters own their existing state machines and CNet managers.
One LB connection, frontend or worker, reserves one destination credit until
manager terminal/context retirement. Worker and frontend credits share the
per-Owner connection bound. Successful handoff transfers the detached stream;
failure closes it and returns its reservation. Stop joins admission producers,
seals queues, drains connections/listeners and releases leases before backend
shutdown. Callback users remain borrowed through completed destruction.

The current SDK batch router accepts one listener. Owner 0 can have both LB
listeners, so the host identifies worker-accept completions by the submitted
generation-safe request identity and routes those through the SDK worker
listener route first. It routes the remaining batch once with the frontend
listener and protocol client, preserving remaining batch order and never observing twice or
rerouting SG-owned completions. Other Owners use the same path with no frontend
listener. Tests must cover concurrent accept completions and cancellation of
both listeners. Sharing this internal host avoids separate proxy/LB shutdown
implementations; it introduces no public generic runtime or new dependency.

LB regression tests cover 1/2 Owner SESSION forwarding, 4 Owner REQUEST reuse
with fragmented registration/responses, callback affinity/reentry, shared
worker/frontend capacity, pinned-Owner rejection/reuse, initialization rollback,
dual-listener cancellation and slow-registration/worker-disconnect isolation.
The formal [`sg_pipeline_test.c`](tests/sg_pipeline_test.c) additionally drives
two independent two-Owner hosts in a SOCKS5 Proxy -> LB -> Worker chain. It
checks healthy forwarding while another worker stalls then disconnects, distinct
runtime Owner identities, no replay and exact terminal/context credit drain.
This does not qualify TLS or every other SaltsNet protocol dependency.

### SG performance baseline

[`benchmark_salts_tcp_proxy_sg.c`](tproxy/tests/benchmark_salts_tcp_proxy_sg.c)
measures four persistent loopback SOCKS5 tunnels on 1/2/4 Owners. Each of 64
samples completes 16 sequential 256-byte echo roundtrips per client (64
operations and 16,384 payload bytes per sample). Payload counts once per
roundtrip, not both wire directions. Creation, handshakes, a verified warm-up
exchange and teardown are outside the timed block. Controller/fixture wake,
SG dispatch/join and payload verification remain inside it. Every received
payload is checked, and terminal credits must drain after the run.

Run through CTest using the same restored SDK/cache environment as the native
CI Release profile, with `QIGAO_SDK_RID`, target/host triplets and
`VCPKG_CACHE_REPOSITORY_ROOT` set. Windows additionally uses `VsDevCmd.bat`
and `QIGAO_VCPKG_ROOT`:

```powershell
cmake --preset ci-win-benchmark-user
cmake --build --preset ci-win-benchmark-user --parallel
ctest --preset ci-win-benchmark-user -V
```

Linux/macOS use `ci-linux-benchmark-user` / `ci-macos-benchmark-user` with
their existing native CI toolchain environment. These profiles select
`BUILD_TESTING=OFF`, `BUILD_BENCHMARKS=ON` and a separate
`build/ci-benchmark/<RID>` tree. Regular tests keep benchmarks disabled;
CTest registers the benchmark under the `benchmark` label. No elapsed-time
threshold gates correctness or ordinary CI.

Local result after sharing the SG host, 2026-10-10: AMD Ryzen 9 7940HX (16 cores / 32 logical
processors), Windows 11 build 26200, MSVC 19.44.35217 Release
(`/O2 /Ob2 /DNDEBUG`), Salts 2.3.0-rc.1 and SaltsUtils 4.3.0-rc.1:

| Owners | Roundtrips/s | Payload MiB/s | Batch min–max (ms) |
| --- | ---: | ---: | ---: |
| 1 | 3,930 | 0.96 | 6.41–32.39 |
| 2 | 4,133 | 1.01 | 10.21–18.00 |
| 4 | 4,229 | 1.03 | 8.45–18.48 |

This is one uncontrolled workstation run with fixed concurrency and per-Owner
session capacity 4. It establishes a reproducible workload, not a demonstrated
scaling improvement. Batch extrema and amortized time per operation are not
individual RTT percentiles. Admission throughput, TLS, saturated queues,
allocation/RSS, P95/P99 and cross-platform results still need separate workloads
and measurement before broader performance claims.

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

The current qualification pair is [Salts 2.3.0-rc.2](https://github.com/qigao/salts/releases/tag/v2.3.0-rc.2)
and [SaltsUtils 4.3.0-rc.2](https://github.com/qigao/salts-utils/releases/tag/v4.3.0-rc.2).
Upgrade or roll back both SDKs together: Unicode is now exported only by Salts,
and SaltsUtils consumes that target. The floating restore policy above remains
unchanged; historical benchmark results retain their actual SDK versions.
macOS builds and installed SDK consumers use AppleClang, matching the rc.2
SDK's native thread-local storage ABI; GCC's emulated TLS is incompatible with
the published TinyTest runtime. Reconfigure an existing macOS CI build tree
with `cmake --fresh --preset ci-macos-release-user` when changing compilers.
Local Windows MSVC Release qualification of this pair passed all 36 project
CTests and both installed SDK C11/C++17 consumer tests. This verifies existing
SaltsNet paths against rc.2; it does not qualify new UDP/WS protocol adapters.

CNet rc.2 adds `cnet_sg_host_route_batch_with_datagrams` for routing UDP and
TCP completions through the same SG Owner, a dedicated TCP/TLS WebSocket
write/terminal bridge, and per-attempt ManagedDial admission hooks. These are
opt-in composition APIs: `cnet_manager_connect` still accepts only TCP/TLS,
while UDP uses the datagram lifecycle. SNMP transaction retries and ICE/STUN/TURN
selection, pacing and readiness remain protocol-owned. Upgrading the SDK alone
does not enable mixed SG hosting in those consumers or implement HTTP/WS
handshakes. Integration and protocol acceptance remain tracked in
[SaltsNet #50](https://github.com/qigao/salts-net/issues/50) and
[Salts #1095](https://github.com/qigao/salts/issues/1095).

The first internal ICE transport slice now separates bounded send/receive
admission and result consumption from waiting. The existing synchronous path
uses the same state; an explicit external initializer borrows the host backend.
The host exclusively observes and routes completions with the rc.2 mixed
router. Each endpoint owns one copied receive slot and at most one in-flight
send; short output buffers retain the received packet. Send results use the
admitted tag and mean local transport completion, never protocol success.
Stop closes admission and retains the endpoint, tag and callback storage until
all actual terminals have been routed. External destroy returns busy while
draining, and synchronous send/receive reject external mode before admission.

The formal `ice_cnet_sg` test uses this internal adapter on 1/2/4 real SG Owners
with a TCP neighbor on each backend. It exercises a fixed-peer STUN exchange,
transaction-ID rejection, copied payload ownership, full send admission,
retained receive data, cancellation and continued TCP/UDP progress after one
UDP endpoint stops. This is a transport composition slice: public ICE/STUN/TURN
and SNMP calls still use their existing synchronous mode. Nonblocking protocol
timers/retransmissions, shared application hosting, public async APIs and
installed-consumer coverage for those APIs remain open in #50. The adapter is
private and is not installed. This keeps one transport state machine instead
of duplicating it or blocking the shared Owner inside a synchronous facade;
rollback drains external borrowers before selecting the existing owned mode.
Local Windows Release validation of this slice passed all 37 CTests, ten
consecutive `ice_cnet_sg` runs (each covering 1/2/4 Owners), and the two existing
installed SDK consumer tests. The latter verify the unchanged public APIs,
not the private external adapter.

The versioned `CMakeUserPresets.json` owns local and CI entry points. Shared
presets retain compiler and platform settings. Following
[SaltsUtils 4.3 prerelease](https://github.com/qigao/salts-utils/releases/tag/v4.3.0-rc.2), vcpkg runs in manifest mode
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
the old unused benchmark switch were removed during migration; the current
`BUILD_BENCHMARKS` option now controls the SG benchmark above. SDK installation
is a normal build/install step. The current formal installed consumers live in
`tests/installed_sdk`: CI installs into `stage/sdk/<RID>`, then
`cmake/ci/verify-installed-sdk.ps1` uses their versioned user presets and empty
vcpkg manifest to configure/build/run C11 and C++17 tests in a separate tree.
It inherits the shared cache toolchain and package roots; it imports SaltsNet
strictly from that fresh install, without source-tree targets or build-tree DLLs.

The prerequisite bump changes build inputs and orchestration. The native SG
proxy and LB integrations above add opt-in public APIs; further CNet 2.3
integration in other protocol consumers remains a separate milestone.
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
