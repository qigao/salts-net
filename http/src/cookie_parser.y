/**
 * @file cookie_parser.y
 * @brief Lemon parser for HTTP Set-Cookie header parsing
 * 
 * Parses cookie strings according to RFC 6265 with full attribute support
 */

%name CookieParse
%token_prefix COOKIE_
%token_type {cookie_token_t}
%default_type {void*}
%extra_argument {cookie_parser_context_t *ctx}

%token_destructor { cookie_token_free(&$$); }

%include {
#include "cookie_parser.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

// Helper function to create cookie attribute
static cookie_attribute_t *cookie_attribute_create(cookie_attribute_type_t type, const char *value) {
    cookie_attribute_t *attr = calloc(1, sizeof(cookie_attribute_t));
    if (!attr) return NULL;
    
    attr->type = type;
    if (value) {
        attr->value = strdup(value);
    }
    return attr;
}

// Helper function to create cookie
static http_cookie_t *cookie_create(const char *name, const char *value) {
    http_cookie_t *cookie = calloc(1, sizeof(http_cookie_t));
    if (!cookie) return NULL;
    
    cookie->name = strdup(name);
    cookie->value = strdup(value);
    cookie->expires = 0;
    cookie->max_age = -1;
    cookie->secure = 0;
    cookie->http_only = 0;
    return cookie;
}

// Helper to unquote string
static char *unquote_string(const char *quoted) {
    if (!quoted || quoted[0] != '"') return strdup(quoted);
    
    size_t len = strlen(quoted);
    if (len < 2 || quoted[len-1] != '"') return strdup(quoted);
    
    char *result = malloc(len - 1);
    if (!result) return NULL;
    
    size_t j = 0;
    for (size_t i = 1; i < len - 1; i++) {
        if (quoted[i] == '\\' && i + 1 < len - 1) {
            result[j++] = quoted[++i]; // Skip escape char, copy next
        } else {
            result[j++] = quoted[i];
        }
    }
    result[j] = '\0';
    return result;
}

// Helper to parse date string (simplified)
static time_t parse_cookie_date(const char *date_str) {
    if (!date_str) return 0;
    
    // Simple parsing for common cookie date formats
    // Format: "Tue, 19 Jan 2038 03:14:07 GMT" or similar
    int day, year, hour, min, sec;
    char month_str[4];
    
    // Try to parse the most common format
    if (sscanf(date_str, "%*3s, %d %3s %d %d:%d:%d", &day, month_str, &year, &hour, &min, &sec) == 6) {
        struct tm tm = {0};
        tm.tm_mday = day;
        tm.tm_year = year - 1900;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        
        // Convert month string to number
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        for (int i = 0; i < 12; i++) {
            if (strcmp(month_str, months[i]) == 0) {
                tm.tm_mon = i;
                break;
            }
        }
        return mktime(&tm);
    }
    
    // Try alternative format without day of week
    if (sscanf(date_str, "%d %3s %d %d:%d:%d", &day, month_str, &year, &hour, &min, &sec) == 6) {
        struct tm tm = {0};
        tm.tm_mday = day;
        tm.tm_year = year - 1900;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        for (int i = 0; i < 12; i++) {
            if (strcmp(month_str, months[i]) == 0) {
                tm.tm_mon = i;
                break;
            }
        }
        return mktime(&tm);
    }
    
    return 0; // Failed to parse
}
}

%type cookie_list {http_cookie_t*}
%type cookie {http_cookie_t*}
%type attribute_list {cookie_attribute_t*}
%type attribute {cookie_attribute_t*}
%type cookie_name {char*}
%type cookie_value {char*}

%destructor cookie_list { http_cookie_free($$); }
%destructor cookie { http_cookie_free($$); }
%destructor attribute_list { cookie_attribute_free_list($$); }
%destructor attribute { cookie_attribute_free($$); }
%destructor cookie_name { free($$); }
%destructor cookie_value { free($$); }

// Start rule
start ::= cookie_list(C). {
    ctx->result = C;
}

// Cookie list (for multiple cookies separated by commas - rare but valid)
cookie_list(A) ::= cookie(C). {
    A = C;
}

cookie_list(A) ::= cookie_list(L) COMMA cookie(C). {
    // Link cookies together
    C->next = L;
    A = C;
}

