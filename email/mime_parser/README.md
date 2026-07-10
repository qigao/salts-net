# MIME Parser

A lightweight, callback-driven MIME message parser based on llhttp design philosophy.

## Design Philosophy

Following Linus Torvalds' "Good Taste" principles:

- **Zero-copy**: Parser holds pointers to original data, no unnecessary copying
- **Callback-driven**: No special cases, unified interface for all message types
- **State machine**: Clean, predictable parsing with minimal complexity
- **Memory pool**: All allocations from `mem_pool_t` for efficient memory management

## Features

### Core Parsing (RFC 2045/2046)
- MIME message structure parsing
- Multipart message support (multipart/mixed, multipart/form-data, etc.)
- Streaming parser (can process data in chunks)
- Callback-driven architecture

### Content-Transfer-Encoding (RFC 2045)
- 7bit, 8bit, binary (pass-through)
- Base64 decoding
- Quoted-Printable decoding

### Content-Disposition (RFC 2183)
- Parse attachment/inline/form-data headers
- Extract filename, name, size parameters
- Essential for HTTP file uploads and downloads

### Encoded-Words (RFC 2047)
- Decode email header encoded-words: `=?charset?encoding?text?=`
- Support Base64 (B) and Quoted-Printable (Q) encoding
- Handle mixed plain and encoded text
- UTF-8, ISO-8859-1, and other charset support

### Utilities
- Content-Type parsing with parameter extraction
- Boundary detection and extraction
- Multipart detection

## Usage

### Basic Message Parsing

```c
#include "mime_parser.h"
#include "turbo_buffer.h"

// Setup callbacks
mime_settings_t settings = {0};
settings.on_header_field = my_header_field_cb;
settings.on_header_value = my_header_value_cb;
settings.on_body = my_body_cb;
settings.on_message_complete = my_complete_cb;

// Initialize parser
mem_pool_t pool;
mem_init(&pool, 8192);

mime_parser_t parser;
mime_parser_init(&parser, &settings, &pool);

// Parse data
const char *message = "Content-Type: text/plain\r\n\r\nHello World";
mime_errno_t err = mime_parse(&parser, message, strlen(message));

if (err != MIME_OK) {
    printf("Parse error: %s\n", mime_errno_name(err));
}

mem_destroy(&pool);
```

### Multipart Message Parsing

```c
mime_settings_t settings = {0};
settings.on_part_begin = my_part_begin_cb;
settings.on_header_field = my_header_field_cb;
settings.on_header_value = my_header_value_cb;
settings.on_body = my_body_cb;
settings.on_part_complete = my_part_complete_cb;

mime_parser_t parser;
mime_parser_init(&parser, &settings, &pool);

const char *multipart_msg =
    "Content-Type: multipart/mixed; boundary=----Boundary\r\n"
    "\r\n"
    "------Boundary\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "Part 1\r\n"
    "------Boundary\r\n"
    "Content-Type: text/html\r\n"
    "\r\n"
    "Part 2\r\n"
    "------Boundary--\r\n";

mime_parse(&parser, multipart_msg, strlen(multipart_msg));
```

### Content-Type Parsing

```c
#include "mime_utils.h"

const char *ct = "text/html; charset=utf-8";
mime_content_type_t result;

if (mime_parse_content_type(ct, strlen(ct), &result) == 0) {
    printf("Type: %.*s\n", (int)result.type_len, result.type);
    printf("Subtype: %.*s\n", (int)result.subtype_len, result.subtype);
    if (result.charset) {
        printf("Charset: %.*s\n", (int)result.charset_len, result.charset);
    }
}
```

### Content-Disposition Parsing

