/**
 * @file security.c
 * @brief Security validation and sanitization implementation for Iris framework
 */

#include "security.h"
#include "turbo_str.h"
#include <fmt.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <cjwt/cjwt.h>
#include "middleware.h"
#include "router.h"
#include "tlog.h"
#include <turbo_parser.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* Global security limits */
static iris_security_limits_t g_security_limits;
static bool g_security_initialized = false;

/* Default security limits */
const iris_security_limits_t IRIS_DEFAULT_SECURITY_LIMITS = {
    .max_header_name_length = 256,
    .max_header_value_length = 8192,
    .max_url_length = 2048,
    .max_cookie_name_length = 256,
    .max_cookie_value_length = 4096,
    .max_json_depth = 32,
    .max_log_message_length = 1024,
    .max_request_body_size = 1024 * 1024, /* 1MB */
    .max_headers_count = 100
};

/* ============================================================================
 * Initialization and Configuration
 * ============================================================================ */

iris_security_result_t iris_security_init(const iris_security_limits_t *limits) {
    if (limits) {
        g_security_limits = *limits;
    } else {
        g_security_limits = IRIS_DEFAULT_SECURITY_LIMITS;
    }
    g_security_initialized = true;
    return IRIS_SECURITY_OK;
}

const iris_security_limits_t *iris_security_get_limits(void) {
    if (!g_security_initialized) {
        iris_security_init(NULL);
    }
    return &g_security_limits;
}

/* ============================================================================
 * Security Context Management
 * ============================================================================ */

iris_security_result_t iris_security_context_init(iris_security_context_t *ctx) {
    if (!ctx) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }
    
    memset(ctx, 0, sizeof(iris_security_context_t));
    ctx->last_request_time = time(NULL);
    ctx->request_count = 0;
    ctx->threat_level = 0;
    ctx->security_level = 1; // Default security level
    
    return IRIS_SECURITY_OK;
}

void iris_security_context_reset(iris_security_context_t *ctx) {
    if (!ctx) {
        return;
    }
    
    // Reset validation state but preserve rate limiting and security flags
    ctx->headers_validated = false;
    ctx->body_validated = false;
    ctx->url_validated = false;
    ctx->cookies_validated = false;
    ctx->output_escaped = false;
}

iris_security_result_t iris_security_context_update_rate_limit(iris_security_context_t *ctx, time_t current_time) {
    if (!ctx) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }
    
    // Simple rate limiting: reset counter every second
    if (current_time > ctx->last_request_time) {
        ctx->request_count = 1;
        ctx->last_request_time = current_time;
    } else {
        ctx->request_count++;
        
        // Basic rate limit: 100 requests per second
        if (ctx->request_count > 100) {
            return IRIS_SECURITY_ERROR_MALICIOUS_CONTENT; // Rate limit exceeded
        }
    }
    
    return IRIS_SECURITY_OK;
}

void iris_security_context_mark_suspicious(iris_security_context_t *ctx, int threat_level) {
    if (!ctx) {
        return;
    }
    
    ctx->suspicious_activity = true;
    if (threat_level > ctx->threat_level) {
        ctx->threat_level = threat_level;
    }
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

// Forward declarations for re2c functions
extern iris_security_result_t iris_validate_http_header_name_re2c(const char *name, size_t len);
extern iris_security_result_t iris_validate_http_header_value_re2c(const char *value, size_t len);
extern iris_security_result_t iris_validate_cookie_name_re2c(const char *name, size_t len);
extern iris_security_result_t iris_validate_cookie_value_re2c(const char *value, size_t len);
extern iris_security_result_t iris_validate_url_path_re2c(const char *path, size_t len);
extern bool iris_is_suspicious_parameter_re2c(const char *param, size_t len);
extern iris_security_result_t iris_sanitize_url_parameter_re2c(const char *param, size_t param_len,
                                                              char *sanitized, size_t buffer_size);

static bool contains_crlf(const char *str) {
    return strstr(str, "\r\n") != NULL || strstr(str, "\n") != NULL || strstr(str, "\r") != NULL;
}

static size_t safe_strlen(const char *str, size_t max_len) {
    if (!str) return 0;
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        len++;
    }
    return len;
}

/* ============================================================================
 * HTTP Header Validation
 * ============================================================================ */

