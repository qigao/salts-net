#ifndef REQUEST_H
#define REQUEST_H

#include <stdbool.h>
#include <stddef.h>
#include "compat.h"
#include "turbo_buffer.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration - hides llhttp implementation details
typedef struct http_parser_impl http_parser_impl_t;

typedef struct
{
    char *key;
    char *value;
} request_item_t;

typedef struct
{
    request_item_t *items;
    int count;
    int capacity;
} request_t;

// HTTP parsing context structure to hold state during parsing
typedef struct
{
    mem_pool_t *arena;       // Arena for this context's memory
    http_parser_impl_t *parser_impl; // Opaque parser implementation (hides llhttp)

    // Dynamic URL parsing state
    char *url;           // Dynamic URL buffer
    size_t url_length;   // Current URL length
    size_t url_capacity; // URL buffer capacity

    char *method;           // Dynamic method buffer
    size_t method_length;   // Current method length
    size_t method_capacity; // Method buffer capacity

    // Request data containers
    request_t headers;      // Headers container
    request_t query_params; // Query parameters container
    request_t url_params;   // URL parameters container

    // Body parsing
    char *body;           // Request body buffer
    size_t body_length;   // Body length
    size_t body_capacity; // Body buffer capacity

    // Keep-alive tracking
    int keep_alive; // 1 for keep-alive, 0 for close
    int headers_complete; // 1 when headers are fully parsed
    int message_complete; // 1 when llhttp reached end of one full message
    int stream_mode; // 1 when body should be exposed chunk-by-chunk instead of buffered
    const char *stream_chunk;
    size_t stream_chunk_len;

    // HTTP version
    int http_major; // Major HTTP version
    int http_minor; // Minor HTTP version

    // Temporary header parsing
    char *current_header_field;   // Dynamic current header field buffer
    size_t header_field_length;   // Current header field length
    size_t header_field_capacity; // Header field buffer capacity
} http_context_t;

// Function to initialize the http context 
CXX_C_API void http_context_init(http_context_t *context, mem_pool_t *arena);

// Function to cleanup the http context
CXX_C_API void http_context_free(http_context_t *context);

// Feed bytes into an initialized HTTP context incrementally.
// Returns:
//   0 when more bytes are needed
//   1 when a full message has been parsed
//   2 when request headers are complete and parsing paused at the body boundary
//   3 when stream_mode is enabled and a body chunk is ready in stream_chunk/stream_chunk_len
// and a negative HTTP status code on parse error.
CXX_C_API int http_context_execute(http_context_t *context, const char *data, size_t len,
                                   size_t *consumed);
CXX_C_API void http_context_resume(http_context_t *context);

// Parse the query string into request_t structure
/* Phase IRIS-1: Updated to use mem_pool_t */
CXX_C_API void parse_query(mem_pool_t *arena, const char *query_string, request_t *query);

// Get value by key from request_t structure
CXX_C_API const char *get_req(const request_t *request, const char *key);

#ifdef __cplusplus
}
#endif

#endif
