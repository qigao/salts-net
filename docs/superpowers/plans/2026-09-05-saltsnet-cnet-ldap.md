# SaltsNet CNet LDAP Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the LDAP client's private CoroNet coroutine runtime with a bounded caller-driven CNet TCP/TLS transport while preserving its synchronous public API.

**Architecture:** `ldap_client_t` privately owns one `cnet_client` and one generation-checked connection. Public synchronous calls admit copied commands and drive the single non-overlapping poll lane until a protocol response, terminal transport state, or absolute deadline; LDAP stream chunks are copied from borrowed callbacks into one fixed 64 KiB assembly buffer. `ldap://` maps to `tcp://`, while `ldaps://` maps to verified `tls://` with no plaintext fallback.

**Tech Stack:** C11, CMake Presets, Salts CNet/Core/Platform/TinyTest, OpenSSL, TinyTest

**Spec:** `docs/superpowers/specs/2026-09-05-saltsnet-cnet-migration-design.md`

## Global Constraints

- The existing synchronous `ldap_client_*` public signatures remain unchanged.
- CNet callbacks execute only from the non-overlapping caller-owned poll lane.
- A receive view is copied before `on_receive` returns and never retained.
- Connection capacity is 1; command, request, completion, and event capacities are 4.
- A copied send is at most 4096 bytes and the LDAP receive assembly is at most 65536 bytes.
- `ldaps://` performs verified TLS 1.2+ through CNet and never falls back to plaintext.
- Stop precedes destroy and client-owned storage remains valid until CNet is quiescent.
- No production fallback to CoroNet or native blocking sockets is allowed.

---

### Task 1: LDAP transport regression tests

**Files:**
- Create: `ldap/tests/test_ldap_cnet.c`
- Modify: `ldap/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ldap_client_create`, `ldap_client_connect`, `ldap_client_simple_bind`, `ldap_client_destroy`, Salts platform threads
- Produces: local deterministic proof for plaintext LDAP response handling and LDAPS fail-closed behavior

- [x] **Step 1: Write a real loopback BindResponse test**

Start a numeric IPv4 loopback TCP listener on an ephemeral port in the test owner, accept in a Salts thread, read one request, and write this complete successful BindResponse:

```c
static const uint8_t LDAP_TEST_BIND_RESPONSE[] = {
    0x30, 0x0c, 0x02, 0x01, 0x01, 0x61, 0x07,
    0x0a, 0x01, 0x00, 0x04, 0x00, 0x04, 0x00};
```

Create the client with `ldap://127.0.0.1:<port>`, call `ldap_client_simple_bind`, and assert return code `0` and LDAP result code `LDAP_SUCCESS`.

- [x] **Step 2: Write an LDAPS plaintext-rejection test**

Accept one TCP peer and immediately close it without a TLS handshake. Create the client with `ldaps://127.0.0.1:<port>` and a 250 ms timeout, then assert `ldap_client_connect(client) != 0`. This catches the prior behavior where `ldaps://` silently used plaintext TCP.

- [x] **Step 3: Run the focused target and observe RED**

Run:

```powershell
cmake --build --preset win-release-user --target test_ldap_cnet
```

Expected before migration: the LDAP target still traverses `SaltsNet::CoroNet`, or the LDAPS plaintext-rejection assertion fails under the legacy transport.

### Task 2: Bounded CNet LDAP transport

**Files:**
- Modify: `ldap/src/ldap_client.c`
- Modify: `ldap/CMakeLists.txt`
- Test: `ldap/tests/test_ldap_cnet.c`

**Interfaces:**
- Consumes: `cnet_client_init`, `cnet_connect`, `cnet_send`, `cnet_receive`, `cnet_send_and_close`, `cnet_client_poll`, `cnet_client_stop`, `cnet_client_destroy`
- Produces: the existing synchronous LDAP operations backed only by CNet

- [x] **Step 1: Replace coroutine ownership with CNet state**

Store `cnet_client`, `cnet_connection`, TLS selection, initialization/connect/terminal/send/receive flags, and the first transport status in `ldap_client_t`. Select IOCP on Windows, epoll on Linux, and kqueue elsewhere.

- [x] **Step 2: Validate URLs and initialize fixed capacity**

Accept `ldap://`, `ldaps://`, and the legacy scheme-less plaintext endpoint form, require a non-empty host, reject unknown schemes, and validate a decimal port in `1..65535`. Initialize CNet with one connection, four command/request/completion/event slots, 4096 copied send bytes, 65536 receive bytes, and at least `CNET_TLS_MIN_IO_BUFFER_BYTES` of TLS storage.

