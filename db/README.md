# Netcore Clients

This directory contains async client libraries for various databases and services, built on top of netcore's async_client_lib.

## Available Clients

### PostgreSQL Client (pquv)
- **Location**: `postgresql/`
- **Description**: Async PostgreSQL client using libpq
- **Features**:
  - Non-blocking queries
  - Query queueing
  - Cross-platform (Unix poll, Windows timer)
  - Connection pooling support

### Redis Client (RESP)
- **Location**: `redis/`
- **Description**: Async Redis client implementing RESP protocol
- **Features**:
  - Full RESP protocol support
  - Pipelined commands
  - Common Redis commands (GET, SET, LPUSH, HSET, etc.)
  - Authentication and database selection

## Usage

Each client is self-contained with its own header and implementation files.

### PostgreSQL Example
```c
#include "postgresql/pquv.h"

void on_result(pg_async_t *pg, PGresult *result, void *data) {
    // Process result
}

pg_async_t *pg = pquv_create(conn, NULL);
pquv_queue(pg, "SELECT * FROM users", 0, NULL, on_result, NULL);
pquv_execute(pg);
```

### Redis Example
```c
#include "redis/redis_client.h"

void on_get(redis_client_t *client, redis_reply_t *reply, void *data) {
    printf("Value: %s\n", reply->str);
}

redis_client_t *client = redis_client_create("localhost", 6379);
redis_client_connect(client, NULL, NULL);
redis_get(client, "mykey", on_get, NULL);
```

## Building

These clients are automatically built as part of the netcore library.

## Dependencies

- **pquv**: Requires libpq (PostgreSQL client library)
- **redis**: No external dependencies (pure RESP implementation)

## Adding New Clients

To add a new client:
1. Create a subdirectory (e.g., `mongodb/`)
2. Add header file with public API
3. Add implementation file
4. Update CMakeLists.txt
5. Add documentation here
