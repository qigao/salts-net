/**
 * @file cookie_parser.c
 * @brief Implementation of RFC 6265 compliant cookie parser
 */

#include "cookie_parser.h"
#include "cookie_jar.h"
#include "turbo_parser.h"
#include "turbo_str.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

// Include generated parser (following JSON parser naming convention)
#include "cookie_parser_gen.h"

#include "tlog.h"

// High-level parsing function
http_cookie_t *parse_set_cookie_rfc(const char *set_cookie_value) {
    if (!set_cookie_value) return NULL;
    TLOG_INFO("Parsing cookie string: {}", set_cookie_value);

    cookie_lexer_t lexer;
    cookie_parser_context_t ctx = {0};

    cookie_lexer_init(&lexer, set_cookie_value);

    void *parser = CookieParseAlloc(malloc);
    if (!parser) return NULL;

    cookie_token_t token;
    while (cookie_lexer_next(&lexer, &token) != COOKIE_EOF) {
        TLOG_DEBUG("Token: {} ({})", (int)token.type, token.text);
        CookieParse(parser, token.type, token, &ctx);
        if (ctx.error) {
            TLOG_ERROR("Parser error encountered at token: {}", token.text);
            cookie_token_free(&token);
            break;
        }
        // Don't free token here - parser takes ownership
    }

    // Signal end of input
    cookie_token_t end_token = {0};
    CookieParse(parser, 0, end_token, &ctx);

    CookieParseFree(parser, free);
    cookie_lexer_cleanup(&lexer);

    if (ctx.error) {
        TLOG_ERROR("Cookie parsing failed with final error");
    } else {
        TLOG_INFO("Cookie parsed successfully: {}", ctx.result ? ctx.result->name : "NULL");
    }
    return ctx.error ? NULL : ctx.result;
}

// Cookie utility functions
void http_cookie_free(http_cookie_t *cookie) {
    if (!cookie) return;
    
    free(cookie->name);
    free(cookie->value);
    free(cookie->domain);
    free(cookie->path);
    free(cookie->same_site);
    cookie_attribute_free_list(cookie->attributes);
    
    // Don't free next - caller manages the list
    free(cookie);
}

void cookie_attribute_free(cookie_attribute_t *attr) {
    if (!attr) return;
    
    free(attr->value);
    free(attr);
}

void cookie_attribute_free_list(cookie_attribute_t *list) {
    while (list) {
        cookie_attribute_t *next = list->next;
        cookie_attribute_free(list);
        list = next;
    }
}

// Domain matching according to RFC 6265
int domain_matches(const char *host, const char *cookie_domain) {
    if (!host || !cookie_domain) return 0;
    
    // Exact match
    if (tstr_casecmp(host, cookie_domain) == 0) return 1;
    
    // Domain attribute must start with '.'
    if (cookie_domain[0] != '.') return 0;
    
    size_t host_len = strlen(host);
    size_t domain_len = strlen(cookie_domain);
    
    // Host must be longer than domain
    if (host_len <= domain_len) return 0;
    
    // Check if host ends with cookie domain
    const char *host_suffix = host + (host_len - domain_len);
    if (tstr_casecmp(host_suffix, cookie_domain) != 0) return 0;
    
    // Ensure we're matching at a domain boundary
    if (host[host_len - domain_len - 1] != '.') return 0;
    
    return 1;
}

// Path matching according to RFC 6265
int path_matches(const char *request_path, const char *cookie_path) {
    if (!request_path || !cookie_path) return 0;
    
    // Default path is "/"
    if (strlen(cookie_path) == 0) cookie_path = "/";
    
    size_t cookie_path_len = strlen(cookie_path);
    size_t request_path_len = strlen(request_path);
    
    // Cookie path must be prefix of request path
    if (strncmp(request_path, cookie_path, cookie_path_len) != 0) return 0;
    
    // Exact match
    if (request_path_len == cookie_path_len) return 1;
    
    // Cookie path ends with '/' or next char in request path is '/'
    if (cookie_path[cookie_path_len - 1] == '/' || 
        request_path[cookie_path_len] == '/') return 1;
    
    return 0;
}

// Check if cookie matches the request URL
int cookie_matches_request(http_cookie_t *cookie, const char *url) {
    if (!cookie || !url) return 0;
    
    uri_t *uri = NULL;
    if (turbo_parse_uri((const uint8_t *)url, strlen(url), &uri) != 0) {
        return 0;
    }
    
    const char *host = turbo_uri_host(uri);
    const char *path = turbo_uri_path(uri);
    const char *scheme = turbo_uri_scheme(uri);
    
    int matches = 1;
    
    // Check domain
    if (cookie->domain) {
        if (!domain_matches(host, cookie->domain)) {
            matches = 0;
        }
    }
    
    // Check path
    if (matches && cookie->path) {
        if (!path_matches(path, cookie->path)) {
            matches = 0;
        }
    }
    
    // Check secure flag
    if (matches && cookie->secure) {
        if (tstr_casecmp(scheme, "https") != 0) {
            matches = 0;
        }
    }
    
    // Check expiration
    if (matches && cookie->expires > 0) {
        if (time(NULL) > cookie->expires) {
            matches = 0;
        }
    }
    
    // Check max-age (takes precedence over expires)
    if (matches && cookie->max_age >= 0) {
        // For simplicity, we'd need to store creation time to implement this properly
        // This is a simplified check
        if (cookie->max_age == 0) {
            matches = 0;
        }
    }
    
    turbo_free_uri(&uri);
    return matches;
}

