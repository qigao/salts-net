# SaltsNet CNet Foundation and SNMP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish the SaltsNet package foundation and replace the SNMP client's CoroNet UDP path with a bounded caller-driven CNet owner.

**Architecture:** The build resolves the installed Salts SDK only from `SALTS_ROOT` and exports `SaltsNet::*`. The SNMP client privately owns one `cnet_client` and one connected UDP handle; its synchronous operations drive that owner until a validated response or deadline without creating threads.

**Tech Stack:** C11, CMake Presets, Salts CNet/Core/TinyTest, OpenSSL, TinyTest

**Spec:** `docs/superpowers/specs/2026-09-05-saltsnet-cnet-migration-design.md`

## Global Constraints

- CNet callbacks execute only from the non-overlapping caller-owned poll lane.
- Receive views are borrowed only during the callback.
- Every CNet capacity is positive and bounded.
- Stop precedes destroy and terminal storage is not released early.
- No fallback to CoroNet or native blocking sockets is allowed in production code.
- The package has no `TurboNet::*` compatibility aliases.

---

### Task 1: SaltsNet package and dependency foundation

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `CMakeOptions.cmake`
- Modify: `CMakeUserPresets.json`
- Modify: `cmake/CmakeUtils.cmake`
- Create: `cmake/SaltsNetConfig.cmake.in`
- Delete: `cmake/TurboNetConfig.cmake.in`
- Modify: every first-party `CMakeLists.txt` that names `Rocida::*`, `TurboNet::*`, or `TurboNetTargets`

**Interfaces:**
- Consumes: installed `SaltsConfig.cmake` from `$ENV{SALTS_ROOT}`
- Produces: package `SaltsNet`, export set `SaltsNetTargets`, namespace `SaltsNet::*`

- [x] **Step 1: Add exact Salts roots to public presets**

Add `SALTS_ROOT=$env{PKG_ROOT}/salts/<profile>` to each host profile and the matching
`salts-android/<profile>` root to Android dependency profiles. Add its `bin`/`lib` directory to the
profile runtime path after the current build output.

- [x] **Step 2: Replace the package identity and dependency boundary**

Set `project(SaltsNet ...)`, validate `SALTS_ROOT`, call:

```cmake
find_package(Salts CONFIG REQUIRED PATHS "$ENV{SALTS_ROOT}" NO_DEFAULT_PATH)
```

Generate/install `SaltsNetConfig.cmake`, `SaltsNetConfigVersion.cmake`, and `SaltsNetTargets.cmake`
under `lib/cmake/SaltsNet` with namespace `SaltsNet::`.

- [x] **Step 3: Replace first-party CMake target names**

Use `Salts::Core`, `Salts::CSTL`, and `Salts::TinyTest` in place of Rocida targets. Rename local
aliases and exports from `TurboNet::*`/`TurboNetTargets` to `SaltsNet::*`/`SaltsNetTargets`.

- [x] **Step 4: Verify preset parsing and configure**

Run:

```powershell
cmake --list-presets
cmake --build --list-presets
ctest --list-presets
cmd /c "call ""<VS>\Common7\Tools\VsDevCmd.bat"" -arch=x64 -host_arch=x64 >nul && cmake --fresh --preset win-release-user"
```

Expected: all public presets list successfully and Release configure resolves Salts only from
`SALTS_ROOT`.

### Task 2: Failing SNMP CNet loopback behavior

