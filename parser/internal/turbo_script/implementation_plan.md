# Turbo Script - High Performance Scripting Language

## Objective
Build a lightweight scripting language "Turbo Script" leveraging existing C parsers (`exprtk_parser`, `strtk_parser`, `datetime_parser`, `json_parser`).

## Architecture
- **Core Engine**: `exprtk_parser` (Variables, Control Flow, Expressions, Functions).
- **Standard Library**:
    - **String Ops**: `strtk_tokenize`, `strtk_split`, `strtk_to_int`, etc.
    - **Date Ops**: `datetime_parse`, `datetime_to_time`, `datetime_format`.
    - **JSON Ops**: `json_query`, `json_parse` (limited).
    - **File Ops**: `file_read`, `file_write` (using generic FS ops).

## Implementation Details

### 1. `turbo_script.h`
Public API for initializing context and running scripts.

```c
typedef struct turbo_script_ctx_s turbo_script_ctx_t;

turbo_script_ctx_t *turbo_script_init();
void turbo_script_free(turbo_script_ctx_t *ctx);

// Run script from string/file
int turbo_script_run(turbo_script_ctx_t *ctx, const char *script);
int turbo_script_run_file(turbo_script_ctx_t *ctx, const char *filename);

// Bind custom functions/variables
void turbo_script_bind_func(turbo_script_ctx_t *ctx, const char *name, turbo_script_func_t func);
void turbo_script_set_var(turbo_script_ctx_t *ctx, const char *name, double value);
double turbo_script_get_var(turbo_script_ctx_t *ctx, const char *name);
```

### 2. Built-in Functions (Native Bindings)

**String Module (`strtk`)**
- `split(str, delim)` -> Returns Vector of Strings.
- `join(vec, delim)` -> Returns String.
- `to_int(str)` -> Number.
- `to_double(str)` -> Number.
- `to_bool(str)` -> Number (0/1).
- `tokenize(str, delims)` -> Vector.

**DateTime Module (`datetime_parser`)**
- `now()` -> Timestamp (Number).
- `date_parse(str)` -> Timestamp.
- `date_format(timestamp, format_str)` -> String.
- `date_part(timestamp, part_str)` -> Number (year, month, etc).
- `date_add(timestamp, val, unit)` -> Timestamp.

**JSON Module (`json_parser`)**
- `json_query(json_str, key_path)` -> Value (String/Number/Boolean).
  - Uses `json_parser` to traverse.
  - Returns raw value or JSON substring for objects/arrays.
- `json_parse(str)` -> Opaque Handle? Or just stick to query for now.
  - `exprtk` supports strict types. Opaque handles (pointers as numbers) are risky without GC.
  - Better to keep it stateless or return stringified JSON.

### 3. Integration with `exprtk`
- `turbo_script_ctx_t` wraps `exprtk_env_t` and `exprtk_parser_t` (if we separate parsing).
- Registers native functions using `exprtk_env_register_func`.
- Handles memory management (arenas) for string results.

## Strategy
1.  **Phase 1**: Structure and Basic Integration.
    - Create `turbo_script` module.
    - Initialize `exprtk` environment.
    - Run basic math scripts.
2.  **Phase 2**: String & File IO.
    - Bind `read_file`, `write_file` (already somewhat in `exprtk` now? I saw `read_file` in `exprtk.c`).
    - Bind `strtk` functions.
3.  **Phase 3**: DateTime & JSON.
    - Bind `datetime` functions.
    - Bind `json_query`.
4.  **Phase 4**: Testing & Validation.
    - Comprehensive test suite using `tinytest`.

## File Structure
- `parser/internal/turbo_script/`
  - `include/turbo_script.h`
  - `src/turbo_script.c`
  - `test/test_turbo_script.c`
  - `CMakeLists.txt`