// Build Cookie header with proper filtering
char *build_cookie_header_rfc(struct http_cookie_jar_s *jar, const char *url) {
    if (!jar || !url) return NULL;
    
    // Calculate total size for matching cookies
    size_t total_size = 0;
    int matching_count = 0;
    
    http_cookie_t *cookie = jar->cookies;
    while (cookie) {
        if (cookie_matches_request(cookie, url)) {
            total_size += strlen(cookie->name) + strlen(cookie->value) + 3; /* name=value; */
            matching_count++;
        }
        cookie = cookie->next;
    }

    if (matching_count == 0)
        return NULL;

    // Build header
    char *header = malloc(total_size + 10); /* "Cookie: " + size */
    if (!header)
        return NULL;

    strcpy(header, "Cookie: ");
    char *p = header + 8;

    cookie = jar->cookies;
    int first = 1;
    while (cookie) {
        if (cookie_matches_request(cookie, url)) {
            if (!first) {
                *p++ = ';';
                *p++ = ' ';
            }
            strcpy(p, cookie->name);
            p += strlen(cookie->name);
            *p++ = '=';
            strcpy(p, cookie->value);
            p += strlen(cookie->value);
            first = 0;
        }
        cookie = cookie->next;
    }
    *p = '\0';

    return header;
}

// Add parsed cookie to jar
void http_cookie_jar_add_parsed(struct http_cookie_jar_s *jar, http_cookie_t *cookie) {
    if (!jar || !cookie) return;
    
    // Check if cookie already exists and replace it
    http_cookie_t **prev = &jar->cookies;
    http_cookie_t *existing = jar->cookies;

    while (existing) {
        if (strcmp(existing->name, cookie->name) == 0) {
            // Check domain and path matching for replacement
            int domain_match = (!existing->domain && !cookie->domain) ||
                              (existing->domain && cookie->domain && 
                               strcmp(existing->domain, cookie->domain) == 0);
            int path_match = (!existing->path && !cookie->path) ||
                            (existing->path && cookie->path && 
                             strcmp(existing->path, cookie->path) == 0);
            
            if (domain_match && path_match) {
                // Replace existing cookie
                *prev = existing->next;
                http_cookie_free(existing);
                jar->count--;
                break;
            }
        }
        prev = &existing->next;
        existing = existing->next;
    }

    // Add new cookie to jar
    cookie->next = jar->cookies;
    jar->cookies = cookie;
    jar->count++;
}

// Clean up expired cookies
void http_cookie_jar_cleanup_expired(struct http_cookie_jar_s *jar) {
    if (!jar) return;
    
    time_t now = time(NULL);
    http_cookie_t **prev = &jar->cookies;
    http_cookie_t *cookie = jar->cookies;

    while (cookie) {
        int should_remove = 0;
        
        // Check expiration
        if (cookie->expires > 0 && now > cookie->expires) {
            should_remove = 1;
        }
        
        // Check max-age (simplified - would need creation timestamp for full implementation)
        if (cookie->max_age == 0) {
            should_remove = 1;
        }
        
        if (should_remove) {
            *prev = cookie->next;
            http_cookie_t *next = cookie->next;
            http_cookie_free(cookie);
            jar->count--;
            cookie = next;
        } else {
            prev = &cookie->next;
            cookie = cookie->next;
        }
    }
}

// Helper function to normalize domain (add leading dot if needed)
static char *normalize_domain(const char *domain) {
    if (!domain) return NULL;
    
    if (domain[0] == '.') {
        return strdup(domain);
    }
    
    size_t len = strlen(domain);
    char *normalized = malloc(len + 2);
    if (!normalized) return NULL;
    
    normalized[0] = '.';
    strcpy(normalized + 1, domain);
    return normalized;
}

// Helper function to normalize path
static char *normalize_path(const char *path) {
    if (!path || strlen(path) == 0) {
        return strdup("/");
    }
    
    if (path[0] != '/') {
        size_t len = strlen(path);
        char *normalized = malloc(len + 2);
        if (!normalized) return NULL;
        
        normalized[0] = '/';
        strcpy(normalized + 1, path);
        return normalized;
    }
    
    return strdup(path);
}

// Enhanced cookie creation with normalization
http_cookie_t *http_cookie_create_normalized(const char *name, const char *value, 
                                           const char *domain, const char *path) {
    http_cookie_t *cookie = calloc(1, sizeof(http_cookie_t));
    if (!cookie) return NULL;
    
    cookie->name = strdup(name);
    cookie->value = strdup(value);
    
    if (domain) {
        cookie->domain = normalize_domain(domain);
    }
    
    if (path) {
        cookie->path = normalize_path(path);
    } else {
        cookie->path = strdup("/");
    }
    
    cookie->expires = 0;
    cookie->max_age = -1;
    cookie->secure = 0;
    cookie->http_only = 0;
    
    return cookie;
}