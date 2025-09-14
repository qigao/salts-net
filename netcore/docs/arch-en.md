# NetCore Architecture

## Overview

NetCore is an asynchronous networking library built on libuv, providing a unified API for multiple transport protocols with zero-copy buffer support.

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Layer                        │
├─────────────────────────────────────────────────────────────┤
│     async_client_t          │        async_server_t          │
│  ┌─────────────────────┐    │    ┌─────────────────────┐    │
│  │   Event Callback    │    │    │   Event Callback    │    │
│  │  (connected/data/   │    │    │  (listen/conn/data/ │    │
│  │   closed/error)     │    │    │   disconnect/error) │    │
│  └─────────────────────┘    │    └─────────────────────┘    │
├─────────────────────────────┼───────────────────────────────┤
│              Transport Vtable (Polymorphism)                 │
│  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐ ┌──────┐ ┌───────────┐    │
│  │ TCP │ │ UDP │ │ KCP │ │ TLS │ │ PIPE │ │ WebSocket │    │
│  └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘ └──┬───┘ └─────┬─────┘    │
│     └───────┴───────┴───────┴───────┴───────────┘           │
├─────────────────────────────────────────────────────────────┤
│                        libuv                                 │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────────────┐    │
│  │ uv_tcp  │ │ uv_udp  │ │ uv_pipe │ │ uv_async/timer  │    │
│  └─────────┘ └─────────┘ └─────────┘ └─────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    Operating System                          │
│            (epoll / kqueue / IOCP / select)                  │
└─────────────────────────────────────────────────────────────┘
```

---

## Core Components

### 1. Transport Vtable

Each transport implements a common interface via vtable, enabling polymorphic behavior:

```c
typedef struct {
    int (*setup)(void *transport, void *loop);
    void (*close)(void *transport);
    int (*connect)(void *transport, const char *host, int port);
    int (*send)(void *transport, const char *data, size_t len);
    int (*sendv)(void *transport, const async_client_iovec_t *iov, size_t iovcnt);
    int (*get_state)(void *transport);
} async_client_transport_ops_t;
```

**Benefits:**
- Add new transports without modifying core code
- Runtime transport selection
- Consistent API across all protocols

### 2. Event-Driven Architecture

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   libuv      │────>│   Event      │────>│   User       │
│   Event      │     │   Dispatch   │     │   Callback   │
└──────────────┘     └──────────────┘     └──────────────┘
       │                    │
       │              ┌─────┴─────┐
       │              │           │
       v              v           v
   ┌──────┐      ┌──────┐    ┌──────┐
   │ Read │      │Write │    │Timer │
   └──────┘      └──────┘    └──────┘
```

Events are delivered through a single callback:

```c
typedef void (*async_client_event_cb)(
    async_client_t *client,
    const async_client_event_t *event,
    void *user_data
);
```

### 3. Zero-Copy Buffer System

