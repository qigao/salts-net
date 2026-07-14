# Outbound stream proxies

`CoroNet` owns outbound proxy tunneling because the tunnel is a property of a
client stream, not of an application protocol. `TProxy` remains the inbound
proxy server.

Supported client modes:

- direct TCP (default)
- SOCKS5 `CONNECT`, with no authentication or RFC 1929 username/password
- HTTP/1.1 `CONNECT`, with optional preemptive Basic proxy authorization

Once the tunnel is established, the socket is an ordinary byte stream. TLS,
WebSocket, HTTP, LDAP, SMTP, POP3, IMAP, and other TCP protocols can therefore
reuse it without implementing SOCKS5 or HTTP CONNECT themselves.

## Socket configuration

The configuration is copied by `coro_socket_set_proxy()`. Configure it before
calling a connect function.

```c
coro_proxy_config_t proxy = {
    .type = CORO_PROXY_SOCKS5,
    .host = "127.0.0.1",
    .port = 1080,
    .username = "user", /* optional */
    .password = "pass", /* optional */
};

coro_socket_t *socket = coro_socket_create_tcpv4(ctx);
if (!socket || coro_socket_set_proxy(socket, &proxy) != 0 ||
    coro_socket_connect(socket, "ldap.example.com", 389) != 0) {
  /* handle error */
}
```

Creating `CORO_SOCKET_TLS` applies TLS after the proxy tunnel is ready. The
target hostname remains the TLS SNI and certificate-validation context.

```c
coro_socket_t *socket = coro_socket_create(ctx, CORO_SOCKET_TLS);
coro_socket_set_proxy(socket, &proxy);
coro_socket_connect(socket, "mail.example.com", 995); /* POP3 over TLS */
```

The WebSocket helpers also inherit the configuration:

```c
coro_socket_t *socket = coro_socket_create_tcpv4(ctx);
coro_socket_set_proxy(socket, &proxy);
coro_socket_connect_ws(socket, "events.example.com", 443, "/feed", 1);
```

Connection pools copy the same settings into every new socket:

```c
coro_pool_t *pool = coro_pool_create(ctx, NULL);
coro_pool_set_proxy(pool, &proxy);
coro_pool_open(pool, "api.example.com", 443, CORO_SOCKET_TLS);
```

Pass `NULL` or `CORO_PROXY_DIRECT` to `coro_socket_set_proxy()` /
`coro_pool_set_proxy()` to clear the setting before connect.

## Contracts and limits

- Proxying applies to client TCP streams. UDP, KCP, pipes, bind, and listen do
  not use this setting; this API does not implement SOCKS5 UDP ASSOCIATE.
- SOCKS5 hostnames are sent to the proxy, so target DNS is resolved remotely.
  The socket connect policy validates the actual proxy endpoint; it cannot
  inspect IP addresses that only the proxy resolves for the target.
- The configured socket timeout bounds proxy DNS, proxy TCP connect, and proxy
  negotiation as one connect deadline.
- A successful proxy response can share a receive packet with target data.
  CoroNet preserves those extra bytes for the first normal `coro_socket_recv()`.
- SOCKS5 username/password and HTTP Basic credentials are not encrypted by
  those authentication schemes. Use only a trusted network or a separately
  protected proxy transport when credentials are sensitive.
- HTTP CONNECT accepts any `2xx` response and switches to tunnel mode after the
  response header. `407` maps to `TURBO_EPERM`; other non-`2xx` statuses map to
  `TURBO_ECONNREFUSED`.

Protocol references:

- [RFC 1928: SOCKS Protocol Version 5](https://www.rfc-editor.org/rfc/rfc1928)
- [RFC 1929: SOCKS5 Username/Password Authentication](https://www.rfc-editor.org/rfc/rfc1929)
- [RFC 9110 §9.3.6: CONNECT](https://www.rfc-editor.org/rfc/rfc9110#section-9.3.6)
- [RFC 7617: Basic HTTP Authentication](https://www.rfc-editor.org/rfc/rfc7617)
