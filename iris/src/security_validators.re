// re2c --lang c
/**
 * @file security_validators.re
 * @brief High-performance security validators using re2c
 *
 * Optimized character validation for HTTP headers, cookies, URLs, and parameters.
 * Replaces manual character-by-character validation loops with generated state machines.
 *
 * Build: re2c -o security_validators_gen.c security_validators.re
 */

#include "security.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Suppress MSVC warnings about unused YYMARKER variables
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4101) // unreferenced local variable
#pragma warning(disable: 4701) // potentially uninitialized local variable used
#pragma warning(disable: 4703) // potentially uninitialized local pointer variable used
#endif

// memmem implementation for platforms that don't have it
#ifndef __GLIBC__
static void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {
    if (needlelen == 0) return (void*)haystack;
    if (haystacklen < needlelen) return NULL;
    
    const char *h = (const char*)haystack;
    const char *n = (const char*)needle;
    
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (memcmp(h + i, n, needlelen) == 0) {
            return (void*)(h + i);
        }
    }
    return NULL;
}
#endif

/**
 * @brief Validate HTTP header name using re2c (RFC 7230 token)
 * @param name Header name to validate
 * @param len Length of header name
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
iris_security_result_t iris_validate_http_header_name_re2c(const char *name, size_t len) {
    if (!name || len == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    const char *YYCURSOR = name;
    const char *YYLIMIT = name + len;
    const char *YYMARKER = name; // Initialize YYMARKER

    /*!re2c
      re2c:define:YYCTYPE = "char";
      re2c:define:YYLIMIT = "YYLIMIT";
      re2c:yyfill:enable = 0;
      
      // RFC 7230 token characters
      TOKEN_CHAR = [a-zA-Z0-9!#$%&'*+\-.^_`|~];
      
      // Valid header name: sequence of token characters
      TOKEN_CHAR+ {
        // Check if we consumed the entire input
        if (YYCURSOR == YYLIMIT) {
          return IRIS_SECURITY_OK;
        }
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
      }
      
      // Invalid characters
      * { return IRIS_SECURITY_ERROR_INVALID_FORMAT; }
    */
}

/**
 * @brief Validate HTTP header value using re2c (RFC 7230)
 * @param value Header value to validate
 * @param len Length of header value
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
iris_security_result_t iris_validate_http_header_value_re2c(const char *value, size_t len) {
    if (!value) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    // Check for CRLF injection first
    if (memchr(value, '\r', len) || memchr(value, '\n', len)) {
        return IRIS_SECURITY_ERROR_MALICIOUS_CONTENT;
    }

    const char *YYCURSOR = value;
    const char *YYLIMIT = value + len;
    const char *YYMARKER = value; // Initialize YYMARKER

    /*!re2c
      re2c:define:YYCTYPE = "char";
      re2c:define:YYLIMIT = "YYLIMIT";
      re2c:yyfill:enable = 0;
      
      // Valid header value characters (printable ASCII + space + tab)
      VALID_CHAR = [\x20-\x7E] | [\t];
      
      // Match entire string of valid characters
      VALID_CHAR* {
        if (YYCURSOR == YYLIMIT) {
          return IRIS_SECURITY_OK;
        }
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
      }
    */
}

/**
 * @brief Validate cookie name using re2c (RFC 6265 token)
 * @param name Cookie name to validate
 * @param len Length of cookie name
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
iris_security_result_t iris_validate_cookie_name_re2c(const char *name, size_t len) {
    // Cookie name uses same token rules as HTTP header name
    return iris_validate_http_header_name_re2c(name, len);
}

/**
 * @brief Validate cookie value using re2c (RFC 6265)
 * @param value Cookie value to validate
 * @param len Length of cookie value
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
iris_security_result_t iris_validate_cookie_value_re2c(const char *value, size_t len) {
    if (!value) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    // Empty cookie value is valid
    if (len == 0) {
        return IRIS_SECURITY_OK;
    }

    // Simple character-by-character validation
    for (size_t i = 0; i < len; i++) {
        char c = value[i];
        
        // Valid characters: alphanumeric, hyphen, underscore, period, tilde
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            continue; // Valid character
        }
        
        // Any other character is invalid
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    }

    return IRIS_SECURITY_OK;
}

/**
 * @brief Validate URL path using re2c
 * @param path URL path to validate
 * @param len Length of path
 * @return IRIS_SECURITY_OK if valid, error code otherwise
 */