```
┌─────────────────────────────────────────────────────────────┐
│                      turbo_arena_t                           │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                    Memory Pool                       │    │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐   │    │
│  │  │ Buffer1 │ │ Buffer2 │ │ Buffer3 │ │   ...   │   │    │
│  │  │ refcnt=2│ │ refcnt=1│ │ refcnt=0│ │         │   │    │
│  │  └────┬────┘ └────┬────┘ └─────────┘ └─────────┘   │    │
│  └───────┼──────────┼──────────────────────────────────┘    │
│          │          │                                        │
│          v          v                                        │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                     Slices                           │    │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐               │    │
│  │  │ Slice A │ │ Slice B │ │ Slice C │               │    │
│  │  │ buf=1   │ │ buf=1   │ │ buf=2   │               │    │
│  │  │ off=0   │ │ off=100 │ │ off=0   │               │    │
│  │  │ len=100 │ │ len=50  │ │ len=200 │               │    │
│  │  └─────────┘ └─────────┘ └─────────┘               │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

**Key Concepts:**

- **Arena**: Memory pool for allocating buffers
- **Buffer**: Reference-counted memory block
- **Slice**: Zero-copy view into a buffer (offset + length)

**Lifecycle:**

1. Allocate buffer from arena
2. Create slice(s) pointing to buffer
3. Pass slice to send function (refcount incremented)
4. Release your reference (refcount decremented)
5. Library releases after send completes (refcount = 0, buffer recycled)

---

## Transport Implementations

### TCP Transport

```
┌───────────────────────────────────────┐
│            TCP Transport               │
│  ┌─────────────┐  ┌────────────────┐  │
│  │  uv_tcp_t   │  │  Write Queue   │  │
│  │             │  │  ┌──────────┐  │  │
│  │  - connect  │  │  │ pending  │  │  │
│  │  - read     │  │  │ writes   │  │  │
│  │  - write    │  │  └──────────┘  │  │
│  └─────────────┘  └────────────────┘  │
└───────────────────────────────────────┘
```

### KCP Transport (Reliable UDP)

```
┌───────────────────────────────────────┐
│            KCP Transport               │
│  ┌─────────────┐  ┌────────────────┐  │
│  │  uv_udp_t   │  │   ikcp_t       │  │
│  │             │  │                │  │
│  │  - sendto   │  │  - ARQ logic   │  │
│  │  - recvfrom │  │  - congestion  │  │
│  │             │  │  - retransmit  │  │
│  └──────┬──────┘  └───────┬────────┘  │
│         │                 │            │
│         └────────┬────────┘            │
│                  v                     │
│         ┌────────────────┐            │
│         │  uv_timer_t    │            │
│         │  (update tick) │            │
│         └────────────────┘            │
└───────────────────────────────────────┘
```

### TLS Transport

```
┌───────────────────────────────────────┐
│            TLS Transport               │
│  ┌─────────────┐  ┌────────────────┐  │
│  │  uv_tcp_t   │  │  mbedtls_ssl   │  │
│  │             │  │                │  │
│  │  - connect  │  │  - handshake   │  │
│  │  - read     │  │  - encrypt     │  │
│  │  - write    │  │  - decrypt     │  │
│  └──────┬──────┘  └───────┬────────┘  │
│         │                 │            │
│         └────────┬────────┘            │
│                  v                     │
│         ┌────────────────┐            │
│         │  BIO Buffers   │            │
│         │  (read/write)  │            │
│         └────────────────┘            │
└───────────────────────────────────────┘
```

---

## Threading Model

NetCore uses a single-threaded event loop per client/server:

```
┌─────────────────────────────────────────────────────────────┐
│                    Main Thread                               │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              Application Code                        │    │
│  │                                                      │    │
│  │  client = async_client_create(...)                   │    │
│  │  async_client_connect(client, ...)                   │    │
│  │                                                      │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                           │
                           v
┌─────────────────────────────────────────────────────────────┐
│                  Internal I/O Thread                         │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              uv_loop_t                               │    │
│  │                                                      │    │
│  │  while (running) {                                   │    │
│  │      uv_run(loop, UV_RUN_DEFAULT);                   │    │
│  │  }                                                   │    │
│  │                                                      │    │
│  │  Events dispatched to user callback on this thread   │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

**Thread Safety:**
- API calls are thread-safe (use internal async queue)
- Callbacks execute on I/O thread
- User data access in callbacks requires no synchronization

---

## Memory Management

### Buffer Lifecycle

```
1. Allocation
   turbo_arena_get_buffer(&arena, size)
   └── Returns buffer with refcount = 1

2. Create Slice
   turbo_arena_buffer_to_slice(buf, len)
   └── Slice references buffer, refcount still 1

3. Send
   async_client_sendv_slices(client, &slice, 1)
   └── Library increments refcount to 2

4. User Release
   turbo_arena_slice_release(&slice)
   └── Refcount decremented to 1

5. Send Complete
   └── Library releases, refcount = 0
   └── Buffer recycled to arena
```

### Arena Growth

```
Initial:  [████████████████] 64KB

Allocate 48KB:
          [████████████░░░░] 48KB used

Allocate 32KB (exceeds capacity):
          [████████████████████████████████] 128KB (auto-grow)
          [████████████████░░░░░░░░░░░░░░░░] 80KB used
```

---

## File Structure

```
netcore/
├── include/
│   ├── turbo_async_client.h    # Client API
│   ├── turbo_async_server.h    # Server API
│   ├── arena_buffer.h          # Zero-copy buffers
│   ├── client_common.h         # Shared types
│   └── platform.h              # Platform detection
├── src/
│   ├── turbo_async_client.c    # Client implementation
│   ├── turbo_async_server.c    # Server implementation
│   ├── turbo_tcp.c             # TCP transport
│   ├── turbo_udp.c             # UDP transport
│   ├── turbo_kcp.c             # KCP transport
│   ├── turbo_tls.c             # TLS transport
│   ├── turbo_pipe.c            # Pipe transport
│   ├── turbo_websocket_*.c     # WebSocket transport
│   └── arena_buffer.c          # Arena implementation
├── tests/
│   ├── test_async_client.c
│   ├── test_async_server.c
│   └── test_transport_vtable.c
└── docs/
    ├── README.md
    ├── api-en.md / api-zh.md
    ├── guide-en.md / guide-zh.md
    └── arch-en.md / arch-zh.md
```
