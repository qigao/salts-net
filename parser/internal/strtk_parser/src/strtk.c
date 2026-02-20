#include "strtk.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

// Helper: Check if char is in delimiter set
static int is_delim(char c, const char *delims) {
    if (!delims) return 0;
    while (*delims) {
        if (c == *delims) return 1;
        delims++;
    }
    return 0;
}

char **strtk_tokenize(const char *input, size_t len, const char *delims, bool ignore_empty, size_t *count) {
    if (!input) return NULL;
    if (len == 0) len = strlen(input);
    if (!delims) delims = "";

    size_t capacity = 16;
    char **tokens = (char**)calloc(capacity, sizeof(char*));
    if (!tokens) return NULL;
    
    *count = 0;
    
    const char *start = input;
    const char *end = input + len;
    const char *curr = start;
    
    while (curr < end) {
        // Skip leading delimiters if needed (strtok style) or empty tokens allowed?
        // If ignore_empty, skip delimiters.
        // If !ignore_empty, delimiters produce empty tokens.
        
        // 1. Find end of token
        const char *token_start = curr;
        const char *token_end = curr;
        
        int in_quote = 0;
        char quote_char = 0;
        
        while (curr < end) {
            char c = *curr;
            
            // Handle quotes
            if ((c == '"' || c == '\'') && (!in_quote || c == quote_char)) {
                if (!in_quote) {
                    in_quote = 1;
                    quote_char = c;
                    // Skip opening quote? No, include it in token for now, or strip it?
                    // StrTk usually strips quotes if parsed as string type.
                    // Here we just split raw strings.
                    token_end = curr + 1; // Include quote
                } else {
                    in_quote = 0;
                    quote_char = 0;
                    token_end = curr + 1; // Include closing quote
                }
                curr++;
                continue;
            }
            
            if (!in_quote && is_delim(c, delims)) {
                break; // Found delimiter outside quotes
            }
            
            token_end = curr + 1;
            curr++;
        }
        
        // Found token [token_start, token_end)
        size_t token_len = (size_t)(token_end - token_start);
        
        if (token_len > 0 || !ignore_empty) {
            // Add token
            if (*count >= capacity) {
                capacity *= 2;
                char **new_tokens = (char**)realloc(tokens, capacity * sizeof(char*));
                if (!new_tokens) {
                    strtk_free_tokens(tokens, *count);
                    return NULL;
                }
                tokens = new_tokens;
            }
            
            char *token_str = (char*)malloc(token_len + 1);
            if (token_str) {
                memcpy(token_str, token_start, token_len);
                token_str[token_len] = '\0';
                tokens[*count] = token_str;
                (*count)++;
            }
        }
        
        // If we stopped at a delimiter, skip it
        if (curr < end && is_delim(*curr, delims)) {
            curr++;
        }
    }
    
    return tokens;
}

void strtk_free_tokens(char **tokens, size_t count) {
    if (!tokens) return;
    for (size_t i = 0; i < count; ++i) {
        if (tokens[i]) free(tokens[i]);
    }
    free(tokens);
}

// ============================================================================
// Conversion
// ============================================================================

bool strtk_to_int(const char *s, long long *out) {
    if (!s || !out) return false;
    char *endptr;
    long long val = strtoll(s, &endptr, 0); // 0 allows hex/oct detection
    if (endptr == s) return false; // No digits found
    // Check if entire string consumed? Or allow trailing whitespace?
    // StrTk strictness varies. Let's allow trailing whitespace but strict otherwise.
    while (isspace((unsigned char)*endptr)) endptr++;
    if (*endptr != '\0') return false;
    
    *out = val;
    return true;
}

bool strtk_to_double(const char *s, double *out) {
    if (!s || !out) return false;
    char *endptr;
    double val = strtod(s, &endptr);
    if (endptr == s) return false;
    while (isspace((unsigned char)*endptr)) endptr++;
    if (*endptr != '\0') return false;
    
    *out = val;
    return true;
}

bool strtk_to_bool(const char *s, bool *out) {
    if (!s || !out) return false;
    if (strcasecmp(s, "true") == 0 || strcasecmp(s, "1") == 0 || strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(s, "false") == 0 || strcasecmp(s, "0") == 0 || strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

char **strtk_split(const char *input, char delimiter, size_t *count) {
    char delims[2] = { delimiter, '\0' };
    return strtk_tokenize(input, 0, delims, false, count);
}
