/**
 * @file turl_batch.c
 * @brief Batch download implementation with concurrent requests
 */

#include "turl_batch.h"
#include "turl_common.h"
#include <http_client_async.h>
#include <platform.h>
#include <tlog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Mutex wrapper using platform.h
typedef turbo_mutex_t turl_mutex_t;
#define TURL_MUTEX_INIT(m) turbo_mutex_init((turbo_mutex_t*)(m))
#define TURL_MUTEX_LOCK(m) turbo_mutex_lock((turbo_mutex_t*)(m))
#define TURL_MUTEX_UNLOCK(m) turbo_mutex_unlock((turbo_mutex_t*)(m))
#define TURL_MUTEX_DESTROY(m) turbo_mutex_destroy((turbo_mutex_t*)(m))

/**
 * Batch download state
 */
typedef struct {
  char **urls;
  int url_count;
  int current_index;
  int active_requests;
  int64_t concurrency;
  int verbose;
  int completed;
  turl_mutex_t mutex;
  const char **headers;
  uint32_t header_count;
  json_value_t *context;
  char *output_directory;
} batch_download_ctx_t;

static void start_next_batch_request(batch_download_ctx_t *ctx, http_async_client_t *client);

typedef struct {
  batch_download_ctx_t *batch;
  char *url;
  http_async_client_t *client;
} per_request_ctx_t;

static void batch_callback_wrapper(http_async_request_t *request, http_async_response_t *response,
                                   void *user_data) {
  per_request_ctx_t *req_ctx = (per_request_ctx_t *)user_data;
  batch_download_ctx_t *ctx = req_ctx->batch;
  char *url = req_ctx->url;
  http_async_client_t *client = req_ctx->client;

  TURL_MUTEX_LOCK(&ctx->mutex);
  ctx->active_requests--;
  ctx->completed++;
  TURL_MUTEX_UNLOCK(&ctx->mutex);

  if (response->error) {
    TLOG_ERROR("Download failed [{}]: {} (code: {})", url, response->error,ENUM_NAME(response->error_code));
  } else {
    if (ctx->verbose) {
      TLOG_INFO("Download finished [{}]: {} OK ({} bytes)", url, response->status_code, response->body_len);
    }

    const char *filename = strrchr(url, '/');
    if (filename)
      filename++;
    else
      filename = "downloaded_file";
    if (!filename || !filename[0] || filename[0] == '?')
      filename = "index.html";

    // Strip query params if any for filename
    char *q = strchr(filename, '?');
    size_t name_len = q ? (size_t)(q - filename) : strlen(filename);
    char safe_name[256];
    if (name_len >= sizeof(safe_name))
      name_len = sizeof(safe_name) - 1;
    strncpy(safe_name, filename, name_len);
    safe_name[name_len] = '\0';

    char path[1024];
    if (ctx->output_directory) {
      turl_ensure_directory_exists(ctx->output_directory);
      snprintf(path, sizeof(path), "%s/%s", ctx->output_directory, safe_name);
    } else {
      strncpy(path, safe_name, sizeof(path));
    }

    FILE *f = fopen(path, "wb");
    if (f) {
      fwrite(response->body, 1, response->body_len, f);
      fclose(f);
      if (ctx->verbose)
        TLOG_INFO("* Written to {}", path);
    } else {
      TLOG_ERROR("Failed to open output file: {}", path);
    }
  }

  free(url);
  free(req_ctx);
  
  // Reuse this client for the next request
  start_next_batch_request(ctx, client);
}

static void start_next_batch_request(batch_download_ctx_t *ctx, http_async_client_t *client) {
  char *raw_url = NULL;
  
  TURL_MUTEX_LOCK(&ctx->mutex);
  if (ctx->current_index < ctx->url_count) {
    if (ctx->verbose) {
      TLOG_INFO("Starting download [{}/{}]: {}", ctx->current_index + 1, ctx->url_count, ctx->urls[ctx->current_index]);
    }
    raw_url = ctx->urls[ctx->current_index++];
    ctx->active_requests++;
  }
  TURL_MUTEX_UNLOCK(&ctx->mutex);

  if (!raw_url) {
    return;
  }

  char *rendered_url = turl_render_template(raw_url, ctx->context);
  
  per_request_ctx_t *req_ctx = malloc(sizeof(per_request_ctx_t));
  req_ctx->batch = ctx;
  req_ctx->url = rendered_url;
  req_ctx->client = client;

  http_async_request(client, HTTP_GET, rendered_url, ctx->headers, ctx->header_count, NULL, 0,
                      batch_callback_wrapper, req_ctx);
}

int turl_batch_download(const char *input_file, int64_t concurrency,
                        char **headers, uint32_t header_count,
                        json_value_t *mustache_context,
                        const char *output_directory,
                        int follow_redirects, int verbose) {
    FILE *f = fopen(input_file, "r");
    if (!f) {
      TLOG_ERROR("Failed to open input file: {}", input_file);
      return 1;
    }

    char **urls = NULL;
    int url_count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
      }
      if (len > 0) {
        urls = realloc(urls, sizeof(char *) * (url_count + 1));
        urls[url_count++] = strdup(line);
      }
    }
    fclose(f);

    if (url_count == 0) {
      if (verbose)
        TLOG_INFO("No URLs found in input file");
      return 0;
    }

    // Render templates for headers
    char *rendered_headers[100];
    for (uint32_t i = 0; i < header_count; i++) {
        rendered_headers[i] = turl_render_template(headers[i], mustache_context);
    }

    http_async_client_t **clients = calloc(concurrency, sizeof(http_async_client_t *));
    batch_download_ctx_t batch_ctx = {
        .urls = urls,
        .url_count = url_count,
        .current_index = 0,
        .active_requests = 0,
        .concurrency = concurrency,
        .verbose = verbose,
        .completed = 0,
        .headers = (header_count > 0) ? (const char **)rendered_headers : NULL,
        .header_count = header_count,
        .context = mustache_context,
        .output_directory = (char*)output_directory
    };
    TURL_MUTEX_INIT(&batch_ctx.mutex);

    // Initialize clients and start requests
    for (int i = 0; i < concurrency; i++) {
        clients[i] = http_async_client_create();
        http_async_client_follow_redirects(clients[i], follow_redirects);
        start_next_batch_request(&batch_ctx, clients[i]);
    }

    // Wait for all requests to complete
    while (1) {
      int done = 0;
      TURL_MUTEX_LOCK(&batch_ctx.mutex);
      if (batch_ctx.completed >= batch_ctx.url_count)
        done = 1;
      TURL_MUTEX_UNLOCK(&batch_ctx.mutex);
      
      if (done) break;

      // Sleep briefly
      turbo_sleep_ms(10);
    }

    // Cleanup batch resources
    for (int i = 0; i < url_count; i++)
      free(urls[i]);
    free(urls);
    for (uint32_t i = 0; i < header_count; i++)
        free(rendered_headers[i]);
        
    for (int i = 0; i < concurrency; i++) {
        http_async_client_destroy(clients[i]);
    }
    free(clients);
    TURL_MUTEX_DESTROY(&batch_ctx.mutex);

    if (verbose)
      TLOG_INFO("Batch download complete. {} files processed.", batch_ctx.completed);

    return 0;
}
