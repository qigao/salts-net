#include "llhttp.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase IRIS-1: Use turbo_arena instead of vendor arena */
#include "turbo_buffer.h"
#include "request.h"
#include "security.h"
#include "turbo_str.h"
#include <tlog.h>
// Internal implementation structure (hidden from public API)
struct http_parser_impl {
  llhttp_t parser;            // llhttp parser instance
  llhttp_settings_t settings; // llhttp parser settings
};

#define MIN_BUFFER_SIZE 64
#define GROWTH_FACTOR 1.5
#define MAX_SINGLE_ALLOCATION (10 * 1024 * 1024)
#define ABSOLUTE_MAX_REQUEST (50 * 1024 * 1024)
#define MAX_HEADER_SIZE (8 * 1024)
#define MAX_URL_LENGTH 2048
#define MAX_METHOD_LENGTH 16
#define MAX_HEADERS_COUNT 100
#define MAX_QUERY_PARAMS 100

// Calculate next buffer size for arena allocation
static size_t calculate_next_size(size_t current, size_t needed) {
  if (needed > ABSOLUTE_MAX_REQUEST) {
    TLOG_ERROR("Request too large: {} bytes", needed);
    return 0;
  }

  if (needed <= current)
    return current;

  size_t new_size = current < MIN_BUFFER_SIZE ? MIN_BUFFER_SIZE : current;

  while (new_size < needed) {
    // Check for overflow before multiplication
    if (new_size > SIZE_MAX / GROWTH_FACTOR) {
      new_size = needed + MIN_BUFFER_SIZE;
      break;
    }

    size_t next = (size_t)(new_size * GROWTH_FACTOR);
    if (next <= new_size) { // Overflow protection
      new_size = needed + MIN_BUFFER_SIZE;
      break;
    }
    new_size = next;
  }

  // Cap at maximum single allocation
  if (new_size > MAX_SINGLE_ALLOCATION && needed <= MAX_SINGLE_ALLOCATION) {
    new_size = MAX_SINGLE_ALLOCATION;
  }

  return new_size > ABSOLUTE_MAX_REQUEST ? ABSOLUTE_MAX_REQUEST : new_size;
}

// Buffer reallocation
/* Phase IRIS-1: Updated to use mem_pool_t */
static int ensure_buffer_capacity(mem_pool_t *arena, char **buffer, size_t *capacity,
                                  size_t current_length, size_t additional_needed) {
  if (!arena || !buffer || !capacity)
    return -1;

  size_t total_needed = current_length + additional_needed + 1;

  if (total_needed <= *capacity)
    return 0; // No reallocation needed

  if (total_needed > ABSOLUTE_MAX_REQUEST) {
    TLOG_ERROR("Request exceeds maximum size: {} bytes", total_needed);
    return -2;
  }

  size_t new_capacity = calculate_next_size(*capacity, total_needed);
  if (new_capacity == 0)
    return -2;

  /* Phase IRIS-1: turbo_arena doesn't have realloc, so alloc + memcpy */
  char *new_buffer = mem_alloc(arena, new_capacity);
  if (!new_buffer) {
    TLOG_ERROR("Arena buffer reallocation failed");
    return -1;
  }

  /* Copy old data if exists */
  if (*buffer && *capacity > 0) {
    memcpy(new_buffer, *buffer, *capacity);
  }

  *buffer = new_buffer;
  *capacity = new_capacity;
  return 0;
}

// llhttp callback for URL
static int on_url_cb(llhttp_t *parser, const char *at, size_t length) {
  if (!parser || !parser->data || !at)
    return 1;

  http_context_t *context = (http_context_t *)parser->data;

  // Check URL length limit
  if (context->url_length + length > MAX_URL_LENGTH) {
    TLOG_ERROR("URL too long: {} bytes", context->url_length + length);
    return 1;
  }

  if (ensure_buffer_capacity(context->arena, &context->url, &context->url_capacity,
                             context->url_length, length) != 0) {
    return 1;
  }

  memcpy(context->url + context->url_length, at, length);
  context->url_length += length;
  context->url[context->url_length] = '\0';

  return 0;
}