- [x] **Step 3: Implement callback ownership and error conversion**

`on_state` records connected or terminal state. `on_receive` requires `CNET_MESSAGE_BYTES`, checks `recv_buf_used + view->size <= 65536`, copies the borrowed bytes, and records a protocol/resource error on overflow. `on_send` clears the one-write-pending flag. Convert `SALTS_ETIMEDOUT` to `LDAP_CLIENT_ERROR_TIMEOUT`, TLS connection failure to `LDAP_CLIENT_ERROR_TLS`, and other CNet failures to `LDAP_CLIENT_ERROR_NETWORK`.

- [x] **Step 4: Implement connect and request progress loops**

Map the stored LDAP endpoint to a checked `tcp://` or `tls://` URI. Admit connect once and poll to connected, terminal, or the absolute timeout. For each request, admit one receive demand before the copied send, drain complete BER messages after callbacks, re-arm one receive only when more bytes are needed, and stop at the same absolute deadline.

- [x] **Step 5: Preserve operation behavior and ordered unbind**

Keep the existing builders, message-id matching, search-entry callbacks, and result transfer. Replace coroutine wrappers with direct synchronous helpers. Send UnbindRequest through `cnet_send_and_close`, poll until terminal or deadline, then reset connection state so destroy can stop and drain safely.

- [x] **Step 6: Replace the build dependency**

Link LDAP privately to `Salts::CNet` and remove `SaltsNet::CoroNet`. Keep existing public dependencies unchanged unless compiler evidence shows they are implementation-only.

- [x] **Step 7: Run focused tests to GREEN**

Build and run `test_ldap_cnet` and `test_ldap`. Expected: both pass, with the loopback test requiring neither external network nor coroutine context.

### Task 3: Adjacent regression and removal-boundary verification

**Files:**
- Modify: `docs/superpowers/plans/2026-09-05-saltsnet-cnet-ldap.md`
- Modify if evidence changes: `docs/superpowers/specs/2026-09-05-saltsnet-cnet-migration-design.md`

**Interfaces:**
- Consumes: completed Tasks 1-2
- Produces: reproducible LDAP migration evidence and an updated CoroNet blocker inventory

- [x] **Step 1: Scan the LDAP boundary**

Use `rg.exe` to prove `ldap/` has no `CoroNet`, `coro_context`, or `coro_socket` references and that its production target links `Salts::CNet`.

- [x] **Step 2: Run Release regression tests**

Build the LDAP and already-migrated SNMP test targets, then run a CTest filter covering `test_ldap`, `test_ldap_cnet`, `test_snmp`, `test_snmp_cnet`, `test_snmp_v3`, `test_asn1_roundtrip`, and `test_context_specific`.

- [x] **Step 3: Record evidence and remaining blockers**

Record exact configure/build/test outcomes. Re-run the repository-wide CoroNet consumer scan and list the remaining mail, ICE/STUN/TURN, proxy/load-balancer, and optional QUIC adapter consumers; do not delete `CoroNet/` while any production consumer remains.

## Execution evidence

- The initial `test_ldap_cnet` build followed `ldap_client -> SaltsNet::CoroNet` and failed in the
  legacy CoroNet target before the new test could link. This established the dependency-level RED.
- `ldap_client` now links `Salts::CNet` privately and contains no `CoroNet`, `coro_context`,
  `coro_socket`, or `TURBONET_LDAP` reference.
- `test_ldap_cnet` performs a real loopback TCP BindRequest/BindResponse exchange without a
  coroutine context and proves that an `ldaps://` client rejects a plaintext peer.
- The Release build produced `test_ldap` and `test_ldap_cnet`; the focused LDAP CTest run passed
  2/2 tests.
- The adjacent Release CTest run passed 7/7 tests: `test_ldap`, `test_ldap_cnet`, `test_snmp`,
  `test_snmp_cnet`, `test_snmp_v3`, `test_asn1_roundtrip`, and `test_context_specific`.
- `clang-format --dry-run --Werror` passed for the modified LDAP implementation and loopback test;
  `git diff --check` also passed.
- Remaining production CoroNet blockers are the SMTP/IMAP/POP3 client family, ICE/STUN/TURN,
  load-balancer/bidirectional-pump, transparent proxy, and optional LSQUIC adapter. Their CMake
  targets still require `SaltsNet::CoroNet`, so the root `add_subdirectory(CoroNet)` cannot yet be
  removed safely.