iris_security_result_t iris_validate_url_path_re2c(const char *path, size_t len) {
    if (!path || len == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    // Path must start with '/'
    if (path[0] != '/') {
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    }

    // Check for path traversal patterns
    if (memmem(path, len, "../", 3) || memmem(path, len, "..\\", 3) ||
        memmem(path, len, "/.", 2) || memmem(path, len, "\\.", 2) ||
        memmem(path, len, "//", 2)) {
        return IRIS_SECURITY_ERROR_MALICIOUS_CONTENT;
    }

    // Check for null bytes and control characters
    if (memchr(path, '\0', len) || memchr(path, '\r', len) || memchr(path, '\n', len)) {
        return IRIS_SECURITY_ERROR_MALICIOUS_CONTENT;
    }

    const char *YYCURSOR = path;
    const char *YYLIMIT = path + len;
    const char *YYMARKER = path; // Initialize YYMARKER

    /*!re2c
      re2c:define:YYCTYPE = "char";
      re2c:define:YYLIMIT = "YYLIMIT";
      re2c:yyfill:enable = 0;
      
      // Valid path characters (safe printable ASCII)
      PATH_CHAR = [a-zA-Z0-9!#$%&'()*+,\-./:;=?@\[\]_~];
      
      // Valid path: starts with / followed by valid characters
      "/" PATH_CHAR* {
        if (YYCURSOR == YYLIMIT) {
          return IRIS_SECURITY_OK;
        }
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
      }
      
      // Invalid path
      * { return IRIS_SECURITY_ERROR_INVALID_FORMAT; }
    */
}

/**
 * @brief Check for suspicious parameter patterns using re2c
 * @param param Parameter to check
 * @param len Length of parameter
 * @return true if suspicious, false otherwise
 */
bool iris_is_suspicious_parameter_re2c(const char *param, size_t len) {
    if (!param || len == 0) return false;

    // Use simple string matching instead of re2c for pattern detection
    // This is more reliable and avoids re2c loop issues
    
    // Script injection patterns
    if (strstr(param, "<script") != NULL) return true;
    if (strstr(param, "</script") != NULL) return true;
    if (strstr(param, "javascript:") != NULL) return true;
    if (strstr(param, "vbscript:") != NULL) return true;
    if (strstr(param, "onload=") != NULL) return true;
    if (strstr(param, "onerror=") != NULL) return true;
    
    // SQL injection patterns
    if (strstr(param, "SELECT") != NULL) return true;
    if (strstr(param, "INSERT") != NULL) return true;
    if (strstr(param, "UPDATE") != NULL) return true;
    if (strstr(param, "DELETE") != NULL) return true;
    if (strstr(param, "DROP") != NULL) return true;
    if (strstr(param, "UNION") != NULL) return true;
    if (strstr(param, "OR 1=1") != NULL) return true;
    
    // Path traversal patterns
    if (strstr(param, "../") != NULL) return true;
    if (strstr(param, "..\\") != NULL) return true;
    if (strstr(param, "/etc/passwd") != NULL) return true;
    
    // Command injection patterns
    if (strstr(param, "cmd.exe") != NULL) return true;
    if (strstr(param, "powershell") != NULL) return true;
    if (strstr(param, "bash") != NULL) return true;
    
    // Template injection patterns
    if (strstr(param, "<?php") != NULL) return true;
    if (strstr(param, "<%") != NULL) return true;
    if (strstr(param, "%>") != NULL) return true;
    if (strstr(param, "${") != NULL) return true;
    if (strstr(param, "#{") != NULL) return true;
    if (strstr(param, "{{") != NULL) return true;
    if (strstr(param, "}}") != NULL) return true;
    
    return false; // No suspicious patterns found
}

/**
 * @brief Sanitize URL parameter using re2c
 * @param param Input parameter
 * @param param_len Length of input parameter
 * @param sanitized Output buffer
 * @param buffer_size Size of output buffer
 * @return IRIS_SECURITY_OK on success, error code otherwise
 */
iris_security_result_t iris_sanitize_url_parameter_re2c(const char *param, size_t param_len,
                                                       char *sanitized, size_t buffer_size) {
    if (!param || !sanitized || buffer_size == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    // Initialize the entire buffer to prevent garbage
    memset(sanitized, 0, buffer_size);

    // Handle empty input
    if (param_len == 0) {
        sanitized[0] = '\0';
        return IRIS_SECURITY_OK;
    }

    size_t out_pos = 0;
    
    // Simple character-by-character processing
    for (size_t i = 0; i < param_len && out_pos < buffer_size - 1; i++) {
        char c = param[i];
        
        if (c >= 'a' && c <= 'z') {
            // Lowercase letters - keep as is
            sanitized[out_pos++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            // Uppercase letters - convert to lowercase to break SQL keywords
            sanitized[out_pos++] = c + 32; // 'A' + 32 = 'a'
        } else if (c >= '0' && c <= '9') {
            // Digits - keep as is
            sanitized[out_pos++] = c;
        } else if (c == '-' || c == '_' || c == '.') {
            // Safe symbols - keep as is
            sanitized[out_pos++] = c;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            // Convert whitespace to underscore
            sanitized[out_pos++] = '_';
        }
        // Skip all other characters
    }

    sanitized[out_pos] = '\0';
    return IRIS_SECURITY_OK;
}

// Restore MSVC warning settings
#ifdef _MSC_VER
#pragma warning(pop)
#endif