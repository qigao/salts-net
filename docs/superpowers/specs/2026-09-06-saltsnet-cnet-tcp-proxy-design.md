# SaltsNet CNet TCP Proxy Design

## Scope and evidence

The existing TProxy combines SOCKS5, HTTP CONNECT, raw forwarding, UDP relay,
rules, health checks, configuration loading, and coroutine/thread-pool
ownership in one object. Its authentication helper is a success stub and its
SOCKS command branch cannot reach UDP ASSOCIATE after first requiring CONNECT.
Those paths cannot be retained as trustworthy behavior.

This migration creates a bounded TCP proxy surface for the behavior that can
be implemented and verified now: SOCKS5 CONNECT, HTTP CONNECT, authenticated
handshakes, access control, routing, and raw configured forwarding. UDP relay,
transparent interception, configuration-file loading, and background health
checks are not exposed by the new API until they have dedicated CNet/CFlow
ownership designs and integration tests.

## Ownership and state

- One caller-owned `salts_tcp_proxy` owns a CNet listener and CNet client.
- `salts_tcp_proxy_poll()` is the sole progress owner.
- Fixed session slots own downstream/upstream handles and bounded handshake
  storage. CNet borrowed receive views are copied only while a handshake must
  accumulate; pump sends use CNet copied admission directly.
- A CMeta `Enum` describes the stable public protocol classification. Runtime
  algorithms and storage remain ordinary C as required by the CMeta boundary.
- Socket protocol progress is not represented as a CFlow synchronous array
  plan: network operations are IO/fallible effect barriers. A later event
  publisher adapter may expose observations to CFlow without transferring
  socket ownership.
- Once pumping begins, each source is re-armed only after the peer write
  completes. This bounds relay retention to CNet command storage.

## Protocol behavior

SOCKS5 accepts CONNECT with IPv4, IPv6, or bounded domain targets. Username /
password authentication follows RFC 1929 when credentials are configured.
HTTP accepts complete CONNECT headers; configured Basic credentials are
compared against a Salts base64 encoding. Any malformed, oversized,
unsupported, unauthenticated, denied, or unroutable request receives the
protocol-appropriate failure where possible and then closes.

## Shutdown and errors

New admission stops first, then live connections drain through CNet. Public
stop is idempotent and destroy requires successful stop. Capacity exhaustion
does not grow storage or create a fallback owner. The first CNet progress or
admission failure becomes the proxy error returned by poll.

## Verification

Native loopback peers verify fragmented SOCKS5 and HTTP handshakes, auth
success/failure, access denial, raw forwarding, bidirectional payload flow,
capacity rejection, and deterministic stop/destroy. C++ header compilation
verifies that the CMeta enum/public ABI remains consumable from C++.
