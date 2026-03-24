# TBE Parser, Runtime Codec & Code Generator - Developer Guide

## Table of Contents
- [Architecture Overview](#architecture-overview)
- [Code Structure](#code-structure)
- [Adding a New Language](#adding-a-new-language)
- [Template System](#template-system)
- [Node Tree API](#node-tree-api)
- [Testing](#testing)
- [Contributing](#contributing)

---

## Architecture Overview

### Components

```
┌─────────────────────────────────────────────────────────┐
│                      schema input                         │
│  ┌──────────┐   ┌──────────┐   ┌──────────────────┐     │
│  │  Schema  │──▶│  Parser  │──▶│  Node Tree (AST) │     │
│  │   File   │   │ (re2c +  │   │                  │     │
│  │          │   │  lemon)  │   │  - schema        │     │
│  └──────────┘   └──────────┘   │  - messages      │     │
│                                 │  - composites    │     │
│                                 │  - groups        │     │
│                                 │  - enums         │     │
│                                 └────────┬─────────┘     │
│                                          │               │
│                                          ▼               │
│                                 ┌────────────────┐      │
│                                 │   Annotators   │      │
│                                 │  - Layouts     │      │
│                                 │  - Wire info   │      │
│                                 │  - Cursors     │      │
│                                 └──────┬────┬────┘      │
│                                        │    │           │
│                      codegen path      │    │ runtime   │
│                                        │    │ codec     │
│                                        ▼    ▼           │
│                               ┌──────────┐ ┌──────────┐ │
│                               │ Mustache │ │DataBind  │ │
│                               │Template  │ │/TLV Bind │ │
│                               └────┬─────┘ └────┬─────┘ │
│                                    │            │       │
│                                    ▼            ▼       │
│                               Generated     Runtime     │
│                                 Code        Parsing     │
└─────────────────────────────────────────────────────────┘
```

### Data Flow

1. **Lexer** (re2c) - Tokenizes schema text
2. **Parser** (lemon) - Builds Node tree (AST)
3. **Annotators** - Add metadata (offsets, sizes, wire info)
4. **Codegen path** (Mustache) - Generates source files
5. **Runtime path** (`data_bind`, `tlv_bind`) - Parses directly from schema metadata

---

## Code Structure

```
tbe/
├── tbe/                          # Core library
│   ├── include/                  # Public headers
│   │   ├── node_tree.h          # AST data structure
│   │   ├── schema_parser_dsl.h  # Parser API
│   │   ├── tbe_error.h          # Error handling
│   │   ├── tbe_version.h        # Version info
│   │   ├── tbe_wire.h           # Encode/decode functions
│   │   └── tbe_endian.h         # Endianness handling
│   ├── src/                      # Implementation
│   │   ├── node_tree.c          # AST implementation
│   │   ├── schema_parser_dsl.c  # Parser + annotators
│   │   ├── mustache_helpers.c   # Mustache data provider
│   │   ├── tbe_error.c          # Error handling
│   │   └── tbe_version.c        # Version info
│   ├── parser/                   # Parser generators
│   │   ├── schema_grammar.y     # Lemon grammar
│   │   ├── schema_lexer.re      # re2c lexer
│   │   ├── schema_lexer.h       # Lexer interface
│   │   └── schema_types.h       # Parser types
│   └── test/                     # Unit tests
│       └── test_sbe_parser.c
├── tbe_compiler/                 # Code generator
│   ├── main.c                    # CLI tool
│   ├── templates/                # Mustache templates
│   │   ├── c_structs.mustache   # C code generation
│   │   ├── python_dataclass.mustache
│   │   └── rust_structs.mustache
│   ├── example.schema            # Example schema
│   └── test_sbe_compiler.c       # Integration tests
├── data_bind/                    # MIR-based runtime parser
│   ├── data_bind.c               # Runtime parser + JIT/interpretive path
│   ├── tbe_helpers.c             # Runtime var-data helpers
│   └── test_data_bind.c          # Runtime parser tests
├── tlv_bind/                     # Custom TLV runtime codec
│   ├── tlv_bind.c                # Dynamic TLV parse/build
│   ├── tlv_schema_parser.c       # .tlvschema parser
│   └── tests/                    # TLV tests/benchmarks
├── README.md                     # Quick start
├── USER_GUIDE.md                 # User documentation
└── DEVELOPER.md                  # This file
```

### Two Different Runtime Models

- Generated C views/builders are zero-copy and operate directly on the buffer.
- `data_bind` is a runtime parser that builds host objects through callbacks.
- `tlv_bind` is a separate custom TLV runtime codec. It is not the same wire model as generated TBE views.

If you blur these together, you will design the wrong API and write garbage docs.

---

## Adding a New Language

### Step 1: Understand the Node Tree

The parser generates a Node tree with this structure:

```c
typedef enum {
    NODE_STRING,  // Leaf: string value
    NODE_LIST,    // Array of nodes
    NODE_MAP      // Key-value pairs
} turbo_node_type_t;

typedef struct turbo_node_s {
    turbo_node_type_t type;
    const char *name;  // Key name (for MAP items)
    union {
        char *string_val;
        struct { Node **items; size_t count; } list;
        struct { Node **items; size_t count; } map;
    } data;
} Node;
```

**Root structure:**
```
root (MAP)
├── schema (MAP)
│   ├── schema_name: "Market"
│   ├── attributes (MAP)
│   │   ├── id (MAP)
│   │   │   ├── name: "id"
│   │   │   └── value: "7"
│   │   └── version (MAP)
│   ├── wire_byte_order: "little"
│   └── is_little_endian: "1"
├── messages (LIST)
│   └── [0] (MAP)
│       ├── message_name: "Order"
│       ├── fixed_block_size: "16"
│       └── fields (LIST)
│           ├── [0] (MAP)
│           │   ├── name: "order_id"
│           │   ├── type: "uint64"
│           │   ├── offset: "0"
│           │   └── field_size_bytes: "8"
│           └── [1] (MAP)
│               ├── name: "price"
│               └── ...
├── composites (LIST)
├── groups (LIST)
└── enums (LIST)
```

---

### Step 2: Create a Mustache Template

Create `templates/<language>_structs.mustache`:

```mustache
{{!-- Header comment --}}
// Generated by TBE compiler v{{version}}

{{!-- Schema info --}}
{{#schema}}
// Schema: {{schema_name}}
{{#attributes}}
// {{name}}: {{value}}
{{/attributes}}
{{/schema}}

{{!-- Enums --}}
{{#enums}}
enum {{enum_name}} {
{{#items}}
    {{enum_name}}_{{name}} = {{value}}{{^last}},{{/last}}
{{/items}}
};
{{/enums}}

{{!-- Messages --}}
{{#messages}}
struct {{message_name}} {
{{#fields}}
{{#is_numeric}}
    {{type}} {{name}};  // offset: {{offset}}
{{/is_numeric}}
{{/fields}}
};
{{/messages}}
```

---

### Step 3: Understand Available Metadata

The annotators add rich metadata to each node:

#### Schema Node
```
schema (MAP)
├── schema_name: "Market"
├── wire_byte_order: "little" | "big"
├── is_little_endian: "1" | is_big_endian: "1"
└── attributes (MAP)
```

#### Message/Composite/Group Node
```
message (MAP)
├── message_name: "Order"
├── fixed_block_size: "16"  (if all fields are fixed)
├── has_fixed_block_size: "1"
├── wire_endian_const: "Market_WIRE_BIG_ENDIAN"
└── fields (LIST)
```

#### Field Node (Numeric)
```
field (MAP)
├── name: "price"
├── type: "uint64"
├── size_bytes: "8"
├── offset: "8"
├── has_offset: "1"
├── field_size_bytes: "8"
├── is_numeric: "1"
├── is_unsigned: "1"  (for unsigned types)
├── is_float: "1"     (for float/double)
└── wire_endian_const: "Market_WIRE_BIG_ENDIAN"
```

#### Field Node (Composite Reference)
```
field (MAP)
├── name: "header"
├── type: "Header"
├── is_user_defined: "1"
├── is_composite_ref: "1"
├── offset: "0"
└── field_size_bytes: "12"
```

#### Field Node (Enum Reference)
```
field (MAP)
├── name: "side"
├── type: "Side"
├── is_user_defined: "1"
├── is_enum_ref: "1"
├── enum_c_type: "Side_t"
├── enum_host_type: "uint8_t"
├── enum_wire_reader: "u8"
├── offset: "12"
└── field_size_bytes: "1"
```

#### Field Node (Fixed Array)
```
field (MAP)
├── name: "digest"
├── ctype: "COLLECTION"
├── is_collection: "1"
├── is_fixed_size: "1"
├── inner_type: "uint8"
├── length_field: "16"
├── element_size_bytes: "1"
├── field_size_bytes: "16"
├── collection_element_is_primitive: "1"
├── collection_element_host_type: "uint8_t"
└── collection_element_wire_reader: "u8"
```

#### Field Node (Group)
```
field (MAP)
├── name: "bids"
├── is_group_field: "1"
├── group_type: "Level"
├── supports_group_cursor: "1"
├── group_cursor_accessible: "1"
├── is_first_group_field: "1"
└── group_dimension_size: "4"
```

#### Field Node (Variable Data)
```
field (MAP)
├── name: "symbol"
├── is_var_data: "1"
├── is_variable_size: "1"
├── var_data_accessor_accessible: "1"
├── is_first_var_data_field: "1"
└── var_data_from_block_length: "1"
```

#### Enum Node
```
enum (MAP)
├── enum_name: "Side"
├── underlying_type: "uint8"
└── items (LIST)
    ├── [0] (MAP)
    │   ├── name: "Buy"
    │   └── value: "1"
    └── [1] (MAP)
        ├── name: "Sell"
        └── value: "2"
```

#### Flags Node
```
flags (MAP)
├── enum_name: "OrderFlags"
├── underlying_type: "uint8"
├── is_flags: "1"
└── items (LIST)
    ├── [0] (MAP)
    │   ├── name: "IOC"
    │   └── value: "1"
    ├── [1] (MAP)
    │   ├── name: "FOK"
    │   └── value: "2"
    └── [2] (MAP)
        ├── name: "PostOnly"
        └── value: "4"
```

**Note:** Flags are stored in the `enums` list but have `is_flags: "1"` marker.

---

### Step 4: Template Best Practices

#### Use Sections for Conditionals
```mustache
{{#is_numeric}}
// This is a numeric field
{{/is_numeric}}

{{^is_numeric}}
// This is NOT a numeric field
{{/is_numeric}}

{{#is_flags}}
// This is a flags type (not a regular enum)
{{/is_flags}}
```

#### Iterate Over Lists
```mustache
{{#messages}}
struct {{message_name}} {
{{#fields}}
    {{type}} {{name}};
{{/fields}}
};
{{/messages}}
```

#### Handle Last Item
```mustache
{{#items}}
    {{name}} = {{value}}{{^last}},{{/last}}
{{/items}}
```

#### Nested Sections
```mustache
{{#messages}}
{{#fields}}
{{#is_collection}}
{{#collection_element_is_primitive}}
// Primitive array: {{name}}[{{length_field}}]
{{/collection_element_is_primitive}}
{{/is_collection}}
{{/fields}}
{{/messages}}
```

---

### Step 5: Register the Template

Edit `tbe_compiler/main.c`:

```c
static const char *resolve_template(const char *user_template,
                                    int64_t     lang_enum) {
    if (user_template) return user_template;

    switch (lang_enum) {
    case 0:  return "templates/c_structs.mustache";
    case 1:  return "templates/python_dataclass.mustache";
    case 2:  return "templates/rust_structs.mustache";
    case 3:  return "templates/go_structs.mustache";  // NEW
    default: return "templates/c_structs.mustache";
    }
}

// Add to enum choices
CmdArgerEnumDesc lang_choices[] = {
    { "c",      "C/C++ header output",      0 },
    { "python", "Python dataclass output",  1 },
    { "rust",   "Rust struct output",       2 },
    { "go",     "Go struct output",         3 },  // NEW
};
```

---

### Step 6: Language-Specific Considerations

#### C/C++
- Use `typedef struct` for types
- Generate inline functions for accessors
- Use `const` for read-only views
- Handle byte order with `tbe_wire_*` functions

#### Python
- Use `dataclass` or `NamedTuple`
- Generate `__init__`, `encode()`, `decode()` methods
- Use `struct.pack/unpack` for binary encoding
- Handle endianness with format strings

#### Rust
- Use `#[repr(C)]` for binary layout
- Generate `impl` blocks for methods
- Use `byteorder` crate for endianness
- Leverage Rust's type system for safety

#### Go
- Use `binary.Read/Write` for encoding
- Generate methods on struct types
- Use `encoding/binary` for byte order
- Consider `unsafe` for zero-copy

#### Java
- Use `ByteBuffer` for encoding
- Generate builder pattern
- Handle endianness with `ByteOrder`
- Consider `sun.misc.Unsafe` for performance

---

### Step 7: Testing

Create `test_<language>_codegen.c`:

```c
it("should generate <language> code") {
    const char *schema = "message Test { uint32 value; }";
    char *output = render_template(schema, "<language>");

    check_str_contains(output, "expected_pattern");
    check_str_contains(output, "another_pattern");

    free(output);
}
```

---

## Template System

### Mustache Basics

Mustache is a logic-less template system. It uses tags:

| Tag | Purpose | Example |
|-----|---------|---------|
| `{{name}}` | Variable | `{{message_name}}` |
| `{{#section}}...{{/section}}` | Section (if true) | `{{#is_numeric}}...{{/is_numeric}}` |
| `{{^section}}...{{/section}}` | Inverted section (if false) | `{{^is_numeric}}...{{/is_numeric}}` |
| `{{!comment}}` | Comment | `{{!-- This is ignored --}}` |

### Data Provider

The `mustache_helpers.c` provides the data:

```c
// Get root node
void *get_root(void *provider_data);

// Dump string value
int dump_node(void *node, ...);

// Get child by name (for {{field_name}})
void *get_child_by_name(void *node, const char *name, size_t size, ...);

// Get child by index (for {{#list}}...{{/list}})
void *get_child_by_index(void *node, unsigned index, ...);
```

### Dot Notation

Access nested fields:
```mustache
{{schema.schema_name}}
{{schema.attributes.id.value}}
```

---

## Node Tree API

### Creating Nodes

```c
// Create string node
Node *create_node_string(const char *name, const char *val);

// Create list node
Node *create_node_list(const char *name);

// Create map node
Node *create_node_map(const char *name);
```

### Manipulating Nodes

```c
// Add to list
int list_add(Node *list, Node *item);

// Add to map
int map_add(Node *map, Node *item);

// Free node tree
void node_free(Node *node);
```

### Querying Nodes

```c
// Find child by name (internal API)
static Node *map_find_named_child(const Node *map, const char *name);

// Get string value (internal API)
static const char *map_find_string_value(const Node *map, const char *name);

// Check if child exists (internal API)
static int map_has_named_child(const Node *map, const char *name);
```

---

## Testing

### Unit Tests

Run parser tests:
```bash
./test_sbe_parser
```

### Integration Tests

Run compiler tests:
```bash
./test_sbe_compiler
```

### Adding Tests

```c
it("should handle new feature") {
    const char *schema = "...";
    Node *root = create_node_map(NULL);
    tbe_error_t err;

    int rc = parse_schema(schema, strlen(schema), root, &err);

    check_int_eq(rc, 0);
    // ... more assertions

    node_free(root);
}
```

---

## Contributing

### Code Style

- **Indentation**: 4 spaces (no tabs)
- **Braces**: K&R style
- **Naming**: `snake_case` for functions, `UPPER_CASE` for macros
- **Comments**: Explain WHY, not WHAT

### Commit Messages

```
<type>: <subject>

<body>

<footer>
```

**Types:**
- `feat`: New feature
- `fix`: Bug fix
- `refactor`: Code refactoring
- `docs`: Documentation
- `test`: Tests
- `perf`: Performance improvement

**Example:**
```
feat: add Go code generation template

- Create go_structs.mustache template
- Add Go to language choices
- Generate zero-copy accessors

Closes #123
```

### Pull Request Process

1. Fork the repository
2. Create a feature branch
3. Write tests for your changes
4. Ensure all tests pass
5. Update documentation
6. Submit pull request

---

## Advanced Topics

### Custom Annotators

Add new metadata to nodes:

```c
static void annotate_custom_metadata(Node *root) {
    Node *messages = map_find_named_child(root, "messages");
    if (!messages) return;

    for (size_t i = 0; i < messages->data.list.count; i++) {
        Node *msg = messages->data.list.items[i];
        // Add custom metadata
        map_set_string(msg, "custom_field", "custom_value");
    }
}

// Register in annotate_schema_tree()
static void annotate_schema_tree(Node *root) {
    annotate_schema_metadata(root);
    annotate_wire_constants(root);
    annotate_layouts(root);
    annotate_type_references(root);
    annotate_group_cursors(root);
    annotate_var_data_accessors(root);
    annotate_custom_metadata(root);  // NEW
}
```

### Custom Wire Functions

Add new encoding functions in `tbe_wire.h`:

```c
static inline void tbe_wire_write_custom(uint8_t *data,
                                         int is_big_endian,
                                         custom_type_t value) {
    // Custom encoding logic
}
```

### Parser Extensions

Extend the grammar in `schema_grammar.y`:

```yacc
custom_decl ::= CUSTOM IDENT(N) LBRACE custom_body RBRACE. {
    // Handle custom declaration
}
```

---

## Debugging

### Enable Debug Output

```c
#define TBE_DEBUG 1
#include "schema_parser_dsl.h"
```

### Inspect Node Tree

```c
static void print_node_tree(Node *node, int depth) {
    // Recursively print tree structure
}
```

### Common Issues

1. **Template not rendering**: Check node names match template tags
2. **Missing metadata**: Ensure annotators run before template
3. **Memory leaks**: Always call `node_free()` on root

---

## Performance Optimization

### Template Compilation

Mustache templates are compiled once, reused many times.

### Zero-Copy Design

Generated code uses pointers, no data copying.

### Inline Functions

All accessors are `static inline` for zero overhead.

---

## Resources

- **Mustache Spec**: https://mustache.github.io/
- **TBE Specification**: https://github.com/real-logic/simple-binary-encoding
- **re2c Manual**: https://re2c.org/
- **Lemon Parser**: https://www.hwaci.com/sw/lemon/

---

## License

See LICENSE file for details.