iris_security_result_t iris_validate_http_header_name(const char *name, size_t max_len) {
    if (!name) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t len = safe_strlen(name, max_len + 1);
    if (len == 0) {
        return IRIS_SECURITY_ERROR_INVALID_INPUT;
    }
    if (len > max_len) {
        return IRIS_SECURITY_ERROR_SIZE_EXCEEDED;
    }

    return iris_validate_http_header_name_re2c(name, len);
}

iris_security_result_t iris_validate_http_header_value(const char *value, size_t max_len) {
    if (!value) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t len = safe_strlen(value, max_len + 1);
    if (len > max_len) {
        return IRIS_SECURITY_ERROR_SIZE_EXCEEDED;
    }

    return iris_validate_http_header_value_re2c(value, len);
}

iris_security_result_t iris_validate_http_header(const char *name, const char *value, 
                                                size_t max_name_len, size_t max_value_len) {
    iris_security_result_t result;
    
    result = iris_validate_http_header_name(name, max_name_len);
    if (result != IRIS_SECURITY_OK) {
        return result;
    }
    
    result = iris_validate_http_header_value(value, max_value_len);
    if (result != IRIS_SECURITY_OK) {
        return result;
    }
    
    return IRIS_SECURITY_OK;
}

/* ============================================================================
 * URL and Parameter Validation
 * ============================================================================ */

iris_security_result_t iris_validate_url_path(const char *path, size_t max_len) {
    if (!path) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t len = safe_strlen(path, max_len + 1);
    if (len == 0) {
        return IRIS_SECURITY_ERROR_INVALID_INPUT;
    }
    if (len > max_len) {
        return IRIS_SECURITY_ERROR_SIZE_EXCEEDED;
    }

    return iris_validate_url_path_re2c(path, len);
}

iris_security_result_t iris_sanitize_url_parameter(const char *param, char *sanitized, size_t buffer_size) {
    if (!param || !sanitized || buffer_size == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t param_len = strlen(param);
    return iris_sanitize_url_parameter_re2c(param, param_len, sanitized, buffer_size);
}

bool iris_is_suspicious_parameter(const char *param) {
    if (!param) return false;

    size_t len = strlen(param);
    if (len == 0) return false;

    /* Use optimized pattern matcher */
    if (iris_is_suspicious_parameter_re2c(param, len)) {
        return true;
    }

    /* Check for excessive special characters */
    int special_count = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)param[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.' && c != ' ') {
            special_count++;
        }
    }

    /* If more than 30% special characters, consider suspicious */
    return (special_count * 100 / (int)len) > 30;
}

/* ============================================================================
 * Cookie Validation
 * ============================================================================ */

iris_security_result_t iris_validate_cookie_name(const char *name, size_t max_len) {
    if (!name) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t len = safe_strlen(name, max_len + 1);
    if (len == 0) {
        return IRIS_SECURITY_ERROR_INVALID_INPUT;
    }
    if (len > max_len) {
        return IRIS_SECURITY_ERROR_SIZE_EXCEEDED;
    }

    return iris_validate_cookie_name_re2c(name, len);
}

iris_security_result_t iris_validate_cookie_value(const char *value, size_t max_len) {
    if (!value) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t len = safe_strlen(value, max_len + 1);
    if (len > max_len) {
        return IRIS_SECURITY_ERROR_SIZE_EXCEEDED;
    }

    return iris_validate_cookie_value_re2c(value, len);
}

iris_security_result_t iris_validate_cookie(const char *name, const char *value) {
    const iris_security_limits_t *limits = iris_security_get_limits();
    
    iris_security_result_t result;
    
    result = iris_validate_cookie_name(name, limits->max_cookie_name_length);
    if (result != IRIS_SECURITY_OK) {
        return result;
    }
    
    result = iris_validate_cookie_value(value, limits->max_cookie_value_length);
    if (result != IRIS_SECURITY_OK) {
        return result;
    }
    
    return IRIS_SECURITY_OK;
}

/* ============================================================================
 * Output Sanitization and Escaping
 * ============================================================================ */

iris_security_result_t iris_escape_html(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t input_len = strlen(input);
    size_t out_pos = 0;

    for (size_t i = 0; i < input_len; i++) {
        char c = input[i];
        const char *replacement = NULL;
        size_t replacement_len = 0;

        switch (c) {
            case '<':
                replacement = "&lt;";
                replacement_len = 4;
                break;
            case '>':
                replacement = "&gt;";
                replacement_len = 4;
                break;
            case '&':
                replacement = "&amp;";
                replacement_len = 5;
                break;
            case '"':
                replacement = "&quot;";
                replacement_len = 6;
                break;
            case '\'':
                replacement = "&#x27;";
                replacement_len = 6;
                break;
            default:
                if (out_pos < output_size - 1) {
                    output[out_pos++] = c;
                }
                continue;
        }

        if (out_pos + replacement_len >= output_size) {
            return IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL;
        }

        memcpy(output + out_pos, replacement, replacement_len);
        out_pos += replacement_len;
    }

    if (out_pos >= output_size) {
        return IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL;
    }

    output[out_pos] = '\0';
    return IRIS_SECURITY_OK;
}

