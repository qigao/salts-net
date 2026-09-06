# SaltsNet ICE CNet Migration Design

## Context

The ICE module currently exposes CoroNet types in its STUN, TURN, and ICE APIs and stores `coro_socket_t` values in candidates. Its tests cannot currently build because the CoroNet target still includes removed TurboUtils headers. The SaltsNet migration removes that dependency and gives each transport a caller-driven CNet owner.

The following source-level API changes are intentional and authorized by the repository-wide migration:

- `stun_binding_request()` no longer accepts `coro_context_t *`;
- `turn_client_create()` no longer accepts `coro_context_t *`, and `turbo_turn_client_t` becomes opaque;
- `ice_agent_create()` no longer accepts `coro_context_t *`;
- `ice_agent_get_context()` is removed because the agent no longer owns a coroutine context.

Protocol packet formats, credentials, candidate serialization, checklist rules, timeouts, and callback payloads remain unchanged.

The installed Salts SDK currently ships `salts_mdns.h` declarations without an implementation or linkable target. Local mDNS candidate publication therefore fails fast at agent creation during this migration; remote `.local` candidate resolution remains available through the platform resolver. Restoring publication requires a separate CNet multicast API and is not silently implemented with CoroNet or raw fallback I/O.

## Decision

Add one private bounded CNet datagram adapter. The adapter owns a `cnet_datagram`, resolves configured hostnames at the transport boundary, copies callback-borrowed datagrams into caller-provided storage, and provides synchronous send/receive operations by driving `cnet_datagram_poll()` on one owner thread.

Standalone STUN and TURN calls keep their synchronous public behavior. ICE candidate sockets use the same adapter for unconnected UDP so one bound socket can send to and receive from multiple peers. Connectivity checks remain blocking through `ice_agent_start_checks()`. Once a pair is selected, the caller advances data, consent, and disconnect detection through the existing `ice_agent_poll_selected_pair()` API; no hidden coroutine is spawned.

## State and ownership

Each STUN request owns a short-lived datagram adapter. Each TURN client owns one long-lived adapter and all allocation, authentication, permission, channel, and unsolicited-packet state. Each host or server-reflexive ICE candidate owns one adapter; related candidates share the designated base candidate's socket exactly as before. Relay candidates reference their owning TURN client.

Only one thread may drive an adapter at a time. `cnet_datagram_wake()` is reserved for an explicit cross-thread interruption path. Callback peers and receive views are borrowed only for the callback duration and are copied before return. No OS descriptor or CNet internal pointer is exposed publicly.

## Address resolution

CNet datagram bind addresses are numeric. The private adapter therefore uses the platform resolver only to convert an external STUN/TURN hostname into a copied `cnet_datagram_peer`. Resolution is isolated at this boundary, prefers IPv4 to preserve the old UDP-v4 behavior, validates the port, and fails if no supported numeric result exists. ICE candidate addresses are already numeric and bypass DNS.

## Bounds and errors

The adapter uses named fixed capacities for send slots, NativeIO requests, completion batches, and receive storage. STUN is limited by `STUN_MAX_MESSAGE_SIZE`; TURN and ICE use their existing named protocol limits. Oversized or truncated datagrams fail and are never published as valid packets.

CNet admission, polling, callback, timeout, stop, and destroy failures are terminal for the current operation. Public APIs retain their existing `0` success and negative failure convention while internal code preserves exact Salts status until the protocol boundary. Retry loops create a fresh STUN transaction identifier per attempt and use one absolute deadline per attempt. No fallback to CoroNet or raw socket I/O is permitted.

## Compatibility and migration cost

Callers must remove coroutine context construction and stop using the concrete TURN client layout. Applications that previously relied on the selected-pair background coroutine must call `ice_agent_poll_selected_pair()` from their event loop or owner thread. That is a deliberate ownership clarification; automatic background progress would otherwise require an undocumented worker and split the transport fact source.

The ICE target will link `Salts::CNet`, `Salts::Core`, and OpenSSL, not `SaltsNet::CoroNet`. Loopback tests will use Salts threads where two blocking ICE agents must progress concurrently.

## Verification

- a raw UDP loopback STUN server verifies request parsing, transaction matching, mapped-address parsing, retry timeout, and context-free construction;
- TURN parser tests and loopback request/unsolicited-data tests exercise the opaque CNet-backed client;
- ICE host-candidate and two-agent loopback tests exercise bound CNet datagrams, connectivity checks, application data, consent, restart, close, and destruction;
- examples compile without CoroNet types;
- `rg.exe` confirms no CoroNet or `coro_*` symbols remain under `ice/`;
- targeted CMake builds and CTest runs cover STUN, TURN, and ICE before adjacent regressions.

## Rollback

The migration is isolated to the ICE target, its public headers, tests, examples, and documentation. It can be reverted independently until the final repository-wide removal of the CoroNet subtree and package export.
