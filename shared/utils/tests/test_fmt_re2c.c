#include "fmt_lexer.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

spec("fmt_str Re2c Lexer Tests") {
  it("should handle basic text") {
    const char *fmt_str = "Hello World";
    const char *cursor = fmt_str;
    const char *token_start;
    size_t token_len;
    fmt_token_t token;

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_TEXT);
    check_size_eq(token_len, 11);
    check_ptr_eq(memcmp(token_start, "Hello World", 11), 0);

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_END);
  }

  it("should handle simple placeholders") {
    const char *fmt_str = "{}";
    const char *cursor = fmt_str;
    const char *token_start;
    size_t token_len;
    fmt_token_t token;

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_PLACEHOLDER);

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_END);
  }

  it("should handle format specifiers") {
    const char *fmt_str = "{:d}";
    const char *cursor = fmt_str;
    const char *token_start;
    size_t token_len;
    fmt_token_t token;

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_SPECIFIER);
    check_size_eq(token_len, 1);
    check_int_eq(token_start[0], 'd');

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_END);
  }

  it("should handle escaped braces") {
    const char *fmt_str = "{{}}";
    const char *cursor = fmt_str;
    const char *token_start;
    size_t token_len;
    fmt_token_t token;

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_LBRACE_ESC);

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_RBRACE_ESC);

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_END);
  }

  it("should handle mixed content") {
    const char *fmt_str = "Val: {:04x}";
    const char *cursor = fmt_str;
    const char *token_start;
    size_t token_len;
    fmt_token_t token;

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_TEXT);
    check_size_eq(token_len, 5);

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_SPECIFIER);
    check_size_eq(token_len, 3);

    token = fmt_scan(&cursor, &token_start, &token_len);
    check_int_eq(token, FMT_TOKEN_END);
  }
}