iris_security_result_t iris_escape_json(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t input_len = strlen(input);
    size_t out_pos = 0;

    for (size_t i = 0; i < input_len; i++) {
        char c = input[i];
        
        if (out_pos >= output_size - 2) {
            return IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL;
        }

        switch (c) {
            case '"':
                output[out_pos++] = '\\';
                output[out_pos++] = '"';
                break;
            case '\\':
                output[out_pos++] = '\\';
                output[out_pos++] = '\\';
                break;
            case '\b':
                output[out_pos++] = '\\';
                output[out_pos++] = 'b';
                break;
            case '\f':
                output[out_pos++] = '\\';
                output[out_pos++] = 'f';
                break;
            case '\n':
                output[out_pos++] = '\\';
                output[out_pos++] = 'n';
                break;
            case '\r':
                output[out_pos++] = '\\';
                output[out_pos++] = 'r';
                break;
            case '\t':
                output[out_pos++] = '\\';
                output[out_pos++] = 't';
                break;
            default:
                if (c < 0x20) {
                    /* Escape control characters */
                    if (out_pos >= output_size - 6) {
                        return IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL;
                    }
                    fmt(output + out_pos, output_size - out_pos, "\\u{:04x}", (unsigned char)c);
                    out_pos += 6;
                } else {
                    output[out_pos++] = c;
                }
                break;
        }
    }

    if (out_pos >= output_size) {
        return IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL;
    }

    output[out_pos] = '\0';
    return IRIS_SECURITY_OK;
}

iris_security_result_t iris_escape_javascript(const char *input, char *output, size_t output_size) {
    /* For JavaScript strings, use similar escaping to JSON but also escape forward slash */
    if (!input || !output || output_size == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    /* First escape as JSON */
    iris_security_result_t result = iris_escape_json(input, output, output_size);
    if (result != IRIS_SECURITY_OK) {
        return result;
    }

    /* Additional JavaScript-specific escaping could be added here */
    return IRIS_SECURITY_OK;
}

iris_security_result_t iris_escape_url(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t input_len = strlen(input);
    size_t out_pos = 0;

    for (size_t i = 0; i < input_len; i++) {
        unsigned char c = (unsigned char)input[i];
        
        /* Check if character needs encoding */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || 
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            /* Unreserved characters - no encoding needed */
            if (out_pos >= output_size - 1) {
                return IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL;
            }
            output[out_pos++] = c;
        } else {
            /* Encode as %XX */
            if (out_pos >= output_size - 3) {
                return IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL;
            }
            fmt(output + out_pos, output_size - out_pos, "%{:02X}", c);
            out_pos += 3;
        }
    }

    if (out_pos >= output_size) {
        return IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL;
    }

    output[out_pos] = '\0';
    return IRIS_SECURITY_OK;
}

/* ============================================================================
 * JSON-RPC Validation
 * ============================================================================ */

static int count_json_depth(const char *json, size_t max_len) {
    int depth = 0;
    int max_depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = 0; i < max_len && json[i] != '\0'; i++) {
        char c = json[i];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else {
            switch (c) {
                case '"':
                    in_string = true;
                    break;
                case '{':
                case '[':
                    depth++;
                    if (depth > max_depth) {
                        max_depth = depth;
                    }
                    break;
                case '}':
                case ']':
                    depth--;
                    break;
            }
        }
    }

    return max_depth;
}