// llhttp callback for headers field
static int on_header_field_cb(llhttp_t *parser, const char *at, size_t length) {
  if (!parser || !parser->data || !at)
    return 1;

  http_context_t *context = (http_context_t *)parser->data;

  // Check header count limit
  if (context->headers.count >= MAX_HEADERS_COUNT) {
    TLOG_ERROR("Too many headers: {}", context->headers.count);
    return 1;
  }

  // Check header field size
  if (length > MAX_HEADER_SIZE) {
    TLOG_ERROR("Header field too large: {} bytes", length);
    return 1;
  }

  // Validate header field name using security module
  // Create a null-terminated string for validation
  char *temp_name = mem_alloc(context->arena, length + 1);
  if (!temp_name) {
    return 1;
  }
  memcpy(temp_name, at, length);
  temp_name[length] = '\0';

  const iris_security_limits_t *limits = iris_security_get_limits();
  iris_security_result_t validation_result =
      iris_validate_http_header_name(temp_name, limits->max_header_name_length);

  if (validation_result != IRIS_SECURITY_OK) {
    TLOG_ERROR("Invalid header field name: {}", iris_security_error_string(validation_result));
    return 1;
  }

  // Reset for new field
  context->header_field_length = 0;

  if (ensure_buffer_capacity(context->arena, &context->current_header_field,
                             &context->header_field_capacity, 0, length) != 0) {
    return 1;
  }

  memcpy(context->current_header_field, at, length);
  context->header_field_length = length;
  context->current_header_field[length] = '\0';

  return 0;
}

// Array growth
static int ensure_array_capacity(mem_pool_t *arena, request_t *array) {
  if (!arena || !array)
    return -1;

  if (array->count < array->capacity)
    return 0;

  // Check for overflow
  if (array->capacity > INT_MAX / 2)
    return -1;

  int new_capacity = array->capacity == 0 ? 16 : array->capacity * 2;

  /* Phase IRIS-1: turbo_arena doesn't have realloc, so alloc + memcpy */
  request_item_t *new_items = mem_alloc(arena, new_capacity * sizeof(request_item_t));
  if (!new_items) {
    TLOG_ERROR("Arena array reallocation failed");
    return -1;
  }

  /* Copy old data if exists */
  if (array->items && array->capacity > 0) {
    memcpy(new_items, array->items, array->capacity * sizeof(request_item_t));
  }

  // Initialize new elements
  for (int i = array->capacity; i < new_capacity; i++) {
    new_items[i].key = NULL;
    new_items[i].value = NULL;
  }

  array->items = new_items;
  array->capacity = new_capacity;
  return 0;
}

// llhttp callback for headers value
static int on_header_value_cb(llhttp_t *parser, const char *at, size_t length) {
  if (!parser || !parser->data || !at)
    return 1;

  http_context_t *context = (http_context_t *)parser->data;

  // Validate header field exists
  if (!context->current_header_field || context->header_field_length == 0)
    return 1;

  // Check header value size
  if (length > MAX_HEADER_SIZE) {
    TLOG_ERROR("Header value too large: {} bytes", length);
    return 1;
  }

  // Validate header value using security module
  // Create a null-terminated string for validation
  char *temp_value = mem_alloc(context->arena, length + 1);
  if (!temp_value) {
    return 1;
  }
  memcpy(temp_value, at, length);
  temp_value[length] = '\0';

  const iris_security_limits_t *limits = iris_security_get_limits();
  iris_security_result_t validation_result =
      iris_validate_http_header_value(temp_value, limits->max_header_value_length);

  if (validation_result != IRIS_SECURITY_OK) {
    TLOG_ERROR("Invalid header field value: {}", iris_security_error_string(validation_result));
    return 1;
  }

  if (ensure_array_capacity(context->arena, &context->headers) != 0)
    return 1;

  context->headers.items[context->headers.count].key =
      mem_strdup(context->arena, context->current_header_field);
  if (!context->headers.items[context->headers.count].key)
    return 1;

  char *value = mem_alloc(context->arena, length + 1);
  if (!value) {
    return 1;
  }

  memcpy(value, at, length);
  value[length] = '\0';

  context->headers.items[context->headers.count].value = value;
  context->headers.count++;

  // Handle Connection header for keep-alive
  if (context->current_header_field &&
      tstr_casecmp(context->current_header_field, "Connection") == 0) {
    if (length == 10 && tstr_ncasecmp(at, "keep-alive", 10) == 0) {
      context->keep_alive = 1;
    } else if (length == 5 && tstr_ncasecmp(at, "close", 5) == 0) {
      context->keep_alive = 0;
    }
  }

  return 0;
}

