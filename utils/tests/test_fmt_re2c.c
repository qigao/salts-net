#include <stdio.h>
#include <string.h>
#include "fmt_lexer.h"

// For testing purposes, we can include the generated source if we don't link it.
// But we will link it in the build command.

void print_token(fmt_token_t token, const char *start, size_t len) {
    printf("Token: %d ", token);
    switch (token) {
        case FMT_TOKEN_END: printf("[END]\n"); break;
        case FMT_TOKEN_TEXT: printf("[TEXT] \"%.*s\"\n", (int)len, start); break;
        case FMT_TOKEN_PLACEHOLDER: printf("[PLACEHOLDER] {}\n"); break;
        case FMT_TOKEN_SPECIFIER: printf("[SPECIFIER] {:%.*s}\n", (int)len, start); break;
        case FMT_TOKEN_LBRACE_ESC: printf("[ESC] {{\n"); break;
        case FMT_TOKEN_RBRACE_ESC: printf("[ESC] }}\n"); break;
        case FMT_TOKEN_INVALID: printf("[INVALID] \"%.*s\"\n", (int)len, start); break;
        default: printf("[UNKNOWN]\n"); break;
    }
}

void test(const char *fmt) {
    printf("Testing format string: \"%s\"\n", fmt);
    const char *cursor = fmt;
    const char *token_start;
    size_t token_len;
    fmt_token_t token;

    do {
        token = fmt_scan(&cursor, &token_start, &token_len);
        print_token(token, token_start, token_len);
    } while (token != FMT_TOKEN_END);
    printf("----------------------------------------\n");
}

int main() {
    test("Hello World");
    test("Val: {}");
    test("Val: {:d}");
    test("Val: {:10.4f}");
    test("Escaped: {{ }}");
    test("Invalid: {d}"); // Should be invalid per fmt.h rules
    test("Mixed: {:<10} {} {{}}");
    return 0;
}
