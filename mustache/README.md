# TurboNet Mustache Module

A C-based Mustache templating engine integrated with TurboNet's JSON parser.

## Features

- **Complete Mustache Implementation**: Based on Mustache4C with full spec compliance
- **Advanced Parser**: Uses re2c + recursive descent for robust lexing and parsing
- **JSON Integration**: Direct integration with TurboNet's high-performance JSON parser
- **Memory Efficient**: Minimal memory footprint with streaming capabilities
- **HTML Escaping**: Built-in HTML escaping for web applications
- **Partial Templates**: Support for template composition
- **Thread Safe**: Can be used in multi-threaded environments
- **AST Support**: Generates Abstract Syntax Tree for advanced template analysis

## Quick Start

```c
#include "mustache_json.h"

// Template string
const char *template_str = "Hello {{name}}! You have {{count}} messages.";

// JSON data
const char *json_str = "{\"name\": \"John\", \"count\": 5}";

// Parse JSON
json_value_t *data = json_parse(json_str, strlen(json_str));

// Compile template
MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);

// Render to string
MUSTACHE_STRING_RENDERER renderer;
mustache_string_renderer_init(&renderer);
mustache_render_json(template, data, &renderer.base, &renderer, NULL, NULL);

char *result = mustache_string_renderer_get(&renderer);
printf("%s\n", result); // Output: Hello John! You have 5 messages.

// Cleanup
free(result);
mustache_string_renderer_free(&renderer);
mustache_release(template);
json_free(data);
```

## Template Syntax

### Variables
```mustache
{{name}}          <!-- HTML escaped -->
{{{name}}}        <!-- Unescaped -->
{{&name}}         <!-- Unescaped (alternative) -->
```

### Sections
```mustache
{{#users}}
  User: {{name}}
{{/users}}
```

### Inverted Sections
```mustache
{{^users}}
  No users found
{{/users}}
```

### Partials
```mustache
{{>header}}
Content here
{{>footer}}
```

## JSON Data Types

The integration handles all JSON data types:

- **Objects**: Used for variable lookup and sections
- **Arrays**: Iterated in sections
- **Strings**: Output directly
- **Numbers**: Converted to strings (integers without decimals)
- **Booleans**: Output as "true"/"false"
- **Null**: Output as "null"

## Advanced Usage

### Custom Template Loader

```c
MUSTACHE_TEMPLATE *load_partial(const char *name, size_t size, void *user_data) {
    // Load template from file system, database, etc.
    char filename[256];
    snprintf(filename, sizeof(filename), "templates/%.*s.mustache", (int)size, name);
    
    // Load and compile template...
    return compiled_template;
}

// Use with renderer
mustache_render_json(template, data, renderer, renderer_data, load_partial, user_data);
```

### Custom Renderer

```c
typedef struct {
    MUSTACHE_RENDERER base;
    FILE *output_file;
} FILE_RENDERER;

int file_out_verbatim(const char *output, size_t size, void *renderer_data) {
    FILE_RENDERER *r = (FILE_RENDERER *)renderer_data;
    return fwrite(output, 1, size, r->output_file) == size ? 0 : -1;
}

// Initialize and use...
```

## Advanced Parser Features

The mustache module now uses re2c + recursive descent parser, providing:

### Benefits over Manual Parsing
- **Better Error Messages**: Precise line/column error reporting
- **Robust Lexing**: Handles edge cases and malformed input gracefully  
- **Performance**: Generated lexer is faster than manual string parsing
- **Maintainability**: Grammar is declarative and easier to extend
- **AST Generation**: Creates Abstract Syntax Tree for advanced use cases

### Parser API

```c
#include "mustache_types.h"

// Parse template into AST
mustache_parse_ctx_t ctx;
if (mustache_parse_template(template_str, strlen(template_str), &ctx) == 0) {
    // Success - use ctx.root AST
    mustache_ast_print(ctx.root, 0); // Debug print
    mustache_ast_free(ctx.root);     // Cleanup
} else {
    printf("Parse error: %s at %d:%d\n", ctx.error_message, ctx.error_line, ctx.error_column);
}

// Tokenize template
mustache_token_t *tokens;
size_t token_count;
if (mustache_tokenize(template_str, strlen(template_str), &tokens, &token_count) == 0) {
    // Process tokens...
    free(tokens);
}
```

### Grammar Overview

The recursive descent parser supports:
- Variables: `{{name}}`, `{{{unescaped}}}`, `{{&unescaped}}`
- Sections: `{{#section}}...{{/section}}`
- Inverted sections: `{{^missing}}...{{/missing}}`
- Partials: `{{>partial_name}}`
- Comments: `{{! comment text }}`
- Delimiter changes: `{{=<% %>=}}`

## Performance Notes

- Templates are compiled once and can be reused multiple times
- JSON data is accessed on-demand without full traversal
- Memory usage scales with template complexity, not data size
- String renderer uses exponential growth for efficient concatenation

## Error Handling

All functions return appropriate error codes:
- `0` for success
- `-1` for errors
- Use `json_get_error()` for JSON parsing errors

## Thread Safety

- Compiled templates are immutable and thread-safe
- JSON data should not be modified during rendering
- Each thread should use its own renderer instance

## Integration with TurboNet

The mustache module is fully integrated with TurboNet's build system and can be used alongside other TurboNet components:

```cmake
target_link_libraries(your_target PRIVATE TurboNet::Mustache)
```

## Examples

See `examples/json_example.c` for a complete working example.

## Tests

Run the integration tests:
```bash
# Basic JSON integration tests
ctest -R mustache_json_integration

# New re2c + recursive descent parser tests  
ctest -R mustache_new_parser

# Comprehensive mustache specification tests
ctest -R mustache_specification

# Run all mustache tests
ctest -R mustache
```

The specification tests are based on the official [mustache specification](https://github.com/mustache/spec) and provide comprehensive coverage of all mustache features.