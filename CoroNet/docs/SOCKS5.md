# SOCKS5 Migration Note

`CoroNet` no longer owns SOCKS5 client or UDP ASSOCIATE APIs.

Those proxy-facing capabilities now live in `TProxy`:

- `tproxy/include/turbo_socks5.h`
- `tproxy/include/turbo_socks5_udp.h`
- `tproxy/src/turbo_socks5.c`
- `tproxy/src/turbo_socks5_udp.c`

Rationale:

- SOCKS5 is proxy semantics, not generic transport infrastructure.
- `CoroNet` should expose sockets, contexts, pools, and transport primitives.
- `TProxy` should own SOCKS5/HTTP proxy control flows.

Current boundary:

- `CoroNet`: generic coroutine networking primitives only.
- `TProxy`: SOCKS5 TCP connect, SOCKS5 UDP ASSOCIATE, and proxy routing logic.

If you need SOCKS5 features, include them from `TProxy`, not `CoroNet`.
