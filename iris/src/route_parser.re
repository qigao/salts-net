/**
 * @file route_parser.re
 * @brief High-performance route parsing using re2c
 *
 * Optimized HTTP method parsing for the route trie.
 * Path tokenization uses simple C loops for reliability.
 *
 * Build: re2c -o route_parser_gen.c route_parser.re
 */

#include "route_trie.h"
#include "turbo_buffer.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Suppress MSVC warnings
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4101)
#pragma warning(disable: 4701)
#pragma warning(disable: 4703)
#endif

/**
 * @brief Parse HTTP method string to enum using re2c
 * @param method HTTP method string (e.g., "GET", "POST")
 * @param len Length of method string
 * @return http_method_t enum value
 */
http_method_t parse_http_method_re2c(const char *method, size_t len) {
    if (!method || len == 0 || len > 7) {
        return METHOD_UNKNOWN;
    }

    const char *YYCURSOR = method;
    const char *YYLIMIT = method + len;
    const char *YYMARKER = method;

    /*!re2c
      re2c:define:YYCTYPE = "unsigned char";
      re2c:define:YYLIMIT = "YYLIMIT";
      re2c:yyfill:enable = 0;
      re2c:flags:case-insensitive = 1;

      "GET"     { return (YYCURSOR == YYLIMIT) ? METHOD_GET : METHOD_UNKNOWN; }
      "POST"    { return (YYCURSOR == YYLIMIT) ? METHOD_POST : METHOD_UNKNOWN; }
      "PUT"     { return (YYCURSOR == YYLIMIT) ? METHOD_PUT : METHOD_UNKNOWN; }
      "DELETE"  { return (YYCURSOR == YYLIMIT) ? METHOD_DELETE : METHOD_UNKNOWN; }
      "PATCH"   { return (YYCURSOR == YYLIMIT) ? METHOD_PATCH : METHOD_UNKNOWN; }
      "HEAD"    { return (YYCURSOR == YYLIMIT) ? METHOD_HEAD : METHOD_UNKNOWN; }
      "OPTIONS" { return (YYCURSOR == YYLIMIT) ? METHOD_OPTIONS : METHOD_UNKNOWN; }
      *         { return METHOD_UNKNOWN; }
    */
}

/**
 * @brief Count path segments using simple loop
 * @param path URL path string
 * @param len Length of path
 * @return Number of segments
 */
int count_path_segments_re2c(const char *path, size_t len) {
    if (!path || len == 0) {
        return 0;
    }

    int count = 0;
    size_t i = 0;

    // Skip leading slash
    if (path[0] == '/') {
        i = 1;
    }

    while (i < len) {
        // Skip slashes
        while (i < len && path[i] == '/') {
            i++;
        }
        
        if (i >= len) {
            break;
        }

        // Found start of segment
        count++;

        // Skip to next slash or end
        while (i < len && path[i] != '/') {
            i++;
        }
    }

    return count;
}

/**
 * @brief Extract next path segment
 * @param cursor Current position in path (updated on return)
 * @param limit End of path string
 * @param segment Output segment info
 * @return true if segment found, false if end of path
 */
bool extract_path_segment_re2c(const char **cursor, const char *limit, path_segment_t *segment) {
    if (!cursor || !*cursor || !limit || !segment || *cursor >= limit) {
        return false;
    }

    const char *p = *cursor;

    // Skip leading slashes
    while (p < limit && *p == '/') {
        p++;
    }

    if (p >= limit) {
        *cursor = p;
        return false;
    }

    const char *start = p;

    // Find end of segment (next slash or end)
    while (p < limit && *p != '/') {
        p++;
    }

    size_t seg_len = (size_t)(p - start);
    if (seg_len == 0) {
        *cursor = p;
        return false;
    }

    segment->start = start;
    segment->len = seg_len;
    segment->is_param = (start[0] == ':');
    segment->is_wildcard = (start[0] == '*');

    *cursor = p;
    return true;
}

/**
 * @brief Tokenize path into segments
 * @param arena Memory arena for allocation
 * @param path URL path string
 * @param result Output tokenized path structure
 * @return 0 on success, -1 on error
 */
int tokenize_path_re2c(turbo_pool_t *arena, const char *path, tokenized_path_t *result) {
    if (!path || !result) {
        return -1;
    }

    // Initialize result
    memset(result, 0, sizeof(tokenized_path_t));

    size_t path_len = strlen(path);
    
    // Handle empty path or root
    if (path_len == 0 || (path_len == 1 && path[0] == '/')) {
        return 0;
    }

    // Count segments first
    int segment_count = count_path_segments_re2c(path, path_len);
    if (segment_count == 0) {
        return 0;
    }

    // Allocate segments
    result->capacity = segment_count;
    result->segments = turbo_pool_alloc(arena, sizeof(path_segment_t) * segment_count);
    if (!result->segments) {
        return -1;
    }

    // Extract segments
    const char *cursor = path;
    const char *limit = path + path_len;
    result->count = 0;

    while (result->count < result->capacity) {
        path_segment_t seg;
        if (!extract_path_segment_re2c(&cursor, limit, &seg)) {
            break;
        }
        result->segments[result->count++] = seg;
    }

    return 0;
}

/**
 * @brief Validate URL path character
 * @param c Character to validate
 * @return true if valid URL path character
 */
bool is_valid_path_char_re2c(unsigned char c) {
    // Valid URL path characters: unreserved + sub-delims + : @ /
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~' ||
           c == '!' || c == '$' || c == '&' || c == '\'' ||
           c == '(' || c == ')' || c == '*' || c == '+' ||
           c == ',' || c == ';' || c == '=' ||
           c == ':' || c == '@' || c == '/';
}

/**
 * @brief Fast path comparison for route matching
 * @param pattern Route pattern segment
 * @param pattern_len Length of pattern
 * @param path Request path segment  
 * @param path_len Length of path segment
 * @return true if segments match
 */
bool match_path_segment_re2c(const char *pattern, size_t pattern_len,
                             const char *path, size_t path_len) {
    if (!pattern || !path) {
        return false;
    }

    // Parameter always matches
    if (pattern_len > 0 && pattern[0] == ':') {
        return true;
    }

    // Wildcard matches everything
    if (pattern_len > 0 && pattern[0] == '*') {
        return true;
    }

    // Exact match required
    if (pattern_len != path_len) {
        return false;
    }

    return memcmp(pattern, path, pattern_len) == 0;
}

// Restore MSVC warning settings
#ifdef _MSC_VER
#pragma warning(pop)
#endif
