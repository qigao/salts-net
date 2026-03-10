# TBE Parser & Code Generator

A high-performance Simple Binary Encoding (TBE) schema parser and code generator for C/C++.

## Features

- **Zero-Copy Design**: Direct buffer access without data copying
- **Type Safety**: Compile-time type checking
- **Multi-Language**: Generate C, Python, or Rust code
- **High Performance**: Inline functions, minimal overhead
- **Memory Safe**: All allocations checked, errors propagated
- **Detailed Errors**: Line numbers and context for parse errors

## Understanding Zero-Copy

**Key Concept:** There is no "binary" vs "struct" representation. Only binary data exists.

```
┌─────────────────────────┐
│  buffer (binary data)   │  ← The ONLY data
└─────────────────────────┘
         ↑
         │ Points to it (no copy)
         │
    ┌────┴────┐
    │  View   │  ← Just a reading tool
    └─────────┘
```

**What this means:**
- No serialization/deserialization
- No data copying
- Binary format IS the in-memory format
- Network data can be read directly

## Quick Start

### Installation

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest  # Run tests
```

### Example Schema

Create `hello.schema`:
```
schema Hello [id(1), version(1), byte_order(little)];

message Greeting {
    uint32 timestamp;
    string message;
}
```

### Generate Code

```bash
tbe_compiler --schema hello.schema --lang c --output hello.h
```

### Use Generated Code

```c
#include "hello.h"

// Encode (write directly to buffer)
uint8_t buffer[1024];
Greeting_builder_t builder;
Greeting_builder_bind(&builder, buffer, sizeof(buffer));
Greeting_builder_timestamp_set(&builder, 1234567890);
Greeting_builder_message_set(&builder, "Hello, World!", 13);

// Decode (read directly from buffer)
Greeting_view_t view;
Greeting_view_bind(&view, buffer, sizeof(buffer));
uint32_t ts = Greeting_view_timestamp_get(&view);

tbe_var_data_t msg;
if (Greeting_message(&view, &msg)) {
    printf("Message: %.*s\n", (int)msg.size, (const char*)msg.data);
}
```

### Real-World Example: Network I/O

```c
// Receive from network → use directly (zero-copy)
uint8_t buffer[1024];
recv(socket, buffer, 1024, 0);

Message_view_t view;
Message_view_bind(&view, buffer, 1024);
uint32_t price = Message_view_price_get(&view);  // Read directly from buffer

// Modify in-place
Message_builder_t builder;
Message_builder_bind(&builder, buffer, 1024);
Message_builder_quantity_set(&builder, 100);  // Write directly to buffer

// Send modified data
send(socket, buffer, 1024, 0);
```

## API Reference

### Core Functions

#### `parse_schema()`
```c
int parse_schema(const char *text, size_t len, Node *root, tbe_error_t *err);
```
Parse a schema text into a Node tree.

**Parameters:**
- `text`: NUL-terminated schema text
- `len`: Length of schema text (excluding NUL)
- `root`: Pre-created NODE_MAP to receive parsed nodes
- `err`: Optional error structure (can be NULL)

**Returns:** 0 on success, -1 on error

#### Node Creation
```c
Node *create_node_string(const char *name, const char *val);
Node *create_node_list(const char *name);
Node *create_node_map(const char *name);
```
Create nodes for building schema trees. Returns NULL on allocation failure.

#### Node Manipulation
```c
int list_add(Node *list, Node *item);
int map_add(Node *map, Node *item);
void node_free(Node *node);
```
Add items to lists/maps and free nodes. Returns 0 on success, -1 on allocation failure.

### Error Handling

```c
typedef enum {
    TBE_OK = 0,
    TBE_ERR_INVALID_ARGUMENT,
    TBE_ERR_OUT_OF_MEMORY,
    TBE_ERR_LEXER_ERROR,
    TBE_ERR_SYNTAX_ERROR,
    TBE_ERR_SEMANTIC_ERROR,
    TBE_ERR_IO_ERROR
} tbe_error_code_t;

typedef struct {
    tbe_error_code_t code;
    int line;
    int column;
    char message[256];
} tbe_error_t;
```

### Version Information

```c
const char *tbe_version(void);  // Returns "1.0.0"
void tbe_version_components(int *major, int *minor, int *patch);
```

## Thread Safety

**This library is NOT thread-safe.**

- The parser maintains internal state and does not use locking
- Use separate Node trees for each thread, OR
- Serialize access with external locking
- Parsed Node trees can be safely read from multiple threads (read-only)

## Schema Syntax

### Composites
```
composite Point {
    int32 x;
    int32 y;
}
```

### Enums
```
enum Side <uint8> {
    Buy = 1;
    Sell = 2;
}
```

### Messages
```
[id(100), version(1)]
message Quote {
    Side side;
    uint32 qty;
    uint64 price;
    string symbol;
}
```

### Groups
```
group Level {
    uint64 price;
    uint32 qty;
}

message BookSnapshot {
    group<Level> bids;
    group<Level> asks;
}
```

### Supported Types

**Primitive types:**
- `uint8`, `uint16`, `uint32`, `uint64`
- `int8`, `int16`, `int32`, `int64`
- `float`, `double`
- `byte`

**Complex types:**
- `bytes` - variable-length binary data
- `bytes(N)` - fixed-length binary data
- `string` - variable-length UTF-8 string
- `Type[N]` - fixed-length array

## Building

```bash
mkdir build && cd build
cmake ..
cmake --build .
ctest  # Run tests
```

## License

See LICENSE file for details.