// Main cookie rule: name=value followed by optional attributes
cookie(A) ::= cookie_name(N) EQUALS cookie_value(V) attribute_list(ATTRS). {
    http_cookie_t *cookie = cookie_create(N, V);
    if (cookie) {
        cookie->attributes = ATTRS;
        // Process attributes into cookie fields
        cookie_attribute_t *attr = ATTRS;
        while (attr) {
            switch (attr->type) {
                case ATTR_DOMAIN:
                    cookie->domain = strdup(attr->value);
                    break;
                case ATTR_PATH:
                    cookie->path = strdup(attr->value);
                    break;
                case ATTR_EXPIRES:
                    cookie->expires = parse_cookie_date(attr->value);
                    break;
                case ATTR_MAX_AGE:
                    cookie->max_age = atoi(attr->value);
                    break;
                case ATTR_SECURE:
                    cookie->secure = 1;
                    break;
                case ATTR_HTTPONLY:
                    cookie->http_only = 1;
                    break;
                case ATTR_SAMESITE:
                    cookie->same_site = strdup(attr->value);
                    break;
            }
            attr = attr->next;
        }
    }
    free(N);
    free(V);
    A = cookie;
}

// Cookie name
cookie_name(A) ::= TOKEN(T). {
    A = T.text; // Transfer ownership
}

// Cookie value (can be token or quoted string)
cookie_value(A) ::= TOKEN(T). {
    A = T.text; // Transfer ownership
}

cookie_value(A) ::= QUOTED_VALUE(T). {
    A = unquote_string(T.text);
    free(T.text);
}

cookie_value(A) ::= NUMBER(T). {
    A = T.text; // Transfer ownership
}

// Attribute list
attribute_list(A) ::= . {
    A = NULL;
}

attribute_list(A) ::= attribute_list(L) SEMICOLON attribute(ATTR). {
    ATTR->next = L;
    A = ATTR;
}

// Individual attributes
attribute(A) ::= DOMAIN EQUALS TOKEN(T). {
    A = cookie_attribute_create(ATTR_DOMAIN, T.text);
    free(T.text);
}

attribute(A) ::= DOMAIN EQUALS QUOTED_VALUE(T). {
    char *unquoted = unquote_string(T.text);
    A = cookie_attribute_create(ATTR_DOMAIN, unquoted);
    free(unquoted);
    free(T.text);
}

attribute(A) ::= PATH EQUALS TOKEN(T). {
    A = cookie_attribute_create(ATTR_PATH, T.text);
    free(T.text);
}

attribute(A) ::= PATH EQUALS QUOTED_VALUE(T). {
    char *unquoted = unquote_string(T.text);
    A = cookie_attribute_create(ATTR_PATH, unquoted);
    free(unquoted);
    free(T.text);
}

attribute(A) ::= EXPIRES EQUALS DATE_VALUE(T). {
    A = cookie_attribute_create(ATTR_EXPIRES, T.text);
    free(T.text);
}

attribute(A) ::= MAX_AGE EQUALS NUMBER(T). {
    A = cookie_attribute_create(ATTR_MAX_AGE, T.text);
    free(T.text);
}

attribute(A) ::= SECURE. {
    A = cookie_attribute_create(ATTR_SECURE, NULL);
}

attribute(A) ::= HTTPONLY. {
    A = cookie_attribute_create(ATTR_HTTPONLY, NULL);
}

attribute(A) ::= SAMESITE EQUALS TOKEN(T). {
    A = cookie_attribute_create(ATTR_SAMESITE, T.text);
    free(T.text);
}

// Handle unknown attributes gracefully
attribute(A) ::= TOKEN(N) EQUALS TOKEN(V). {
    A = NULL; // Ignore unknown attributes
    free(N.text);
    free(V.text);
}

attribute(A) ::= TOKEN(N) EQUALS QUOTED_VALUE(V). {
    A = NULL; // Ignore unknown attributes
    free(N.text);
    free(V.text);
}

attribute(A) ::= TOKEN(T). {
    A = NULL; // Ignore unknown flag attributes
    free(T.text);
}

%syntax_error {
    ctx->error = 1;
    ctx->error_message = "Syntax error in cookie parsing";
}

%parse_failure {
    ctx->error = 1;
    ctx->error_message = "Parse failure in cookie parsing";
}