iris_security_result_t iris_validate_json_structure(const char *json, size_t max_len, int max_depth) {
    if (!json) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t len = safe_strlen(json, max_len + 1);
    if (len == 0) {
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    }
    if (len > max_len) {
        return IRIS_SECURITY_ERROR_SIZE_EXCEEDED;
    }

    /* Check nesting depth */
    int depth = count_json_depth(json, len);
    if (depth > max_depth) {
        return IRIS_SECURITY_ERROR_MALICIOUS_CONTENT;
    }

    /* Skip leading whitespace */
    const char *start = json;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
        if (start - json >= (ptrdiff_t)len) {
            return IRIS_SECURITY_ERROR_INVALID_FORMAT;
        }
    }
    
    /* Recalculate remaining length after skipping whitespace */
    size_t remaining_len = len - (size_t)(start - json);
    
    /* More strict JSON validation */
    char first_char = *start;
    
    if (first_char == '{' || first_char == '[') {
        /* Objects and arrays - basic check for matching braces */
        int brace_count = 0;
        int bracket_count = 0;
        bool in_string = false;
        bool escaped = false;
        
        for (size_t i = 0; i < len; i++) {
            char c = json[i];
            
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
            } else {
                switch (c) {
                    case '"':
                        in_string = true;
                        break;
                    case '{':
                        brace_count++;
                        break;
                    case '}':
                        brace_count--;
                        if (brace_count < 0) return IRIS_SECURITY_ERROR_INVALID_FORMAT;
                        break;
                    case '[':
                        bracket_count++;
                        break;
                    case ']':
                        bracket_count--;
                        if (bracket_count < 0) return IRIS_SECURITY_ERROR_INVALID_FORMAT;
                        break;
                }
            }
        }
        
        /* Check for balanced braces/brackets */
        if (brace_count != 0 || bracket_count != 0) {
            return IRIS_SECURITY_ERROR_INVALID_FORMAT;
        }
        
        return IRIS_SECURITY_OK;
    } else if (first_char == '"') {
        /* String - check for closing quote */
        bool found_closing = false;
        bool escaped = false;
        
        for (size_t i = 1; i < len; i++) {
            char c = json[i];
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                found_closing = true;
                break;
            }
        }
        
        return found_closing ? IRIS_SECURITY_OK : IRIS_SECURITY_ERROR_INVALID_FORMAT;
    } else if ((first_char >= '0' && first_char <= '9') || first_char == '-') {
        /* Number - basic validation */
        return IRIS_SECURITY_OK;
    } else if (remaining_len >= 4 && strncmp(start, "true", 4) == 0) {
        /* Check that it's exactly "true" and not "truexxx" */
        if (remaining_len == 4 || (!isalnum((unsigned char)start[4]) && start[4] != '_')) {
            return IRIS_SECURITY_OK;
        }
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    } else if (remaining_len >= 5 && strncmp(start, "false", 5) == 0) {
        /* Check that it's exactly "false" and not "falsexxx" */
        if (remaining_len == 5 || (!isalnum((unsigned char)start[5]) && start[5] != '_')) {
            return IRIS_SECURITY_OK;
        }
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    } else if (remaining_len >= 4 && strncmp(start, "null", 4) == 0) {
        /* Check that it's exactly "null" and not "nullxxx" */
        if (remaining_len == 4 || (!isalnum((unsigned char)start[4]) && start[4] != '_')) {
            return IRIS_SECURITY_OK;
        }
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    }

    return IRIS_SECURITY_ERROR_INVALID_FORMAT;
}

iris_security_result_t iris_validate_json_rpc(const char *json, size_t max_len, int max_depth) {
    iris_security_result_t result = iris_validate_json_structure(json, max_len, max_depth);
    if (result != IRIS_SECURITY_OK) {
        return result;
    }

    /* Additional JSON-RPC specific validation could be added here */
    /* For now, basic JSON validation is sufficient */
    
    return IRIS_SECURITY_OK;
}

/* ============================================================================
 * Log Sanitization
 * ============================================================================ */

bool iris_detect_log_injection(const char *input) {
    if (!input) return false;

    /* Check for log injection patterns */
    const char *injection_patterns[] = {
        "\n", "\r", "\r\n", "%0a", "%0d", "%0A", "%0D",
        "\\n", "\\r", "\\t", NULL
    };

    for (int i = 0; injection_patterns[i]; i++) {
        if (strstr(input, injection_patterns[i])) {
            return true;
        }
    }

    return false;
}

iris_security_result_t iris_sanitize_log_output(const char *input, char *output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t input_len = strlen(input);
    size_t out_pos = 0;

    for (size_t i = 0; i < input_len && out_pos < output_size - 1; i++) {
        char c = input[i];
        
        /* Replace newlines and carriage returns with spaces */
        if (c == '\n' || c == '\r') {
            output[out_pos++] = ' ';
        } else if (c == '\t') {
            /* Replace tabs with spaces */
            output[out_pos++] = ' ';
        } else if (c >= 0x20 && c <= 0x7E) {
            /* Only allow printable ASCII characters */
            output[out_pos++] = c;
        } else {
            /* Replace non-printable characters with ? */
            output[out_pos++] = '?';
        }
    }

    output[out_pos] = '\0';
    return IRIS_SECURITY_OK;
}