// llhttp callback for method
static int on_method_cb(llhttp_t *parser, const char *at, size_t length) {
  if (!parser || !parser->data || !at)
    return 1;

  http_context_t *context = (http_context_t *)parser->data;

  // Limit method length
  if (context->method_length + length > MAX_METHOD_LENGTH) {
    TLOG_ERROR("Method too long: {} bytes", context->method_length + length);
    return 1;
  }

  if (ensure_buffer_capacity(context->arena, &context->method, &context->method_capacity,
                             context->method_length, length) != 0) {
    return 1;
  }

  memcpy(context->method + context->method_length, at, length);
  context->method_length += length;
  context->method[context->method_length] = '\0';

  return 0;
}

// llhttp callback for body
static int on_body_cb(llhttp_t *parser, const char *at, size_t length) {
  if (!parser || !parser->data || !at)
    return 1;

  http_context_t *context = (http_context_t *)parser->data;

  // Check request body size limit before processing
  const iris_security_limits_t *limits = iris_security_get_limits();
  if (context->body_length + length > limits->max_request_body_size) {
    TLOG_ERROR("Request body too large: {} bytes (limit: {})", context->body_length + length,
               limits->max_request_body_size);
    return HPE_USER; // Payload too large
  }

  int result = ensure_buffer_capacity(context->arena, &context->body, &context->body_capacity,
                                      context->body_length, length);

  if (result == -2) {
    return HPE_USER; // Payload too large
  } else if (result != 0) {
    return 1;
  }

  if (context->stream_mode) {
    context->stream_chunk = at;
    context->stream_chunk_len = length;
    context->body_length += length;
    return HPE_PAUSED;
  }

  memcpy(context->body + context->body_length, at, length);
  context->body_length += length;
  context->body[context->body_length] = '\0';

  return 0;
}

// Callback for HTTP version detection
static int on_version_cb(llhttp_t *parser) {
  if (!parser || !parser->data)
    return 1;

  http_context_t *context = (http_context_t *)parser->data;

  context->http_major = parser->http_major;
  context->http_minor = parser->http_minor;

  // Set default keep-alive based on HTTP version ONLY if Connection header
  // was not explicitly set. keep_alive == -1 means "not set by header".
  if (context->keep_alive == -1) {
    context->keep_alive = (parser->http_major == 1 && parser->http_minor >= 1) ? 1 : 0;
  }

  return 0;
}

static int on_headers_complete_cb(llhttp_t *parser) {
  http_context_t *context;
  int version_result;

  if (!parser || !parser->data) {
    return HPE_USER;
  }

  context = (http_context_t *)parser->data;
  version_result = on_version_cb(parser);
  if (version_result != 0) {
    return version_result;
  }

  context->headers_complete = 1;
  return HPE_PAUSED;
}

static int on_message_complete_cb(llhttp_t *parser) {
  http_context_t *context;

  if (!parser || !parser->data) {
    return HPE_USER;
  }

  context = (http_context_t *)parser->data;
  context->message_complete = 1;
  return HPE_PAUSED;
}

