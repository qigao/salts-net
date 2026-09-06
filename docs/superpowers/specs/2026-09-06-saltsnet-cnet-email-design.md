# SaltsNet Email CNet Migration Design

## Context

The email module exposes synchronous SMTP, POP3, and IMAP clients, but their transport is currently a CoroNet socket bound to a caller-provided `coro_context_t`. The SaltsNet migration removes that dependency and uses the installed Salts CNet runtime. The protocol behavior remains synchronous from the caller's perspective.

The migration changes the public constructors because a coroutine context is no longer part of ownership:

- `smtp_client_create(const smtp_config_t *config)`
- `pop3_client_create(const pop3_config_t *config)`
- `imap_client_create(const imap_config_t *config)`
- the unified `email_client_create_*` functions likewise no longer accept a context

This is an intentional source-level API break authorized by the repository-wide TurboNet-to-SaltsNet refactor.

## Decision

Add one private `email_cnet_transport` adapter shared by all three protocols. Each protocol client owns one adapter, and the thread executing a protocol call is the sole CNet progress owner. The adapter drives `cnet_client_poll()` until a connect, send, receive, TLS upgrade, close, timeout, or interruption reaches a terminal condition.

The adapter provides:

- bounded single-connection CNet ownership;
- `tcp://` and verified `tls://` connection admission;
- chunked synchronous sends over CNet's fixed maximum command size;
- one-demand-at-a-time receives copied out of callback-borrowed storage;
- same-handle `cnet_start_tls()` for SMTP STARTTLS, POP3 STLS, and IMAP STARTTLS;
- deterministic stop/destroy cleanup;
- cross-thread interruption through an atomic status plus `cnet_client_wake()`.

Protocol parsers keep their existing line and literal buffers. A CNet receive callback copies its borrowed view into bounded transport storage; parser code then consumes it synchronously. No callback-borrowed pointer escapes the callback.

## State and ownership

The protocol client is the sole owner of configuration copies, parser buffers, and the private transport. The transport is the sole owner of `cnet_client` and its generation-checked connection handle.

The progress thread alone may connect, poll, send, receive, upgrade TLS, close, stop, or destroy. A concurrent interrupter may only publish an atomic non-zero status and call `cnet_client_wake()`. The interrupter must stop before client destruction, matching CNet's wake lifetime contract.

An admitted interrupt closes the active connection on the progress thread. A timeout or transport failure also makes that connection unusable. Reconnection starts with cleared protocol read state and a fresh CNet connection handle.

## TLS behavior

Direct TLS uses `tls://host:port`. STARTTLS/STLS first consumes the complete positive plaintext protocol response, then calls `cnet_start_tls()` on the same quiescent connection. The client waits for `CNET_CONNECTION_TLS_HANDSHAKING` followed by `CNET_CONNECTION_CONNECTED`.

Peer and hostname verification remain enabled by CNet. TLS failure is terminal: the client closes the connection and never retries in plaintext. Protocol read buffers are cleared at the plaintext/TLS boundary so pre-upgrade bytes cannot be interpreted as encrypted application data.

## Bounds and errors

All CNet capacities and retained byte storage are named private constants. Sends larger than one CNet command are split into ordered chunks; only one send is pending at a time. Parser buffers retain their existing protocol-specific bounds. POP3 multiline bodies remain dynamically allocated as before, but arithmetic and allocation growth are checked and a named maximum response size is enforced.

Public protocol operations keep their existing `0`/`-1` success/failure convention. Diagnostic strings include the failed transport operation and CNet status. `smtp_interrupt()` and `pop3_interrupt()` return Salts error codes (`SALTS_OK`, `SALTS_EINVAL`, `SALTS_ENOTCONN`, or a wake error).

## Compatibility and migration cost

User-visible protocol commands, response parsing, authentication selection, message serialization, and external integration environment variables remain unchanged. Callers must remove coroutine creation/spawn/run code and stop passing `coro_context_t *` to constructors.

The email target links `Salts::CNet` and no longer links `SaltsNet::CoroNet`. Other repository modules may retain CoroNet temporarily; removing the top-level CoroNet subtree is deferred until those consumers migrate.

## Verification

- constructor and validation tests compile without CoroNet types;
- plaintext loopback SMTP and POP3 tests exercise synchronous CNet connect/send/receive;
- a STARTTLS loopback test verifies that the existing socket carries a TLS ClientHello after the positive upgrade response and that plaintext failure does not reconnect or fall back;
- existing message tests and opt-in smtp4dev roundtrip remain available;
- examples and README compile against the context-free API;
- `rg.exe` confirms no CoroNet or coroutine symbols remain under `email/`;
- targeted CMake build and CTest runs cover all email tests and examples.

## Rollback

The change is isolated to the email target, its public headers, examples, tests, and documentation. It can be reverted without changing message/MIME modules or the already migrated LDAP/SNMP transports.
