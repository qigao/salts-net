# Mustache Test Suite

This directory contains the centralized test definitions and framework for the mustache module using the **acutest** testing framework.

## Structure

- `CMakeLists.txt` - Test build configuration and definitions
- `acutest.h` - Lightweight C/C++ unit testing framework
- `test_suite.h/c` - Common test utilities and helpers
- `test_*.c` - Individual test implementation files (each with their own TEST_LIST)
- `README.md` - This documentation

## Test Framework: acutest

All tests use the [acutest](https://github.com/mity/acutest) framework, which provides:
- Simple `TEST_CHECK()` and `TEST_CHECK_()` macros
- Automatic test discovery via `TEST_LIST`
- Built-in test runner with detailed output
- Cross-platform support
- No external dependencies

### Test Structure

Each test file follows this pattern:

```c
#include "acutest.h"
#include "mustache_json.h"  // or other headers

static void test_example(void) {
    // Test implementation
    TEST_CHECK(condition);
    TEST_CHECK_(complex_condition, "Error message with %s", value);
}

TEST_LIST = {
    { "example", test_example },
    { 0 }  // Terminator
};
```

## Test Categories

### Integration Tests (`test_json_integration.c`)
- **basic_rendering** - Test basic mustache template rendering with JSON data
- **array_iteration** - Test array iteration in mustache templates  
- **inverted_sections** - Test inverted sections with missing data
- **html_escaping** - Test HTML escaping vs unescaped output

### Parser Tests (`test_new_parser.c`)
- **simple_template** - Test basic template parsing (currently skipped)
- **section_template** - Test section parsing (currently skipped)
- **tokenizer** - Test lexical analysis (currently skipped)

### Specification Tests (`test_spec_adapted.c`)
- Comprehensive tests based on official mustache specification
- Adapted to use TurboNet's JSON parser API
- Includes comments, interpolation, sections, inverted sections, and partials

## Building Tests

Tests are automatically built when `ENABLE_TESTS=ON`:

```bash
cmake -DENABLE_TESTS=ON ..
make
```

## Running Tests

### Individual Test Executables
```bash
# Run specific test suites
./test_mustache_json
./test_new_parser  
./test_mustache_spec
```

Each executable will show detailed output:
```
Test basic_rendering... [ OK ]
Test array_iteration... [ OK ]
Test inverted_sections... [ OK ]
Test html_escaping... [ OK ]

Summary: 4 tests run, 4 passed, 0 failed, 0 skipped
```

### CTest Integration
```bash
# Run all tests
ctest

# Run tests by label/category
ctest -L integration
ctest -L parser
ctest -L specification

# Run specific test
ctest -R test_mustache_json

# Verbose output
ctest -V
```

### Custom Test Targets
```bash
# Run by category using custom targets
make test_integration
make test_parser
make test_specification
make test_all_mustache
```

### Test Options

acutest supports various command-line options:

```bash
# List available tests
./test_mustache_json --list

# Run specific test
./test_mustache_json --run basic_rendering

# Verbose output
./test_mustache_json --verbose

# No summary
./test_mustache_json --no-summary

# TAP output format
./test_mustache_json --tap
```

## Adding New Tests

### 1. Add test function to existing file

```c
static void test_new_feature(void) {
    // Setup
    const char *template_str = "{{greeting}} {{name}}!";
    const char *json_str = "{\"greeting\": \"Hi\", \"name\": \"Alice\"}";
    
    // Test
    json_value_t *json_data = test_parse_json(json_str);
    TEST_CHECK(json_data != NULL);
    
    MUSTACHE_TEMPLATE *template = test_compile_template(template_str);
    TEST_CHECK(template != NULL);
    
    char *result = test_render_template(template, json_data);
    TEST_CHECK_(strcmp(result, "Hi Alice!") == 0, "Expected 'Hi Alice!', got '%s'", result);
    
    // Cleanup
    free(result);
    mustache_release(template);
    json_free(json_data);
}
```

### 2. Update TEST_LIST

```c
TEST_LIST = {
    { "basic_rendering", test_basic_rendering },
    { "array_iteration", test_array_iteration },
    { "new_feature", test_new_feature },  // Add here
    { 0 }
};
```

### 3. For new test files

1. Create `test_new_category.c` with acutest structure
2. Add to `mustache/test/CMakeLists.txt`:
   ```cmake
   add_mustache_test(test_new_category test_new_category.c)
   set_tests_properties(test_new_category PROPERTIES LABELS "new_category")
   ```

## Test Utilities

Common helper functions in `test_suite.h/c`:

```c
// Parse JSON with error checking
json_value_t* test_parse_json(const char* json_str);

// Compile template with error checking  
MUSTACHE_TEMPLATE* test_compile_template(const char* template_str);

// Render template and return result string
char* test_render_template(MUSTACHE_TEMPLATE* template, json_value_t* json_data);
```

## Configuration

Test behavior can be configured in `mustache/test/CMakeLists.txt`:
- Test timeouts
- Compiler definitions  
- Test-specific flags
- Dependencies
- Custom test targets

## Migration Benefits

Moving to acutest and using dedicated test CMakeLists.txt provides:

✅ **Consistent Framework**: All tests use the same acutest macros  
✅ **Better Output**: Detailed test results with pass/fail status  
✅ **Centralized Configuration**: All test definitions in `mustache/test/CMakeLists.txt`  
✅ **IDE Integration**: Better support for test discovery and running  
✅ **Flexible Execution**: Run individual tests, categories, or full suites  
✅ **Standard Compliance**: Uses widely-adopted testing patterns  
✅ **Custom Targets**: Easy category-based test execution with `make test_integration`