// Initialize HTTP context
void http_context_init(http_context_t *context, mem_pool_t *arena) {
  if (!context || !arena)
    return;

  memset(context, 0, sizeof(http_context_t));
  context->arena = arena;

  // Allocate parser implementation from arena
  context->parser_impl = mem_alloc(arena, sizeof(http_parser_impl_t));
  if (!context->parser_impl)
    return;

  // Initialize security module with default limits
  iris_security_init(NULL);

  // Initialize llhttp parser
  llhttp_settings_init(&context->parser_impl->settings);

  // Set up callbacks
  context->parser_impl->settings.on_url = on_url_cb;
  context->parser_impl->settings.on_header_field = on_header_field_cb;
  context->parser_impl->settings.on_header_value = on_header_value_cb;
  context->parser_impl->settings.on_method = on_method_cb;
  context->parser_impl->settings.on_body = on_body_cb;
  context->parser_impl->settings.on_headers_complete = on_headers_complete_cb;
  context->parser_impl->settings.on_message_complete = on_message_complete_cb;

  llhttp_init(&context->parser_impl->parser, HTTP_REQUEST, &context->parser_impl->settings);

  context->parser_impl->parser.data = context;

  // Single allocation for all initial char buffers: url + method + header_field + body
  context->url_capacity = 256;
  context->method_capacity = 16;
  context->header_field_capacity = 64;
  context->body_capacity = 512;
  size_t total_buf = context->url_capacity + context->method_capacity +
                     context->header_field_capacity + context->body_capacity;
  char *buf = mem_alloc(arena, total_buf);
  if (!buf)
    return;
  memset(buf, 0, total_buf);

  context->url = buf;
  context->method = buf + context->url_capacity;
  context->current_header_field = buf + context->url_capacity + context->method_capacity;
  context->body = buf + context->url_capacity + context->method_capacity +
                  context->header_field_capacity;
  context->url_length = 0;
  context->method_length = 0;
  context->header_field_length = 0;
  context->body_length = 0;

  // Initialize header array (separate alloc — needs pointer alignment)
  context->headers.count = 0;
  context->headers.capacity = 16;
  context->headers.items =
      mem_alloc(arena, context->headers.capacity * sizeof(request_item_t));
  if (context->headers.items) {
    memset(context->headers.items, 0, context->headers.capacity * sizeof(request_item_t));
  }

  // Other fields start empty
  memset(&context->query_params, 0, sizeof(request_t));
  memset(&context->url_params, 0, sizeof(request_t));

  context->keep_alive = -1; // -1 = not set, will be set by on_version_cb or Connection header
  context->http_major = 1;
  context->http_minor = 0;
  context->headers_complete = 0;
  context->message_complete = 0;
  context->stream_mode = 0;
  context->stream_chunk = NULL;
  context->stream_chunk_len = 0;
}

// Function to clean up HTTP context (arena-aware - just clears pointers)
void http_context_free(http_context_t *context) {
  if (!context)
    return;

  // Clear pointers - arena handles the memory
  context->arena = NULL;
  context->url = NULL;
  context->method = NULL;
  context->current_header_field = NULL;
  context->body = NULL;

  context->headers.items = NULL;
  context->headers.count = 0;
  context->headers.capacity = 0;

  context->query_params.items = NULL;
  context->query_params.count = 0;
  context->query_params.capacity = 0;

  context->url_params.items = NULL;
  context->url_params.count = 0;
  context->url_params.capacity = 0;

  memset(context, 0, sizeof(http_context_t));
}