/* ============================================================================
 * Content Type and File Validation
 * ============================================================================ */

iris_security_result_t iris_validate_content_type(const char *content_type, size_t max_len) {
    if (!content_type) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    size_t len = safe_strlen(content_type, max_len + 1);
    if (len == 0) {
        return IRIS_SECURITY_ERROR_INVALID_INPUT;
    }
    if (len > max_len) {
        return IRIS_SECURITY_ERROR_SIZE_EXCEEDED;
    }

    /* Basic content type format validation */
    const char *slash = strchr(content_type, '/');
    if (!slash || slash == content_type || slash == content_type + len - 1) {
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    }

    /* Check for CRLF injection */
    if (contains_crlf(content_type)) {
        return IRIS_SECURITY_ERROR_MALICIOUS_CONTENT;
    }

    return IRIS_SECURITY_OK;
}

iris_security_result_t iris_validate_file_extension(const char *filename, const char **allowed_extensions) {
    if (!filename || !allowed_extensions) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

    const char *ext = strrchr(filename, '.');
    if (!ext) {
        return IRIS_SECURITY_ERROR_INVALID_FORMAT;
    }

    ext++; /* Skip the dot */

    for (int i = 0; allowed_extensions[i]; i++) {
        if (tstr_casecmp(ext, allowed_extensions[i]) == 0) {
            return IRIS_SECURITY_OK;
        }
    }

    return IRIS_SECURITY_ERROR_MALICIOUS_CONTENT;
}

iris_security_result_t iris_validate_file_size(size_t file_size, size_t max_size) {
    if (file_size > max_size) {
        return IRIS_SECURITY_ERROR_SIZE_EXCEEDED;
    }
    return IRIS_SECURITY_OK;
}

/* ============================================================================
 * Security Utilities
 * ============================================================================ */

iris_security_result_t iris_generate_random_bytes(uint8_t *buffer, size_t size) {
    if (!buffer || size == 0) {
        return IRIS_SECURITY_ERROR_NULL_POINTER;
    }

#ifdef _WIN32
    HCRYPTPROV hProv;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return IRIS_SECURITY_ERROR_INVALID_INPUT;
    }
    
    BOOL result = CryptGenRandom(hProv, (DWORD)size, buffer);
    CryptReleaseContext(hProv, 0);
    
    return result ? IRIS_SECURITY_OK : IRIS_SECURITY_ERROR_INVALID_INPUT;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return IRIS_SECURITY_ERROR_INVALID_INPUT;
    }
    
    ssize_t bytes_read = read(fd, buffer, size);
    close(fd);
    
    return (bytes_read == (ssize_t)size) ? IRIS_SECURITY_OK : IRIS_SECURITY_ERROR_INVALID_INPUT;
#endif
}

bool iris_secure_string_compare(const char *str1, const char *str2, size_t len) {
    if (!str1 || !str2) return false;
    
    volatile uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= str1[i] ^ str2[i];
    }
    
    return result == 0;
}

