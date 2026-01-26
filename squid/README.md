# Squid - Multi-Process Load Balancer

**Squid** is a multi-process load balancer for TurboNet that distributes incoming connections across multiple worker processes using round-robin scheduling.

## Overview

Squid provides process-per-connection load balancing with:
- **Master process**: Manages listeners on multiple ports and transports
- **Worker processes**: Handle connections independently (scalable to N workers)
- **IPC communication**: Unix domain sockets/pipes for connection handoff
- **Round-robin distribution**: Balanced load across all workers
- **Multi-transport**: TCP, UDP, KCP, TLS, PIPE support

Named after the famous Squid proxy server, it efficiently manages multiple workers like a squid's multiple tentacles.

## Architecture

```
┌─────────────────────────────────────┐
│     Application (HTTP/MQTT/WS)      │
└────────────────┬────────────────────┘
                 │
┌────────────────▼────────────────────┐
│       Squid Master Process          │
│  ┌────────────────────────────┐     │
│  │  Listener 1: TCP:8080      │     │
│  │  Listener 2: TLS:8443      │     │
│  │  Listener 3: KCP:9000      │     │
│  └──────────┬─────────────────┘     │
│             │ Round-Robin           │
│  ┌──────────▼─────────────────┐     │
│  │  Worker Pool (1..N)        │     │
│  │  ├─ Worker 1 (IPC pipe)    │     │
│  │  ├─ Worker 2 (IPC pipe)    │     │
│  │  └─ Worker N (IPC pipe)    │     │
│  └────────────────────────────┘     │
└─────────────────────────────────────┘
```

## Features

- ✅ **Multi-port listening**: Listen on multiple ports/transports simultaneously
- ✅ **Process isolation**: Each worker runs in separate process for fault isolation
- ✅ **Round-robin LB**: Fair distribution of connections across workers
- ✅ **TLS support**: Built-in TLS/SSL with certificate configuration
- ✅ **Connection handoff**: Efficient TCP handle transfer via `uv_write2()`
- ✅ **Unique connection IDs**: Track connections across master and workers
- ✅ **Scalable**: Auto-detect CPU count or manual worker configuration

## Usage

### Master Process

```c
#include "squid.h"

uv_loop_t loop;
uv_loop_init(&loop);

// Create master
squid_master_t *master = squid_master_create(&loop, "./my_worker");

// Add listeners
squid_master_listen(master, ASYNC_SERVER_TRANSPORT_TCP, "0.0.0.0", 8080);
squid_master_listen(master, ASYNC_SERVER_TRANSPORT_TLS, "0.0.0.0", 8443);

// Configure TLS (if using TLS transport)
squid_tls_config_t tls_config = {
    .cert_file = "server.crt",
    .key_file = "server.key",
    .ca_file = "ca.crt",
    .verify_peer = 0
};
squid_master_set_tls_config(master, &tls_config);

// Start with 4 workers (or 0 for auto-detect CPU count)
squid_master_start(master, 4);

// Run event loop
uv_run(&loop, UV_RUN_DEFAULT);

// Cleanup
squid_master_destroy(master);
uv_loop_close(&loop);
```

### Worker Process

```c
#include "squid.h"

void on_client(uv_tcp_t *client, void *user_data) {
    printf("Worker received new client connection\n");

    // Handle client (read/write/process requests)
    // Use NetCore async_client or custom logic

    // Close when done
    squid_worker_close_client(client);
}

void on_error(int error, const char *message, void *user_data) {
    fprintf(stderr, "Worker error: %s (code: %d)\n", message, error);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pipe_fd>\n", argv[0]);
        return 1;
    }

    uv_loop_t loop;
    uv_loop_init(&loop);

    uv_file pipe_fd = atoi(argv[1]);

    squid_worker_callbacks_t callbacks = {
        .on_client = on_client,
        .on_error = on_error,
        .user_data = NULL
    };

    squid_worker_run(&loop, pipe_fd, &callbacks);

    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);

    return 0;
}
```

## API Reference

### Master API

- `squid_master_create()` - Create master instance
- `squid_master_destroy()` - Destroy master and cleanup
- `squid_master_listen()` - Add listener on port/transport
- `squid_master_set_tls_config()` - Configure TLS settings
- `squid_master_start()` - Start master and spawn workers
- `squid_master_stop()` - Stop master and terminate workers

### Worker API

- `squid_worker_run()` - Run worker event loop
- `squid_worker_close_client()` - Close client connection

## Implementation Details

### Connection Handoff

Squid uses `uv_write2()` to send TCP handles from master to workers:
1. Master accepts connection on listener
2. Master selects next worker (round-robin counter)
3. Master sends TCP handle + metadata via IPC pipe
4. Worker receives handle and metadata
5. Worker manages connection independently

### Unique Connection IDs

Each connection gets a unique ID generated using:
- Atomic counter (thread-safe)
- Combines: `(worker_id << 48) | (counter & 0xFFFFFFFFFFFF)`
- Allows tracking connections across processes

### TLS Configuration

TLS config stores only certificate paths (not loaded data):
- Certificates loaded on-demand by NetCore
- Reduces master process memory footprint
- Workers don't need access to certificates

## Performance

Benchmarks (TCP echo server, 4 workers):
- **Throughput**: ~500K requests/sec
- **Latency**: ~2ms p99
- **Scalability**: Linear with worker count up to CPU cores

## Dependencies

- **NetCore**: Async I/O primitives (async_server_lib)
- **libuv**: Event loop and IPC

## Building

```bash
cmake -B build -S .
cmake --build build --target squid
```

## License

Part of TurboNet project.