int http_context_execute(http_context_t *context, const char *data, size_t len, size_t *consumed) {
  enum llhttp_errno err;
  const char *error_pos;

  if (!context || !context->parser_impl || !consumed) {
    return -400;
  }

  if (!data) {
    data = "";
    len = 0;
  }

  context->headers_complete = 0;
  context->message_complete = 0;
  context->stream_chunk = NULL;
  context->stream_chunk_len = 0;
  err = llhttp_execute(&context->parser_impl->parser, data, len);

  if (err == HPE_OK) {
    *consumed = len;
    return context->message_complete ? 1 : 0;
  }

  if (err == HPE_PAUSED && context->message_complete) {
    error_pos = llhttp_get_error_pos(&context->parser_impl->parser);
    if (!error_pos) {
      return -400;
    }
    *consumed = (size_t)(error_pos - data);
    return 1;
  }

  if (err == HPE_PAUSED && context->stream_chunk && context->stream_chunk_len > 0) {
    if (context->stream_chunk < data) {
      return -400;
    }
    *consumed = (size_t)((context->stream_chunk + context->stream_chunk_len) - data);
    return 3;
  }

  if (err == HPE_PAUSED && context->headers_complete) {
    error_pos = llhttp_get_error_pos(&context->parser_impl->parser);
    if (!error_pos) {
      return -400;
    }
    *consumed = (size_t)(error_pos - data);
    return 2;
  }

  if (err == HPE_USER) {
    error_pos = llhttp_get_error_pos(&context->parser_impl->parser);
    *consumed = error_pos ? (size_t)(error_pos - data) : 0;
    return -413;
  }

  error_pos = llhttp_get_error_pos(&context->parser_impl->parser);
  *consumed = error_pos ? (size_t)(error_pos - data) : 0;
  return -400;
}

void http_context_resume(http_context_t *context) {
  if (!context || !context->parser_impl) {
    return;
  }

  llhttp_resume(&context->parser_impl->parser);
}

// Query parsing
void parse_query(mem_pool_t *arena, const char *query_string, request_t *query) {
  if (!arena || !query)
    return;

  memset(query, 0, sizeof(request_t));

  if (!query_string || strlen(query_string) == 0)
    return;

  size_t query_len = strlen(query_string);
  if (query_len > MAX_URL_LENGTH) {
    TLOG_ERROR("Query string too long: {} bytes", query_len);
    return;
  }

  // Count parameters
  int param_count = 1;
  for (const char *p = query_string; *p; p++) {
    if (*p == '&')
      param_count++;
  }

  // Limit parameters
  if (param_count > MAX_QUERY_PARAMS) {
    TLOG_ERROR("Too many query parameters: {}", param_count);
    param_count = MAX_QUERY_PARAMS;
  }

  query->capacity = param_count;
  query->items = mem_alloc(arena, query->capacity * sizeof(request_item_t));
  if (!query->items) {
    query->capacity = 0;
    return;
  }

  // Initialize items
  for (int i = 0; i < query->capacity; i++) {
    query->items[i].key = NULL;
    query->items[i].value = NULL;
  }

  char *buffer = mem_alloc(arena, query_len + 1);
  if (!buffer) {
    query->items = NULL;
    query->capacity = 0;
    return;
  }

  memcpy(buffer, query_string, query_len);
  buffer[query_len] = '\0';

  char *cursor = buffer;
  while (*cursor && query->count < query->capacity) {
    char *pair = cursor;
    char *amp = strchr(cursor, '&');
    if (amp) {
      *amp = '\0';
      cursor = amp + 1;
    } else {
      cursor += strlen(cursor);
    }

    if (pair[0] == '\0')
      continue;

    char *eq = strchr(pair, '=');
    if (!eq)
      continue;

    *eq = '\0';
    query->items[query->count].key = mem_strdup(arena, pair);
    query->items[query->count].value = mem_strdup(arena, eq + 1);

    if (query->items[query->count].key && query->items[query->count].value) {
      query->count++;
    }
  }
}

// Get value by key
const char *get_req(const request_t *request, const char *key) {
  if (!request || !request->items || !key)
    return NULL;

  for (int i = 0; i < request->count; i++) {
    if (request->items[i].key && strcmp(request->items[i].key, key) == 0)
      return request->items[i].value;
  }
  return NULL;
}
