# PostgreSQL Client (pquv)

Async PostgreSQL client using libpq, built on libuv for non-blocking database operations.

## Features

- **Async Queries**: Non-blocking PostgreSQL queries
- **Query Queueing**: Queue multiple queries for sequential execution
- **Cross-Platform**: Unix (poll) and Windows (timer) support
- **Connection Reuse**: Use existing PGconn connections
- **Error Handling**: Detailed error reporting
- **Callback-Based**: Event-driven result handling

## Building

The PostgreSQL client requires libpq:

```bash
# Ubuntu/Debian
sudo apt-get install libpq-dev

# macOS
brew install postgresql

# Windows
# Install PostgreSQL or use vcpkg
vcpkg install libpq
```

Build with netcore:

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## Usage

### Basic Example

```c
#include "pquv.h"
#include <stdio.h>

void on_result(pg_async_t *pg, PGresult *result, void *data) {
    if (PQresultStatus(result) == PGRES_TUPLES_OK) {
        int rows = PQntuples(result);
        int cols = PQnfields(result);
        
        for (int i = 0; i < rows; i++) {
            for (int j = 0; j < cols; j++) {
                printf("%s\t", PQgetvalue(result, i, j));
            }
            printf("\n");
        }
    } else {
        fprintf(stderr, "Query failed: %s\n", PQresultErrorMessage(result));
    }
    
    PQclear(result);
}

int main() {
    // Connect to PostgreSQL
    PGconn *conn = PQconnectdb("host=localhost dbname=mydb user=myuser");
    
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "Connection failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return 1;
    }
    
    // Create async context
    pg_async_t *pg = pquv_create(conn, NULL);
    
    // Queue queries
    pquv_queue(pg, "SELECT * FROM users", 0, NULL, on_result, NULL);
    pquv_queue(pg, "SELECT * FROM posts", 0, NULL, on_result, NULL);
    
    // Execute queries
    pquv_execute(pg);
    
    // Run event loop...
    
    return 0;
}
```

### Parameterized Queries

```c
void on_user_result(pg_async_t *pg, PGresult *result, void *data) {
    if (PQresultStatus(result) == PGRES_TUPLES_OK) {
        if (PQntuples(result) > 0) {
            printf("User: %s\n", PQgetvalue(result, 0, 0));
        }
    }
    PQclear(result);
}

// Query with parameters
const char *params[] = {"john@example.com"};
pquv_queue(pg, 
           "SELECT username FROM users WHERE email = $1",
           1,
           params,
           on_user_result,
           NULL);
pquv_execute(pg);
```

### Multiple Queries

```c
// Queue multiple queries - they execute sequentially
pquv_queue(pg, "BEGIN", 0, NULL, NULL, NULL);
pquv_queue(pg, "INSERT INTO users (name) VALUES ('Alice')", 0, NULL, on_insert, NULL);
pquv_queue(pg, "INSERT INTO users (name) VALUES ('Bob')", 0, NULL, on_insert, NULL);
pquv_queue(pg, "COMMIT", 0, NULL, NULL, NULL);

pquv_execute(pg);
```

### With User Data

```c
typedef struct {
    int user_id;
    char *username;
} UserContext;

void on_user_posts(pg_async_t *pg, PGresult *result, void *data) {
    UserContext *ctx = (UserContext*)data;
    printf("Posts for user %d (%s):\n", ctx->user_id, ctx->username);
    
    // Process results...
    
    free(ctx->username);
    free(ctx);
    PQclear(result);
}

UserContext *ctx = malloc(sizeof(UserContext));
ctx->user_id = 123;
ctx->username = strdup("john");

const char *params[] = {"123"};
pquv_queue(pg,
           "SELECT * FROM posts WHERE user_id = $1",
           1,
           params,
           on_user_posts,
           ctx);
pquv_execute(pg);
```

## API Reference

### Initialization

```c
pg_async_t* pquv_create(PGconn *existing_conn, void *data);
```

Creates an async PostgreSQL context using an existing connection.

**Parameters:**
- `existing_conn`: Existing PGconn connection (must be connected)
- `data`: User data pointer (optional)

**Returns:** pg_async_t context or NULL on error

### Query Queueing

```c
int pquv_queue(pg_async_t *pg,
               const char *sql,
               int param_count,
               const char **params,
               pg_result_cb_t result_cb,
               void *query_data);
```

Queues a query for execution.

**Parameters:**
- `pg`: Async context
- `sql`: SQL query string (can use $1, $2, etc. for parameters)
- `param_count`: Number of parameters
- `params`: Array of parameter values (strings)
- `result_cb`: Callback for results (can be NULL)
- `query_data`: User data for this query (can be NULL)

**Returns:** 0 on success, -1 on error

### Query Execution

```c
int pquv_execute(pg_async_t *pg);
```

Starts executing queued queries.

**Parameters:**
- `pg`: Async context

**Returns:** 0 on success, -1 on error

### Callback Type

```c
typedef void (*pg_result_cb_t)(pg_async_t *pg, PGresult *result, void *data);
```

**Parameters:**
- `pg`: Async context
- `result`: PostgreSQL result (must be freed with PQclear)
- `data`: User data passed to pquv_queue

## Platform Differences

### Unix/Linux/macOS
Uses `uv_poll_t` to monitor the PostgreSQL socket for readability/writability.

### Windows
Uses `uv_timer_t` to periodically check query status (PostgreSQL doesn't provide a socket handle on Windows).

## Error Handling

Check result status in callbacks:

```c
void on_result(pg_async_t *pg, PGresult *result, void *data) {
    ExecStatusType status = PQresultStatus(result);
    
    switch (status) {
        case PGRES_TUPLES_OK:
            // SELECT query succeeded
            break;
        case PGRES_COMMAND_OK:
            // INSERT/UPDATE/DELETE succeeded
            break;
        default:
            fprintf(stderr, "Query error: %s\n", PQresultErrorMessage(result));
            break;
    }
    
    PQclear(result);
}
```

## Connection Management

pquv uses an existing PGconn connection. You're responsible for:
- Creating the connection
- Keeping it alive
- Closing it when done

```c
// Create connection
PGconn *conn = PQconnectdb("host=localhost dbname=mydb");

// Use with pquv
pg_async_t *pg = pquv_create(conn, NULL);

// ... use pquv ...

// Cleanup (pquv doesn't close the connection)
PQfinish(conn);
```

## Thread Safety

pquv is **not thread-safe**. Each thread should have its own pg_async_t instance and PGconn connection.

## Performance Tips

1. **Batch queries**: Queue multiple queries before calling pquv_execute
2. **Use parameters**: Parameterized queries are safer and can be faster
3. **Connection pooling**: Reuse connections across requests
4. **Prepared statements**: Use PostgreSQL prepared statements for repeated queries

## Example: Web Handler

```c
void get_users_handler(Req *req, Res *res) {
    pg_async_t *pg = pquv_create(db_conn, res);
    
    pquv_queue(pg,
               "SELECT id, username, email FROM users LIMIT 100",
               0,
               NULL,
               on_users_result,
               res);
    
    pquv_execute(pg);
}

void on_users_result(pg_async_t *pg, PGresult *result, void *data) {
    Res *res = (Res*)data;
    
    if (PQresultStatus(result) == PGRES_TUPLES_OK) {
        // Build JSON response
        char *json = build_json_from_result(result);
        send_json(res, 200, json);
        free(json);
    } else {
        send_error(res, 500, "Database error");
    }
    
    PQclear(result);
}
```

## Dependencies

- libpq (PostgreSQL client library)
- libuv (event loop)

## License

Same as parent project.