```c
#include "mime_content_disposition.h"

// Parse HTTP file upload header
const char *header = "form-data; name=\"file\"; filename=\"photo.jpg\"";
mime_content_disposition_t disp;

if (mime_parse_content_disposition(header, strlen(header), &disp) == 0) {
    printf("Type: %s\n", mime_disposition_type_name(disp.type));

    if (disp.filename) {
        char *filename = mime_disposition_get_filename(&pool, &disp);
        printf("Filename: %s\n", filename);
    }

    if (disp.name) {
        char *name = mime_disposition_get_name(&pool, &disp);
        printf("Field name: %s\n", name);
    }
}

// Parse download header
const char *download = "attachment; filename=\"report.pdf\"; size=12345";
mime_parse_content_disposition(download, strlen(download), &disp);
printf("Size: %zu bytes\n", disp.size);
```

### Encoded-Word Decoding (Email Headers)

```c
#include "mime_encoded_word.h"

// Decode email subject
const char *subject = "=?UTF-8?B?5L2g5aW9?="; // "你好" in base64
char *decoded = mime_decode_header_auto(&pool, subject);
printf("Subject: %s\n", decoded); // "你好"

// Decode mixed plain and encoded text
const char *from = "=?UTF-8?Q?Fran=C3=A7ois?= <francois@example.com>";
decoded = mime_decode_header_auto(&pool, from);
printf("From: %s\n", decoded); // "François <francois@example.com>"

// Decode complex header
const char *complex = "Re: =?UTF-8?B?5rWL6K+V?= from =?UTF-8?Q?John?=";
decoded = mime_decode_header_auto(&pool, complex);
printf("%s\n", decoded); // "Re: 测试 from John"
```

### Body Decoding

```c
#include "mime_utils.h"

// Decode quoted-printable
const char *encoded = "Hello=20World=21";
char *decoded = NULL;
size_t decoded_len = 0;

mime_decode_body(&pool, encoded, strlen(encoded),
                 MIME_ENCODING_QUOTED_PRINTABLE,
                 &decoded, &decoded_len);

printf("Decoded: %s\n", decoded); // "Hello World!"

// Decode base64
const char *base64_data = "SGVsbG8gV29ybGQ=";
uint8_t *binary = NULL;
size_t binary_len = 0;

mime_decode_base64(&pool, base64_data, strlen(base64_data),
                   &binary, &binary_len);
```

## API Reference

### Core Parser API (`mime_parser.h`)

- `mime_parser_init()` - Initialize parser with settings and memory pool
- `mime_parse()` - Parse MIME data (streaming-capable)
- `mime_parser_reset()` - Reset parser for reuse
- `mime_errno_name()` - Get error message string
- `mime_extract_boundary()` - Extract boundary from Content-Type
- `mime_is_multipart()` - Check if Content-Type is multipart
- `mime_find_boundary()` - Find boundary in data buffer

### Utility Functions (`mime_utils.h`)

- `mime_parse_encoding()` - Parse Content-Transfer-Encoding header
- `mime_parse_content_type()` - Parse Content-Type with parameters
- `mime_decode_body()` - Decode body based on encoding
- `mime_decode_base64()` - Decode Base64 content
- `mime_decode_quoted_printable()` - Decode Quoted-Printable content

### Content-Disposition API (`mime_content_disposition.h`)

- `mime_parse_content_disposition()` - Parse Content-Disposition header
- `mime_disposition_type_name()` - Get disposition type name string
- `mime_disposition_get_filename()` - Copy filename to pool string
- `mime_disposition_get_name()` - Copy name to pool string

### Encoded-Word API (`mime_encoded_word.h`)

- `mime_is_encoded_word()` - Check if string is encoded-word
- `mime_parse_encoded_word()` - Parse single encoded-word
- `mime_decode_encoded_word()` - Decode single encoded-word
- `mime_decode_header()` - Decode entire header with multiple encoded-words
- `mime_decode_header_auto()` - Convenience wrapper with auto length

## Callbacks

All callbacks return `int`: 0 to continue, non-zero to abort parsing.

