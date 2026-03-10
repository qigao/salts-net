# TBE Parser & Code Generator - User Guide

## Table of Contents
- [Introduction](#introduction)
- [Quick Start](#quick-start)
- [Schema Language](#schema-language)
- [Using Generated Code](#using-generated-code)
- [API Reference](#api-reference)
- [Examples](#examples)
- [Troubleshooting](#troubleshooting)

---

## Introduction

The TBE (Simple Binary Encoding) Parser is a high-performance schema compiler and code generator for binary message encoding. It generates zero-copy, type-safe code for encoding and decoding binary messages.

### Key Features

- **Zero-Copy Design**: Direct buffer access without data copying
- **Type Safety**: Compile-time type checking
- **Multi-Language**: Generate C, Python, or Rust code
- **High Performance**: Inline functions, minimal overhead
- **Standards Compliant**: Compatible with TBE specification

### When to Use TBE

✅ **Good for:**
- Low-latency trading systems
- High-throughput message processing
- Fixed-schema binary protocols
- Memory-constrained environments

❌ **Not ideal for:**
- Schema evolution (use Protobuf/Avro instead)
- Self-describing messages
- Human-readable formats

---

## Quick Start

### Installation

```bash
# Build from source
mkdir build && cd build
cmake ..
cmake --build .
ctest  # Run tests
```

### Your First Schema

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

// Encode
uint8_t buffer[1024];
Greeting_builder_t builder;
Greeting_builder_bind(&builder, buffer, sizeof(buffer));
Greeting_builder_timestamp_set(&builder, 1234567890);
Greeting_builder_message_set(&builder, "Hello, World!", 13);

// Decode
Greeting_view_t view;
Greeting_view_bind(&view, buffer, sizeof(buffer));
uint32_t ts = Greeting_view_timestamp_get(&view);
tbe_var_data_t msg;
if (Greeting_view_message(&view, &msg)) {
    printf("Message: %.*s\n", (int)msg.size, msg.data);
}
```

---

## Schema Language

### Basic Syntax

#### Schema Declaration
```
schema <Name> [<attributes>];
```

**Attributes:**
- `id(N)` - Schema identifier
- `version(N)` - Schema version
- `byte_order(little|big)` - Byte order

**Example:**
```
schema Market [id(7), version(2), byte_order(little)];
```

---

### Type Definitions

#### Enums
```
[<attributes>]
enum <Name> <<underlying_type>> {
    <Item1> = <value1>;
    <Item2> = <value2>;
}
```

**Example:**
```
[id(1)]
enum Side <uint8> {
    Buy = 1;
    Sell = 2;
}
```

**Supported underlying types:**
- `uint8`, `uint16`, `uint32`, `uint64`
- `int8`, `int16`, `int32`, `int64`

---

#### Flags (Bit Fields)
```
[<attributes>]
flags <Name> <<underlying_type>> {
    <Flag1> = <value1>;
    <Flag2> = <value2>;
}
```

**Example:**
```
[id(2)]
flags OrderFlags <uint8> {
    IOC = 1;        // Immediate or Cancel
    FOK = 2;        // Fill or Kill
    PostOnly = 4;   // Post Only
    Hidden = 8;     // Hidden order
}
```

**Auto-increment:**
Flags auto-increment as powers of 2:
```
flags Permissions {
    Read;      // = 1
    Write;     // = 2
    Execute;   // = 4
    Delete;    // = 8
}
```

**Generated helper functions:**
```c
// Check if flag is set
bool OrderFlags_has(OrderFlags_t flags, OrderFlags_t flag);

// Set flag
OrderFlags_t OrderFlags_set(OrderFlags_t flags, OrderFlags_t flag);

// Clear flag
OrderFlags_t OrderFlags_clear(OrderFlags_t flags, OrderFlags_t flag);

// Toggle flag
OrderFlags_t OrderFlags_toggle(OrderFlags_t flags, OrderFlags_t flag);
```

**Usage:**
```c
// Set multiple flags
OrderFlags_t flags = OrderFlags_IOC | OrderFlags_PostOnly;

// Check flag
if (OrderFlags_has(flags, OrderFlags_IOC)) {
    // Handle IOC order
}

// Set flag
flags = OrderFlags_set(flags, OrderFlags_Hidden);

// Clear flag
flags = OrderFlags_clear(flags, OrderFlags_PostOnly);
```

---

#### Composites (Fixed-Size Structures)
```
[<attributes>]
composite <Name> {
    <type> <field_name>;
    ...
}
```

**Example:**
```
[id(10)]
composite Header {
    uint32 seq_num;
    uint64 timestamp;
}
```

**Rules:**
- All fields must be fixed-size
- No variable-length fields (string/bytes)
- Can contain other composites

---

#### Groups (Repeating Groups)
```
[<attributes>]
group <Name> {
    <type> <field_name>;
    ...
}
```

**Example:**
```
[id(20)]
group Level {
    uint64 price;
    uint32 quantity;
}
```

**Rules:**
- All fields must be fixed-size
- Used in messages with `group<Name>` syntax

---

#### Messages
```
[<attributes>]
message <Name> {
    <fixed_fields>
    <group_fields>
    <var_data_fields>
}
```

**Example:**
```
[id(100), version(1)]
message BookSnapshot {
    Header header;              // Fixed field
    uint8[16] digest;           // Fixed array
    group<Level> bids;          // Group field
    string symbol;              // Variable data
    bytes payload;              // Variable data
}
```

**Field Order Rules:**
1. **Fixed fields** - Primitives, composites, fixed arrays
2. **Group fields** - Repeating groups
3. **Variable data** - strings, bytes

⚠️ **Order is mandatory!** Fields must be declared in this order.

---

### Primitive Types

| Type | Size | Description |
|------|------|-------------|
| `uint8`, `byte` | 1 | Unsigned 8-bit |
| `int8` | 1 | Signed 8-bit |
| `uint16` | 2 | Unsigned 16-bit |
| `int16` | 2 | Signed 16-bit |
| `uint32` | 4 | Unsigned 32-bit |
| `int32` | 4 | Signed 32-bit |
| `uint64` | 8 | Unsigned 64-bit |
| `int64` | 8 | Signed 64-bit |
| `float` | 4 | IEEE 754 single |
| `double` | 8 | IEEE 754 double |

### Complex Types

| Type | Description | Example |
|------|-------------|---------|
| `Type[N]` | Fixed array | `uint32[10] values;` |
| `bytes(N)` | Fixed bytes | `bytes(16) hash;` |
| `bytes` | Variable bytes | `bytes payload;` |
| `string` | Variable string | `string username;` |
| `group<T>` | Repeating group | `group<Level> bids;` |

---

## Using Generated Code

### Understanding Zero-Copy Design

**Core Principle:** TBE does not have separate "binary" and "struct" representations. There is only one representation: **binary data in a buffer**.

View and Builder are not data containers - they are **tools** that let you read/write the buffer directly.

```
┌─────────────────────────┐
│  buffer (binary data)   │  ← The ONLY real data
└─────────────────────────┘
         ↑
         │ Points to it (no copy)
         │
    ┌────┴────┐
    │  View   │  ← Just a pointer + metadata
    └─────────┘
```

**What this means:**
- No serialization/deserialization step
- No data copying between formats
- Binary format IS the in-memory format
- Reading/writing happens directly on the buffer

**Example flow:**
```c
// 1. Network receives data
uint8_t buffer[1024];
recv(socket, buffer, 1024, 0);

// 2. Bind view (zero-copy, just records pointer)
Message_view view;
Message_view_bind(&view, buffer, 1024);

// 3. Read fields (directly from buffer)
uint32_t price = Message_view_price(&view);

// 4. Modify (bind builder to same buffer)
Message_builder builder;
Message_builder_bind(&builder, buffer, 1024);
Message_builder_set_quantity(&builder, 100);

// 5. Send (buffer was modified in-place)
send(socket, buffer, 1024, 0);
```

**No "switching" between binary and struct - they are the same thing.**

---

### C Language

#### View (Read-Only)

Views provide read-only access to binary data. All returned pointers point directly into the buffer (zero-copy).

##### Basic Usage

```c
// Bind to buffer
Message_view view;
Message_view_bind(&view, buffer, buffer_size);

// Read numeric fields (returns value)
uint32_t order_id = Message_view_order_id(&view);
float ratio = Message_view_ratio(&view);
double amount = Message_view_amount(&view);

// Read enum fields (returns enum value)
Side_t side = Message_view_side(&view);
if (side == Side_Buy) {
    // ...
}
```

##### Reading Different Field Types

**1. Numeric Fields (uint32, int64, float, double)**
```c
// Schema
message Order {
    uint64 order_id;
    uint32 price;
    float ratio;
    double amount;
}

// Generated API - returns value directly
uint64_t id = Order_view_order_id(&view);
uint32_t price = Order_view_price(&view);
float ratio = Order_view_ratio(&view);
double amount = Order_view_amount(&view);
```

**2. Enum Fields**
```c
// Schema
enum Side <uint8> { Buy = 1; Sell = 2; }
message Order {
    Side side;
}

// Generated API - returns enum value
Side_t side = Order_view_side(&view);
```

**3. Composite Fields (nested structures)**
```c
// Schema
composite Header {
    uint32 seq_num;
    uint64 timestamp;
}
message Order {
    Header header;
}

// Generated API - returns nested View
Header_view hdr = Order_view_header(&view);
uint32_t seq = Header_view_seq_num(&hdr);
uint64_t ts = Header_view_timestamp(&hdr);
```

**4. Fixed Arrays**
```c
// Schema
message Data {
    bytes(16) digest;      // Fixed 16 bytes
    uint32[10] values;     // Fixed 10 integers
}

// Generated API - returns pointer (zero-copy!)
const uint8_t *digest = Data_view_digest(&view);
for (size_t i = 0; i < 16; i++) {
    printf("%02x", digest[i]);
}

const uint32_t *values = Data_view_values(&view);
for (size_t i = 0; i < 10; i++) {
    printf("%u ", values[i]);
}
```

**5. Composite Arrays**
```c
// Schema
composite Point { int32 x; int32 y; }
message Data {
    Point[10] points;
}

// Generated API - access by index
size_t count = Data_view_points_length(&view);  // Returns 10
for (size_t i = 0; i < count; i++) {
    Point_view p = Data_view_points_at(&view, i);
    int32_t x = Point_view_x(&p);
    int32_t y = Point_view_y(&p);
}
```

**6. Variable-Length Strings**
```c
// Schema
message Order {
    string symbol;
}

// Generated API - returns pointer + length
size_t len;
const char *symbol = Order_view_symbol(&view, &len);

// ⚠️ WARNING: String may NOT be null-terminated!
// Use length-aware printing:
printf("Symbol: %.*s\n", (int)len, symbol);

// ❌ WRONG: Don't use %s without length
// printf("%s\n", symbol);  // May read past end!
```

**7. Variable-Length Bytes**
```c
// Schema
message Data {
    bytes payload;
}

// Generated API - returns pointer + length
size_t len;
const uint8_t *payload = Data_view_payload(&view, &len);
process_data(payload, len);
```

**8. Multiple Variable-Length Fields**
```c
// Schema
message Trade {
    uint64 trade_id;
    string symbol;    // First var-data
    string venue;     // Second var-data
    bytes metadata;   // Third var-data
}

// Usage - access in order
size_t symbol_len, venue_len, meta_len;
const char *symbol = Trade_view_symbol(&view, &symbol_len);
const char *venue = Trade_view_venue(&view, &venue_len);
const uint8_t *meta = Trade_view_metadata(&view, &meta_len);
```

#### Builder (Write)

Builders provide write access to binary data. All writes happen directly to the buffer (zero-copy).

##### Basic Usage

```c
// Bind to buffer
Message_builder builder;
Message_builder_bind(&builder, buffer, buffer_size);

// Write numeric fields
Message_builder_set_order_id(&builder, 12345);
Message_builder_set_price(&builder, 100);
Message_builder_set_ratio(&builder, 1.5f);

// Write enum fields
Message_builder_set_side(&builder, Side_Buy);
```

##### Writing Different Field Types

**1. Numeric Fields**
```c
Order_builder_set_order_id(&builder, 12345);
Order_builder_set_price(&builder, 100);
Order_builder_set_ratio(&builder, 1.5f);
Order_builder_set_amount(&builder, 1000.0);
```

**2. Enum Fields**
```c
Order_builder_set_side(&builder, Side_Buy);
```

**3. Composite Fields**
```c
// Schema
composite Header {
    uint32 seq_num;
    uint64 timestamp;
}
message Order {
    Header header;
}

// Generated API - get nested builder
Header_builder hdr = Order_builder_header(&builder);
Header_builder_set_seq_num(&hdr, 123);
Header_builder_set_timestamp(&hdr, 1234567890);
```

**4. Fixed Arrays**
```c
// Schema
message Data {
    bytes(16) digest;
    uint32[10] values;
}

// Write byte array
uint8_t digest[16] = {0x01, 0x02, ...};
Data_builder_set_digest(&builder, digest);

// Write integer array
uint32_t values[10] = {1, 2, 3, ...};
Data_builder_set_values(&builder, values);
```

**5. Composite Arrays**
```c
// Schema
composite Point { int32 x; int32 y; }
message Data {
    Point[10] points;
}

// Write each element
for (size_t i = 0; i < 10; i++) {
    Point_builder_t p;
    Data_builder_points_builder_at(&builder, i, &p);
    Point_builder_x_set(&p, x_values[i]);
    Point_builder_y_set(&p, y_values[i]);
}
```

**6. Variable-Length Strings**
```c
// Schema
message Order {
    string symbol;
}

// Write string (no null terminator needed)
const char *symbol = "AAPL";
Order_builder_symbol_set(&builder, symbol, strlen(symbol));

// Or with explicit length
Order_builder_symbol_set(&builder, "AAPL", 4);
```

**7. Variable-Length Bytes**
```c
// Schema
message Data {
    bytes payload;
}

// Write bytes
uint8_t payload[100] = {0};
Data_builder_payload_set(&builder, payload, 100);
```

**8. Multiple Variable-Length Fields**
```c
// Schema
message Trade {
    uint64 trade_id;
    string symbol;
    string venue;
    bytes metadata;
}

// Write in order (must follow schema order!)
Trade_builder_trade_id_set(&builder, 12345);
Trade_builder_symbol_set(&builder, "AAPL", 4);
Trade_builder_venue_set(&builder, "NYSE", 4);
Trade_builder_metadata_set(&builder, meta_data, meta_len);
```

#### Group Cursors (Iteration)

Groups are repeating blocks of fixed-size fields. Use cursors to iterate.

##### Reading Groups

```c
// Schema
group Level {
    uint64 price;
    uint32 quantity;
}
message BookSnapshot {
    group<Level> bids;
}

// Read groups
Level_cursor_t cursor;
Level_view_t item;
if (BookSnapshot_bids_cursor(&view, &cursor)) {
    while (Level_cursor_next(&cursor, &item)) {
        uint64_t price = Level_view_price_get(&item);
        uint32_t qty = Level_view_quantity_get(&item);
        printf("Bid: %lu @ %u\n", price, qty);
    }
}
```

##### Writing Groups

```c
// Reserve space for N items
Level_builder_cursor_t cursor;
BookSnapshot_bids_builder_begin(&builder, 10, &cursor);

// Write each item
Level_builder_t level;
while (Level_builder_cursor_next(&cursor, &level)) {
    Level_builder_price_set(&level, prices[i]);
    Level_builder_quantity_set(&level, quantities[i]);
}
```

---

### Complete Example: All Field Types

```c
// Schema
composite Header {
    uint32 seq_num;
    uint64 timestamp;
}

enum Side <uint8> { Buy = 1; Sell = 2; }

group Level {
    uint64 price;
    uint32 quantity;
}

message Order {
    Header header;           // Composite
    uint64 order_id;         // Numeric
    Side side;               // Enum
    uint32 price;            // Numeric
    bytes(16) digest;        // Fixed array
    group<Level> levels;     // Group
    string symbol;           // Variable string
    bytes metadata;          // Variable bytes
}

// Encoding
uint8_t buffer[4096];
Order_builder_t builder;
Order_builder_bind(&builder, buffer, sizeof(buffer));

// 1. Composite
Header_builder_t hdr;
Order_header_builder(&builder, &hdr);
Header_builder_seq_num_set(&hdr, 123);
Header_builder_timestamp_set(&hdr, 1234567890);

// 2. Numeric
Order_builder_order_id_set(&builder, 12345);
Order_builder_price_set(&builder, 100);

// 3. Enum
Order_builder_side_set(&builder, Side_Buy);

// 4. Fixed array
uint8_t digest[16] = {0x01, 0x02};
Order_builder_digest_set(&builder, digest, 16);

// 5. Group
Level_builder_cursor_t cursor;
Order_levels_builder_begin(&builder, 3, &cursor);
Level_builder_t lvl;
while (Level_builder_cursor_next(&cursor, &lvl)) {
    Level_builder_price_set(&lvl, prices[lvl.current_index-1]);
    Level_builder_quantity_set(&lvl, quantities[lvl.current_index-1]);
}

// 6. Variable string
Order_builder_symbol_set(&builder, "AAPL", 4);

// 7. Variable bytes
Order_builder_metadata_set(&builder, meta, meta_len);

// Decoding
Order_view_t view;
Order_view_bind(&view, buffer, sizeof(buffer));

// 1. Composite
Header_view_t hdr_v;
Order_header(&view, &hdr_v);
uint32_t seq = Header_view_seq_num_get(&hdr_v);

// 2. Numeric
uint64_t order_id = Order_view_order_id_get(&view);

// 3. Enum
Side_t side = Order_view_side_get(&view);

// 4. Fixed array
const uint8_t *digest_v = Order_view_digest_ptr(&view);

// 5. Group
Level_cursor_t cursor;
Level_view_t item;
if (Order_levels_cursor(&view, &cursor)) {
    while (Level_cursor_next(&cursor, &item)) {
        uint64_t price = Level_view_price_get(&item);
        uint32_t qty = Level_view_quantity_get(&item);
    }
}

// 6. Variable string
tbe_var_data_t symbol_data;
if (Order_symbol(&view, &symbol_data)) {
    printf("Symbol: %.*s\n", (int)symbol_data.size, symbol_data.data);
}

// 7. Variable bytes
tbe_var_data_t meta_data;
Order_metadata(&view, &meta_data);
```

---

### Common Pitfalls and Best Practices

#### ⚠️ Pitfall 1: String Null Termination

**Problem:** Variable-length strings are NOT null-terminated.

```c
// ❌ WRONG
tbe_var_data_t symbol;
Order_symbol(&view, &symbol);
printf("%s\n", (const char*)symbol.data);  // May read past end!

// ✅ CORRECT
tbe_var_data_t symbol;
if (Order_symbol(&view, &symbol)) {
    printf(".*s\n", (int)symbol.size, (const char*)symbol.data);
    if (symbol.size == 4 && memcmp(symbol.data, "AAPL", 4) == 0) {
        // Safe comparison
    }
}
```

#### ⚠️ Pitfall 2: Pointer Lifetime

**Problem:** View/Builder pointers point into the buffer. If buffer is freed, pointers become invalid.

```c
// ❌ WRONG
const char* get_symbol(const uint8_t *buffer, size_t size) {
    Order_view_t view;
    Order_view_bind(&view, buffer, size);
    tbe_var_data_t symbol;
    Order_symbol(&view, &symbol);
    return (const char*)symbol.data;  // Pointer to buffer
}
// If caller frees buffer, returned pointer is dangling!

// ✅ CORRECT - Option 1: Use within buffer lifetime
void process_order(const uint8_t *buffer, size_t size) {
    Order_view_t view;
    Order_view_bind(&view, buffer, size);
    tbe_var_data_t symbol;
    if (Order_symbol(&view, &symbol)) {
        // Use symbol here while buffer is valid
        printf("%.*s\n", (int)symbol.size, (const char*)symbol.data);
    }
}

// ✅ CORRECT - Option 2: Copy if needed
char* get_symbol_copy(const uint8_t *buffer, size_t size) {
    Order_view_t view;
    Order_view_bind(&view, buffer, size);
    tbe_var_data_t symbol;
    if (!Order_symbol(&view, &symbol)) return NULL;

    char *copy = malloc(symbol.size + 1);
    memcpy(copy, symbol.data, symbol.size);
    copy[symbol.size] = '\0';
    return copy;  // Caller must free
}
```

#### ⚠️ Pitfall 3: Variable Data Access Order

**Problem:** Accessing variable-length fields out of order is inefficient (but still works).

```c
// Schema
message Trade {
    string symbol;   // First
    string venue;    // Second
    bytes metadata;  // Third
}

// ⚠️ INEFFICIENT (but works)
tbe_var_data_t meta, sym;
Trade_metadata(&view, &meta);  // Skips symbol+venue
Trade_symbol(&view, &sym);      // Re-scans from start

// ✅ EFFICIENT - Access in schema order
tbe_var_data_t sym, venue, meta;
Trade_symbol(&view, &sym);
Trade_venue(&view, &venue);
Trade_metadata(&view, &meta);
```

#### ⚠️ Pitfall 4: Buffer Size

**Problem:** Buffer too small for message.

```c
// ❌ WRONG
uint8_t buffer[10];  // Too small!
Order_builder_t builder;
Order_builder_bind(&builder, buffer, 10);
Order_builder_order_id_set(&builder, 12345);  // May overflow!

// ✅ CORRECT - Calculate required size
// Fixed block size + group data + variable data
size_t required = Order_BLOCK_LENGTH
                + 4 + (num_levels * Level_BLOCK_LENGTH) // 4 byte dimension
                + 4 + symbol_len  // uint32 length prefix + data
                + 4 + meta_len;   // uint32 length prefix + data

uint8_t *buffer = malloc(required);
Order_builder_t builder;
Order_builder_bind(&builder, buffer, required);
```

#### ⚠️ Pitfall 5: Reusing Builders

**Problem:** Not resetting builder state between messages.

```c
// ❌ WRONG
Order_builder_t builder;
for (int i = 0; i < 100; i++) {
    // Reusing same builder without rebinding
    Order_builder_order_id_set(&builder, i);  // Undefined behavior!
}

// ✅ CORRECT
uint8_t buffer[1024];
Order_builder_t builder;
for (int i = 0; i < 100; i++) {
    Order_builder_bind(&builder, buffer, sizeof(buffer));  // Rebind each time
    Order_builder_order_id_set(&builder, i);
    // ... complete message ...
}
```

#### ✅ Best Practice 1: Stack Buffers for Small Messages

```c
// For messages with known max size
uint8_t buffer[1024];  // Stack allocation
Order_builder_t builder;
Order_builder_bind(&builder, buffer, sizeof(buffer));
```

#### ✅ Best Practice 2: Helper Functions for Complex Initialization

```c
// Don't write this everywhere
static void init_order_from_data(
    Order_builder *builder,
    const OrderData *data
) {
    Order_builder_order_id_set(builder, data->id);
    Order_builder_price_set(builder, data->price);
    Order_builder_side_set(builder, data->side);
    Order_builder_symbol_set(builder, data->symbol, data->symbol_len);
}

// Use it
Order_builder_t builder;
Order_builder_bind(&builder, buffer, size);
init_order_from_data(&builder, &my_data);
```

#### ✅ Best Practice 3: Validate Buffer Size Before Binding

```c
size_t min_size = Order_BLOCK_LENGTH;
if (buffer_size < min_size) {
    // Error: buffer too small
    return -1;
}

Order_view_t view;
Order_view_bind(&view, buffer, buffer_size);
```

---

### Python Language

```python
# Decode
view = GreetingView(buffer)
timestamp = view.timestamp()
message = view.message()

# Encode
builder = GreetingBuilder(buffer)
builder.set_timestamp(1234567890)
builder.set_message("Hello")
```

### Rust Language

```rust
// Decode
let view = GreetingView::new(&buffer)?;
let timestamp = view.timestamp();
let message = view.message();

// Encode
let mut builder = GreetingBuilder::new(&mut buffer)?;
builder.set_timestamp(1234567890);
builder.set_message("Hello");
```

---

## API Reference

### Core Functions

#### `parse_schema()`
```c
int parse_schema(const char *text, size_t len, Node *root, tbe_error_t *err);
```
Parse schema text into AST.

**Parameters:**
- `text` - Schema text (NUL-terminated)
- `len` - Length of text
- `root` - Pre-created NODE_MAP
- `err` - Optional error info (can be NULL)

**Returns:** 0 on success, -1 on error

---

### Wire Functions

#### Read Functions
```c
uint8_t  tbe_wire_read_u8(const uint8_t *data, int is_big_endian);
uint16_t tbe_wire_read_u16(const uint8_t *data, int is_big_endian);
uint32_t tbe_wire_read_u32(const uint8_t *data, int is_big_endian);
uint64_t tbe_wire_read_u64(const uint8_t *data, int is_big_endian);
float    tbe_wire_read_f32(const uint8_t *data, int is_big_endian);
double   tbe_wire_read_f64(const uint8_t *data, int is_big_endian);
```

#### Write Functions
```c
void tbe_wire_write_u8(uint8_t *data, int is_big_endian, uint8_t value);
void tbe_wire_write_u16(uint8_t *data, int is_big_endian, uint16_t value);
void tbe_wire_write_u32(uint8_t *data, int is_big_endian, uint32_t value);
void tbe_wire_write_u64(uint8_t *data, int is_big_endian, uint64_t value);
void tbe_wire_write_f32(uint8_t *data, int is_big_endian, float value);
void tbe_wire_write_f64(uint8_t *data, int is_big_endian, double value);
```

#### Variable Data
```c
bool tbe_wire_read_var_data(const uint8_t *data, size_t size,
                            int is_big_endian, tbe_var_data_t *out);

bool tbe_wire_write_var_data(uint8_t *data, size_t size,
                             int is_big_endian,
                             const void *value_data, size_t value_size);
```

---

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

const char *tbe_error_string(tbe_error_code_t code);
```

---

## Examples

### Example 1: Simple Message

**Schema:**
```
schema Trading [id(1), version(1), byte_order(little)];

enum Side <uint8> {
    Buy = 1;
    Sell = 2;
}

message Order {
    uint64 order_id;
    Side side;
    uint32 quantity;
    uint64 price;
}
```

**Usage:**
```c
// Encode
uint8_t buffer[64];
Order_builder_t builder;
Order_builder_bind(&builder, buffer, sizeof(buffer));
Order_builder_order_id_set(&builder, 12345);
Order_builder_side_set(&builder, Side_Buy);
Order_builder_quantity_set(&builder, 100);
Order_builder_price_set(&builder, 5000);

// Decode
Order_view_t view;
Order_view_bind(&view, buffer, sizeof(buffer));
uint64_t id = Order_view_order_id_get(&view);
Side_t side = Order_view_side_get(&view);
```

---

### Example 2: Market Data with Groups

**Schema:**
```
schema Market [id(2), version(1), byte_order(little)];

group Level {
    uint64 price;
    uint32 quantity;
}

message BookSnapshot {
    uint64 timestamp;
    group<Level> bids;
    group<Level> asks;
    string symbol;
}
```

**Usage:**
```c
// Encode
uint8_t buffer[4096];
BookSnapshot_builder_t builder;
BookSnapshot_builder_bind(&builder, buffer, sizeof(buffer));
BookSnapshot_builder_timestamp_set(&builder, 1234567890);

// Write bids
Level_builder_cursor_t cursor;
BookSnapshot_bids_builder_begin(&builder, 5, &cursor);
Level_builder_t level;
while (Level_builder_cursor_next(&cursor, &level)) {
    Level_builder_price_set(&level, bid_prices[cursor.current_index-1]);
    Level_builder_quantity_set(&level, bid_quantities[cursor.current_index-1]);
}

// Write symbol
BookSnapshot_builder_symbol_set(&builder, "AAPL", 4);

// Decode
BookSnapshot_view_t view;
BookSnapshot_view_bind(&view, buffer, sizeof(buffer));

Level_cursor_t cursor;
Level_view_t item;
if (BookSnapshot_bids_cursor(&view, &cursor)) {
    while (Level_cursor_next(&cursor, &item)) {
        uint64_t price = Level_view_price_get(&item);
        uint32_t qty = Level_view_quantity_get(&item);
        printf("Bid: %lu @ %u\n", price, qty);
    }
}
```

---

## Troubleshooting

### Common Errors

#### "Parse error at line X"
**Cause:** Syntax error in schema
**Solution:** Check schema syntax, ensure semicolons, braces match

#### "Failed to allocate"
**Cause:** Out of memory
**Solution:** Check system memory, reduce schema complexity

#### "Buffer too small"
**Cause:** Bind buffer smaller than message size
**Solution:** Increase buffer size or check message layout

#### "Fields must be ordered as fixed, group, var-data"
**Cause:** Incorrect field order in message
**Solution:** Reorder fields: fixed → groups → variable data

---

### Performance Tips

1. **Use stack buffers** for small messages
2. **Reuse builders/views** instead of recreating
3. **Avoid variable data** when possible (use fixed arrays)
4. **Batch group writes** instead of one-by-one
5. **Profile before optimizing** - measure first!

---

### Thread Safety

⚠️ **This library is NOT thread-safe.**

- Parser maintains internal state
- Use separate Node trees per thread, OR
- Serialize access with external locking
- Parsed trees are read-only safe

---

### Version Information

```c
const char *tbe_version(void);  // Returns "1.0.0"
void tbe_version_components(int *major, int *minor, int *patch);
```

---

## Support

- **Documentation**: See `DEVELOPER.md` for extending to new languages
- **Issues**: Report bugs on GitHub
- **Examples**: Check `examples/` directory

---

## License

See LICENSE file for details.
