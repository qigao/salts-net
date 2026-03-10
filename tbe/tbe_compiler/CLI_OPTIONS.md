# tbe_compiler Command Line Options

## Overview

`tbe_compiler` generates code and DSL declarations from TBE schema files for multiple target languages and RulesForge integration.

## Basic Usage

```bash
tbe_compiler --schema <schema_file> [options]
```

## Options

### Required

- `--schema <file>` or `-s <file>`
  - Path to the `.schema` definition file
  - Example: `--schema order.schema`

### Language Output

- `--output <file>` or `-o <file>`
  - Output file path for generated code
  - Default: stdout
  - Example: `--output order.h`

- `--lang <language>` or `-l <language>`
  - Target language (uses built-in template)
  - Options: `c`, `python`, `rust`
  - Default: `c`
  - Example: `--lang c`

- `--template <file>` or `-t <file>`
  - Path to custom Mustache template file
  - Overrides `--lang` option
  - Example: `--template my_template.mustache`

### DSL Integration (RulesForge)

- `--dsl-output <file>` or `-d <file>`
  - Generate DSL type declarations (.rfl file)
  - Contains `declare` statements for use in RulesForge
  - Example: `--dsl-output order.rfl`

## Usage Examples

### Example 1: Generate C Header Only

```bash
tbe_compiler --schema order.schema --output order.h
```

### Example 2: Generate DSL Type Declarations

```bash
tbe_compiler --schema order.schema --dsl-output order.rfl
```

Output `order.rfl`:
```rfl
package OrderSchema

declare Order
    id: int
    total: double
    tier: String
end
```

### Example 3: Generate Both

```bash
tbe_compiler --schema order.schema --output order.h --dsl-output order.rfl
```

## Removed Options

- `--codec-project` (REMOVED)
  - Codec DLL project generation is no longer supported by the compiler directly.
- `--rfl-output` (REPLACED)
  - Replaced by `--dsl-output`.

## Notes

- `--dsl-output` uses `templates/rfl_types.mustache` by default.
- DSL output is intended for use in RulesForge to define the structure of data being processed.