```c
typedef int (*mime_data_cb)(mime_parser_t *parser, const char *data, size_t len);
typedef int (*mime_cb)(mime_parser_t *parser);

struct mime_settings_s {
  mime_data_cb on_header_field;      // Header name
  mime_data_cb on_header_value;      // Header value
  mime_cb on_headers_complete;       // After all headers
  mime_data_cb on_body;              // Body data chunks
  mime_cb on_part_begin;             // New multipart part
  mime_cb on_part_complete;          // Part ends
  mime_cb on_message_complete;       // Entire message parsed
};
```

## Error Codes

- `MIME_OK` - Success
- `MIME_ERROR_INVALID_HEADER` - Malformed header
- `MIME_ERROR_INVALID_BOUNDARY` - Invalid boundary format
- `MIME_ERROR_MISSING_BOUNDARY` - Multipart without boundary
- `MIME_ERROR_NESTED_TOO_DEEP` - Nesting exceeds limit (8 levels)
- `MIME_ERROR_MEMORY` - Memory allocation failed
- `MIME_ERROR_CALLBACK_FAILED` - Callback returned error

## Building

```bash
cd parser/internal/mime_parser
cmake -B build -S .
cmake --build build
./build/test_mime_parser
./build/test_mime_utils
```

## Integration

Add to your CMakeLists.txt:

```cmake
add_subdirectory(parser/internal/mime_parser)
target_link_libraries(your_target mime_parser turbo_utils)
```

## Design Notes

### Why Callback-Driven?

Following llhttp's proven design:
- No special cases: single part and multipart use same code path
- Streaming-friendly: process data as it arrives
- Memory efficient: no need to buffer entire message

### Why No Built-in Message Structure?

The parser is deliberately low-level. Building a DOM-like structure is application-specific:
- Email clients need different structures than HTTP servers
- Some apps only need headers, not bodies
- Callbacks give maximum flexibility

### Encoding Philosophy

Encoding/decoding is separate from parsing:
- Parser identifies encoding type
- Application decides whether to decode
- Keeps parser simple and focused

## Testing

All code is TDD/BDD tested:
- `test_mime_parser.c` - Core parser tests
- `test_mime_utils.c` - Utility function tests
- `test_mime_content_disposition.c` - Content-Disposition parser tests
- `test_mime_encoded_word.c` - Encoded-word decoder tests

Run tests:
```bash
cd build
ctest --verbose
```

Or run individual tests:
```bash
./test_mime_parser
./test_mime_utils
./test_mime_content_disposition
./test_mime_encoded_word
```

## Real-World Examples

### HTTP File Upload Handler

```c
// Parse multipart/form-data upload
mime_settings_t settings = {0};
settings.on_header_field = on_header_field;
settings.on_header_value = on_header_value;
settings.on_body = on_body;
settings.on_part_complete = on_part_complete;

mime_parser_t parser;
mime_parser_init(&parser, &settings, pool);

// In header callback, parse Content-Disposition
int on_header_value(mime_parser_t *p, const char *data, size_t len) {
    if (current_header_is("Content-Disposition")) {
        mime_content_disposition_t disp;
        mime_parse_content_disposition(data, len, &disp);

        if (disp.name) {
            // This is the form field name
            save_field_name(disp.name, disp.name_len);
        }
        if (disp.filename) {
            // This is a file upload
            save_filename(disp.filename, disp.filename_len);
        }
    }
    return 0;
}
```

### Email Subject Decoder

```c
// Decode internationalized email subject
const char *raw_subject = "=?UTF-8?B?5L2g5aW9?= =?UTF-8?Q?World?=";
char *subject = mime_decode_header_auto(&pool, raw_subject);
printf("Subject: %s\n", subject); // "你好World"
```

### HTTP Download Response

```c
// Generate Content-Disposition for download
const char *filename = "report.pdf";
char header[256];
snprintf(header, sizeof(header),
         "Content-Disposition: attachment; filename=\"%s\"",
         filename);
// Send in HTTP response headers
```

## License

Part of TurboUtils project.
