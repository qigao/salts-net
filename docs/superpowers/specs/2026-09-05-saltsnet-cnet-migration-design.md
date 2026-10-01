# SaltsNet CNet Migration Design

## Decision

The repository becomes the `SaltsNet` package and uses the installed Salts SDK as its networking
and runtime foundation. `CoroNet` is removed after every built consumer has moved to CNet; it is not
kept as a fallback or compatibility implementation. The package, exported CMake namespace, build
options, and visibility macros use the `SaltsNet` / `SALTSNET` names.

## Existing constraints

- CNet is caller-driven. A non-overlapping owner calls `cnet_client_poll()` or the corresponding
  datagram/packet poll function. CNet does not create an I/O worker thread.
- Successful send admission copies bytes into bounded CNet-owned storage. Receive bytes are borrowed
  only for the callback and must be parsed or copied before callback return.
- Each owner has explicit positive capacities. Full capacity is a normal error and is never replaced
  by unbounded allocation.
- Stop closes admission and drains terminal work; destroy follows a completed stop.
- TLS remains fail-closed and verifies the peer and hostname. Protocols that require in-place
  STARTTLS need a CNet capability before their CoroNet implementation can be removed.

## Target architecture

```text
application / CLI
    -> protocol client owner (SNMP, LDAP, SMTP, IMAP, POP3, ICE/TURN, proxy)
        -> protocol codec and operation state
        -> CFlow statechart only for long-lived multi-event protocol workflows
        -> CNet TCP/TLS/UDP/KCP/WebSocket owner
            -> Salts NativeIO

CMeta describes stable protocol enums, records, and typed events. It does not own wire parsing,
runtime storage, or I/O progress.
```

Each protocol client owns its mutable operation state. CNet is the sole transport fact source.
Callbacks synchronously consume borrowed receive views and copy only the bytes that must survive the
callback. A synchronous legacy-shaped operation may drive its private CNet owner until one terminal
result; long-lived services expose explicit poll/stop/destroy operations instead of hidden threads.

## Migration units

1. **Package foundation** — rename the project/package/export namespace to SaltsNet, resolve Salts
   exclusively through `SALTS_ROOT`, and replace Rocida CMake targets with their Salts equivalents.
2. **SNMP over connected CNet UDP** — preserve the synchronous SNMP public operations while removing
   the requirement to run inside a coroutine context. One private `cnet_client` owns one connected
   UDP handle; each retry admits one receive and one copied send, then polls to response or deadline.
3. **LDAP and mail stream clients** — move direct TLS and plaintext operations to `cnet_client`.
   Extend CNet first for same-connection STARTTLS; never reconnect and pretend it is an upgrade.
4. **ICE/STUN/TURN** — use CNet datagram/packet owners and explicit polling. CFlow owns the
   connectivity-check and consent state machine; CNet remains the only socket owner.
5. **LB/TProxy adapters** — use CNet listeners, generation-checked connections, bounded
   bidirectional forwarding, and explicit shutdown. WebSocket framing uses `<cnet/websocket.h>`.
6. **Removal gate** — delete `CoroNet/`, coroutine-only adapters/tests/examples, and every CoroNet or
   `coro_*` reference after replacement tests pass. Package installation must contain no CoroNet
   headers, libraries, targets, or transitive dependencies.

## SNMP data-path protocol

| Item | Contract |
|---|---|
| Data unit | One bounded SNMP UDP datagram, at most `recv_buffer_size` bytes |
| Fact source | The private CNet connection and the request's terminal state |
| Ownership | `cnet_send` copies request bytes; receive callback parses/copies before return |
| Topology | One caller thread, one CNet owner, one in-flight SNMP request |
| Ordering | One request/response at a time; responses with a different request id are ignored |
| Capacity | One connection, bounded command/event/request slots, configured receive maximum |
| Backpressure | CNet admission errors map to `SNMP_CLIENT_ERROR_NETWORK`; no retry on invalid bounds |
| Timeout | Absolute request deadline; timeout retries at most `retries + 1` total attempts |
| Shutdown | Reject new work, close connection, stop CNet, destroy CNet, then free client state |

## Compatibility and error semantics

This is a major-version migration: the CMake package and namespace change without TurboNet aliases.
Protocol function names such as `snmp_client_get` remain when their behavior can be preserved. Public
types that expose `coro_context_t` are replaced by explicit owner/poll APIs in their migration unit.
Existing protocol error domains remain stable where possible; a CNet status is converted once at the
protocol boundary and is not logged again by lower layers.

## Verification gates

- Configure/build/test/install use the repository's public CMake presets.
- Every migrated transport has loopback success, timeout, malformed response, capacity, and shutdown
  tests. TLS clients additionally cover verification failure and direct-TLS/STARTTLS parity.
- `rg.exe` finds no built dependency on `CoroNet`, `TurboNet::CoroNet`, or coroutine socket/context
  APIs before the removal commit.
- The final installed-package smoke test uses `find_package(SaltsNet CONFIG REQUIRED)` and links only
  `SaltsNet::*` plus transitive Salts targets.
