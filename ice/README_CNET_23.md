# ICE/STUN/TURN over CNet 2.3 (ACE/POSA2 boundary)

SaltsNet ICE is a **datagram consumer**, not a stream connection pool. It
already uses CNet's native Reactor/Proactor backend and caller-owned progress:
`ice_cnet_datagram` for bounded send, receive demand, completion tags, wake,
and stop. It does not require `cnet_manager`, stream `cnet_client_pool`, or
`cnet_managed_dial` just to satisfy a 2.3 version check.

| Component | CNet 2.3 responsibility | SaltsNet protocol responsibility |
| --- | --- | --- |
| ICE candidate UDP | CNet Datagram single Owner, completion/receive ownership | RFC 8445 checks, priorities, triggered/nominated pairs and consent |
| STUN | CNet send/receive/poll, correlated completion | Binding transaction ID, sender validation, timed UDP retransmission |
| TURN | CNet datagram transport and bounded progress | 401/438 challenges, allocation/permissions/channels, authentication and refresh |
| TCP Proxy | CNet stream state + destination strategy | SOCKS5/HTTP CONNECT framing, authorization, one tunnel per client |
| LB | CNet destination strategy for ready workers | Worker-group routing, framing, bounded session/request queues |

**No hidden protocol retransmit:** A CNet physical reconnect is not proof that
a STUN/TURN/ICE request is safe to replay. ICE/TURN retransmission follows its
own RFC transaction/nonce/permission constraints and preserves its own total
time budget. CNet Client Pool and SG-host cohabitation require explicit
long-lived Owner leases; this synchronous datagram API owns its own backend
and must not pretend to be a native shared SG service.

The `ice_cnet_23` CTest exercises real loopback send terminal, receive demand,
buffer-too-small retry without packet loss, invalid admission and generation
tag overflow. Host tests execute on Linux x64/ARM64, Windows and macOS. Mobile
CI qualifies SDK compile/link only.