**Files:**
- Create: `snmp/test/test_snmp_cnet.c`
- Modify: `snmp/test/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `snmp_client_create`, `snmp_client_get`, Salts platform threads
- Produces: regression proof that SNMP creation no longer requires a coroutine context

- [x] **Step 1: Write a real loopback UDP agent test**

Create a numeric loopback UDP socket in a Salts test thread. Receive one GetRequest and return a
fixed valid GetResponse with request id `1`. On the test owner thread call:

```c
snmp_client_config_t config = {
    .host = "127.0.0.1",
    .port = agent.port,
    .community = "public",
    .version = SNMP_VERSION_2C,
    .timeout_ms = 1000u,
    .retries = 0u,
    .recv_buffer_size = 8192u
};
snmp_client_t *client = snmp_client_create(&config);
check_not_null(client);
check_equal(snmp_client_get(client, &oid, 1u, &response), SNMP_CLIENT_OK);
check_equal(response.pdu.request_id, 1);
```

- [x] **Step 2: Run the focused test and observe RED**

Run the `test_snmp_cnet` target and executable. Expected: failure because current
`snmp_client_create()` requires `coro_context_current()`.

### Task 3: Bounded CNet SNMP transport

**Files:**
- Modify: `snmp/src/snmp_client.c`
- Modify: `snmp/CMakeLists.txt`
- Test: `snmp/test/test_snmp_cnet.c`

**Interfaces:**
- Consumes: `cnet_client_init`, `cnet_connect`, `cnet_send`, `cnet_receive`, `cnet_client_poll`, `cnet_client_stop`, `cnet_client_destroy`
- Produces: existing synchronous SNMP client operations backed solely by CNet

- [x] **Step 1: Replace coroutine-owned fields with CNet owner state**

Store one `cnet_client`, `cnet_connection`, terminal/connect/receive flags, copied response bytes,
and the first CNet status. Use platform-selected IOCP/epoll/kqueue and positive fixed capacities.

- [x] **Step 2: Implement CNet callbacks**

`on_state` records connected or terminal failure. `on_receive` validates datagram kind and copies at
most `recv_buffer_size` bytes into client-owned storage before returning. `on_send` records terminal
send completion only.

- [x] **Step 3: Connect and poll during construction**

Build `udp://<host>:<port>` with checked formatting, admit `cnet_connect`, and poll to connected or
the configured deadline. Any failure performs stop/destroy before releasing client memory.

- [x] **Step 4: Drive one request transaction**

For each attempt, reset operation state, request one receive value, admit one copied send, and poll
until a response, terminal failure, or absolute deadline. Parse the copied bytes after the callback
returns. Ignore malformed or wrong-request-id datagrams within the same attempt; retry only after the
deadline.

- [x] **Step 5: Implement ordered shutdown**

Close the live handle when needed, call `cnet_client_stop`, then `cnet_client_destroy`, and finally
release SNMP-owned buffers and configuration strings.

- [x] **Step 6: Run focused and adjacent tests**

Build/run `test_snmp_cnet`, `test_snmp`, `test_snmp_v3`, `test_asn1_roundtrip`, and
`test_context_specific`. Expected: all pass without a CoroNet link dependency from `snmp_parser`.

### Task 4: Package and dependency verification

**Files:**
- Modify: `docs/superpowers/plans/2026-09-05-saltsnet-cnet-foundation-snmp.md`

**Interfaces:**
- Consumes: completed Tasks 1-3
- Produces: reproducible first-stage migration evidence

- [x] **Step 1: Search migration boundaries**

Use `rg.exe` to verify `snmp/` contains no `CoroNet`, `coro_context`, or `coro_socket` references and
the root package files contain no `TurboNet` package/export namespace.

- [x] **Step 2: Build and test the Release slice**

Run the public Release configure preset, build the SNMP targets, and run the focused CTest filter.

- [x] **Step 3: Record remaining removal blockers**

List the exact remaining CoroNet consumers grouped into stream clients, ICE/TURN, and proxy/QUIC.
Do not delete `CoroNet/` until those consumers have replacement tests and implementations.

## Execution evidence

- `cmake --fresh --preset win-release-user` configured successfully after removing the unused
  TurboParser package requirement.
- The first focused build failed while traversing `SaltsNet::CoroNet`, because that legacy target
  still includes removed TurboUtils headers. After SNMP moved to `Salts::CNet`, the target built.
- `test_snmp_cnet` covers a real connected UDP exchange without a coroutine context and rejects a
  stale request id before accepting the matching response.
- The Release SNMP slice passed `test_snmp`, `test_snmp_cnet`, `test_snmp_v3`,
  `test_asn1_roundtrip`, and `test_context_specific`.
- Remaining removal blockers are LDAP; SMTP/IMAP/POP3; ICE/STUN/TURN; and LB/TProxy. These retain
  public or implementation-level coroutine ownership and need
  their own replacement tests before `CoroNet/` can be deleted.