void iris_secure_memzero(void *ptr, size_t size) {
    if (!ptr || size == 0) return;
    
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    for (size_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

const char *iris_security_error_string(iris_security_result_t result) {
    switch (result) {
        case IRIS_SECURITY_OK:
            return "Success";
        case IRIS_SECURITY_ERROR_INVALID_INPUT:
            return "Invalid input";
        case IRIS_SECURITY_ERROR_SIZE_EXCEEDED:
            return "Size limit exceeded";
        case IRIS_SECURITY_ERROR_MALICIOUS_CONTENT:
            return "Malicious content detected";
        case IRIS_SECURITY_ERROR_INVALID_FORMAT:
            return "Invalid format";
        case IRIS_SECURITY_ERROR_NULL_POINTER:
            return "Null pointer";
        case IRIS_SECURITY_ERROR_BUFFER_TOO_SMALL:
            return "Buffer too small";
        default:
            return "Unknown error";
    }
}

static const char *cjwt_code_to_str(cjwt_code_t rv) {
    switch (rv) {
        case CJWTE_OK:                      return "OK";
        case CJWTE_INVALID_PARAMETERS:      return "Invalid parameters";
        case CJWTE_INVALID_SECTIONS:        return "Invalid sections";
        case CJWTE_OUT_OF_MEMORY:           return "Out of memory";
        case CJWTE_HEADER_MISSING:          return "Header missing";
        case CJWTE_HEADER_INVALID_BASE64:   return "Header invalid base64";
        case CJWTE_HEADER_INVALID_JSON:     return "Header invalid JSON";
        case CJWTE_HEADER_MISSING_ALG:      return "Header missing alg";
        case CJWTE_HEADER_UNSUPPORTED_ALG:  return "Header unsupported alg";
        case CJWTE_PAYLOAD_MISSING:         return "Payload missing";
        case CJWTE_PAYLOAD_INVALID_BASE64:  return "Payload invalid base64";
        case CJWTE_PAYLOAD_INVALID_JSON:    return "Payload invalid JSON";
        case CJWTE_SIGNATURE_MISSING:       return "Signature missing";
        case CJWTE_SIGNATURE_VALIDATION_FAILED: return "Signature validation failed";
        case CJWTE_TIME_BEFORE_NBF:         return "Time before nbf";
        case CJWTE_TIME_AFTER_EXP:          return "Time after exp";
        default:                            return "Other error";
    }
}

/* ============================================================================
 * JWT (JSON Web Token) Support Implementation
 * ============================================================================ */

static char *g_iris_jwt_secret = NULL;

void iris_jwt_set_secret(const char *secret) {
    if (g_iris_jwt_secret) free(g_iris_jwt_secret);
    g_iris_jwt_secret = secret ? strdup(secret) : NULL;
}

static void jwt_cleanup_cb(void *data) {
    if (data) {
        cjwt_destroy((cjwt_t *)data);
    }
}

int iris_jwt_middleware(Req *req, Res *res, Chain *chain) {
    const char *auth = get_headers(req, "Authorization");
    if (!auth || tstr_ncasecmp(auth, "Bearer ", 7) != 0) {
        send_json(res, 401, "{\"error\":\"Unauthorized\", \"message\":\"Missing or invalid Authorization header\"}");
        return 1;
    }

    const char *token = auth + 7;
    // Skip any extra spaces after Bearer
    while (*token == ' ') token++;

    if (!g_iris_jwt_secret) {
        TLOG_ERROR("JWT Middleware: Verification secret not configured");
        send_json(res, 500, "{\"error\":\"Internal Server Error\", \"message\":\"JWT verification secret not configured\"}");
        return 1;
    }

    cjwt_t *jwt = NULL;
    int64_t current_time = (int64_t)time(NULL);
    
    /* Decodes token with HS256 algorithm */
    cjwt_code_t rv = cjwt_decode(token, (int)strlen(token), OPT_ALLOW_ONLY_HS_ALG, (const uint8_t *)g_iris_jwt_secret, (int)strlen(g_iris_jwt_secret), current_time, 0, &jwt);

    if (rv != CJWTE_OK) {
        TLOG_ERROR("JWT Middleware: Token verification failed (error: {})", cjwt_code_to_str(rv));
        char err_msg[256];
        fmt(err_msg, sizeof(err_msg),
            "{{\"error\":\"Unauthorized\", \"message\":\"Invalid or expired token (cjwt error code: {})\"}}",
            (int)rv);
        send_json(res, 401, err_msg);
        return 1;
    }

    // Success - attach JWT to request context
    set_context(req, jwt, sizeof(cjwt_t), jwt_cleanup_cb);

    return next(chain, req, res);
}

char *iris_jwt_encode(const char *secret, const char *claims_json) {
    if (!secret || !claims_json) return NULL;

    json_value_t *private_claims = NULL;
    if (turbo_parse_json((const uint8_t *)claims_json, strlen(claims_json), &private_claims) != 0) {
        TLOG_ERROR("JWT Encode: Failed to parse claims JSON");
        return NULL;
    }

    cjwt_t jwt = {0};
    jwt.header.alg = alg_hs256;
    jwt.private_claims = private_claims;

    char *token = NULL;
    cjwt_code_t rv = cjwt_encode(&jwt, (const uint8_t *)secret, (int)strlen(secret), &token);
    turbo_free_json(&private_claims);

    if (rv != CJWTE_OK) {
        TLOG_ERROR("JWT Encode: Failed to encode token (error: {})", cjwt_code_to_str(rv));
        if (token) free(token);
        return NULL;
    }

    return token;
}

void *iris_jwt_get_claims(Req *req) {
    return get_context(req);
}
