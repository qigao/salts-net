# TLV Bind - Dynamic TLV Codec

Runtime TLV (Tag-Length-Value) parser/builder with zero compilation overhead.

## Features

- **Dynamic Loading**: Load `.tlvschema` files at runtime, no code generation needed
- **Zero Compilation**: End users don't need compilers
- **Simple API**: Compatible with DataBind for easy integration
- **Flexible**: Works with any Value representation (cJSON, msgpack, custom structs)

## Quick Start

### 1. Define Schema

```
# order.tlvschema
message Order {
    int32   order_id
    string  symbol
    double  price
    int32   quantity
}
```

Field numbers are auto-assigned based on declaration order (1, 2, 3, ...).

### 2. Implement Value API

```c
TlvBindValueApi api = {
    .create_object = my_create_object,
    .set_field_int32 = my_set_int32,
    // ... other callbacks
};
```

### 3. Parse TLV Data

```c
TlvBind* codec = tlv_bind_create("order.tlvschema", &api);
Value* order = tlv_bind_parse(codec, "Order", tlv_data, len);
```

## TLV Encoding Format

```
┌─────────┬─────────┬─────────┐
│  Tag    │ Length  │  Value  │
│ varint  │ varint  │  bytes  │
└─────────┴─────────┴─────────┘
```

- **Tag**: `(field_number << 3) | wire_type`
- **Length**: Number of bytes in value
- **Value**: Raw bytes

## Supported Types

- `int32`: 32-bit integer (4 bytes)
- `int64`: 64-bit integer (8 bytes)
- `double`: 64-bit float (8 bytes)
- `string`: UTF-8 string (variable length)
- `bytes`: Raw binary data (variable length)
- Nested messages

## Build

```bash
mkdir build && cd build
cmake ..
make tlv_dynamic_example
./examples/tlv_dynamic_example
```

## Comparison with TLV Parser

TLV Bind and TLV Parser serve different purposes:

| Feature | TLV Bind | TLV Parser |
|---------|----------|------------|
| Format | Dynamic TLV (tag-length-value) | Fixed frame format |
| Schema | Runtime schema files | Compile-time fixed |
| Nested messages | ✅ Supported | ❌ Not supported |
| Performance (small) | 1.8M parses/s | 9.2M parses/s |
| Performance (large) | 300K parses/s | 117K parses/s |
| Use case | Flexible protocols (like Protobuf) | High-performance frame parsing |

Choose TLV Bind when:
- You need dynamic message formats
- You want schema-driven development
- You need nested message support
- 1.8M ops/s is sufficient

Choose TLV Parser when:
- You have a fixed frame format
- You need maximum performance (9M+ ops/s)
- You can manually pack/unpack payloads
- Zero-copy is critical

## Performance

Benchmark results (optimized with O(1) field lookup and dynamic buffer):

```
TLV Bind Performance
benchmark                            iters      avg(us)      ops/s
---------                            -----      -------      -----
build Order message                 100000        0.311    3,217,907
parse Order message                 100000        0.560    1,785,316
round-trip Order                     50000        0.848    1,179,251
parse nested Trade                   50000        3.582      279,204
```

Key optimizations:
- **O(1) field lookup**: Hash table (`field_map[256]`) eliminates linear search
- **Zero-copy parsing**: Strings/bytes point directly to input buffer
- **Dynamic buffer**: Single-pass build with on-demand growth
- **Efficient varint encoding**: Compact tag and length representation

Performance characteristics:
- Simple messages: 3.2M builds/s, 1.8M parses/s
- Nested messages: 279K parses/s
- Comparable to Protobuf C++ implementation
- No external dependencies or code generation required

## Roadmap

- [x] Phase 1: Schema parser + dynamic parse (read-only)
- [x] Phase 2: Dynamic build (write)
- [x] Phase 3: Zero-copy optimization
- [x] Phase 4: O(1) field lookup optimization
- [ ] Phase 5: Static code generator (optional)
- [ ] Phase 6: MIR optimization (optional)

## Comparison with DataBind

| Feature | DataBind | TlvBind |
|---------|----------|---------|
| Format | Protobuf-like | TLV |
| Complexity | High | Low |
| Unknown fields | Needs schema | Auto-skip via Length |
| Use case | Complex protocols | Simple protocols |
