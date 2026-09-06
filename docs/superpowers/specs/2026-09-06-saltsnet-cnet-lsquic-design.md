# SaltsNet CNet LSQUIC Adapter Design

## Context

The optional LSQUIC target currently exposes CoroNet context and datagram types. Its timer and
receive callbacks are owned by a hidden coroutine loop, so the root CoroNet target cannot be
removed while the adapter remains. LSQUIC itself already separates protocol processing from UDP
transport through `ea_packets_out`, `lsquic_engine_packet_in`, and its advisory tick API.

## Decision

Replace the adapter with a caller-driven `SaltsNet::LSQUIC` target backed by one bounded CNet
datagram. Creation copies the LSQUIC engine API, initializes fixed send/receive capacities, and
arms bounded receive demand. `salts_lsquic_poll` is the only progress owner: it limits the CNet
poll wait by LSQUIC's next advisory tick and processes due connections before returning.

Outbound scatter/gather packets are flattened into one adapter-owned fixed scratch buffer and
then copied into CNet's bounded send storage. `SALTS_ENOBUFS` is reported to LSQUIC as a partial
send; send completion retries LSQUIC's unsent queue. Incoming borrowed CNet views are converted to
portable socket addresses and consumed synchronously by LSQUIC.

CFlow is not inserted into packet transport because CNet callbacks and LSQUIC ticks are fallible
I/O effect boundaries with strict owner-thread ordering. A future observation publisher may expose
copied metrics, but it must not become the socket progress owner.

## Ownership and failure semantics

The adapter owns the CNet datagram, LSQUIC engine, scratch buffer, and one process-global LSQUIC
reference. Caller callbacks and contexts remain borrowed until stop. Configuration capacities are
hard bounds. Invalid addresses, oversized packets, CNet callback errors, and poll errors fail fast;
there is no raw-socket or CoroNet fallback. Stop is idempotent, destroys protocol state, drains the
datagram, and destroy requires a completed stop.

## Compatibility and rollback

This intentionally removes the `turbo_lsquic_*` CoroNet API. Callers migrate to explicit
create/poll/stop/destroy ownership and query the copied local address instead of borrowing a
CoroNet datagram. The independent vendored LSQUIC core remains unchanged. Until merged, rollback
is available through Git history.

## Verification

- build with `SALTSNET_ENABLE_LSQUIC=ON`;
- test lifecycle and stable local endpoint;
- verify scatter/gather output reaches a native UDP peer through CNet;
- verify incoming UDP is consumed without retaining the borrowed view;
- run the adapter test repeatedly and scan built sources for CoroNet references.
