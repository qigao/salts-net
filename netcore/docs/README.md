# NetCore Module

Async networking core built on libuv with zero-copy arena buffers.

基于 libuv 构建的异步网络核心，支持零拷贝 arena buffer。

## Documentation

| English | 中文 |
|---------|------|
| [API Reference](./api-en.md) | [API 参考](./api-zh.md) |
| [Guide](./guide-en.md) | [使用指南](./guide-zh.md) |
| [Architecture](./arch-en.md) | [架构设计](./arch-zh.md) |

## Quick Start

```c
#include "turbo_async_client.h"

// Create TCP client
async_client_t *client = async_client_create(
    ASYNC_CLIENT_TRANSPORT_TCP,
    on_event,
    user_data
);

// Connect
async_client_connect(client, "127.0.0.1", 8080);

// Send data
async_client_send(client, "Hello", 5);

// Cleanup
async_client_destroy(client);
```

## Supported Transports

| Transport | Client | Server | Description |
|-----------|--------|--------|-------------|
| TCP | ✅ | ✅ | Reliable stream |
| UDP | ✅ | ✅ | Unreliable datagram |
| KCP | ✅ | ✅ | Reliable UDP (ARQ) |
| TLS | ✅ | ✅ | Encrypted TCP |
| Pipe | ✅ | ✅ | Unix socket / Named pipe |
| WebSocket | ✅ | ✅ | WebSocket protocol |
