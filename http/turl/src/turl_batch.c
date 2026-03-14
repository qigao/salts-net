/**
 * @file turl_batch.c
 * @brief Batch download implementation with concurrent coroutines
 */

#include "turl_batch.h"
#include "turl_common.h"
#include <http_client.h>
#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlog.h>
#include <turbo_coro.h>
#include "CoroNet/turbo_coro_context.h"


/**
 * Batch download state (single-threaded, no mutex needed)
 */
typedef struct {
  char **urls;
  int url_count;
  int current_index;
  int completed;
  int verbose;
  int follow_redirects;
  const char **headers;
  uint32_t header_count;
  json_value_t *context;
  char *output_directory;
} batch_download_ctx_t;

static void batch_worker(coro_t *co, void *arg) {
  UNUSED(co);
  batch_download_ctx_t *ctx = (batch_download_ctx_t *)arg;

  http_client_t *client = http_client_create(NULL);
  if (!client) {
    TLOG_ERROR("Failed to create HTTP client for batch worker");
    return;
  }
  http_client_follow_redirects(client, ctx->follow_redirects);

  while (ctx->current_index < ctx->url_count) {
    int idx = ctx->current_index++;
    char *raw_url = ctx->urls[idx];

    if (ctx->verbose) {
      TLOG_INFO("Starting download [{}/{}]: {}", idx + 1, ctx->url_count, raw_url);
    }

    char *rendered_url = turl_render_template(raw_url, ctx->context);

    http_response_t *resp =
        http_request(client, HTTP_GET, rendered_url, ctx->headers, ctx->header_count, NULL, 0);

    if (resp->error) {
      TLOG_ERROR("Download failed [{}]: {} (code: {})", rendered_url, resp->error,
                 ENUM_NAME(resp->error_code));
    } else {
      if (ctx->verbose) {
        TLOG_INFO("Download finished [{}]: {} OK ({} bytes)", rendered_url, resp->status_code,
                  resp->body_len);
      }

      const char *filename = strrchr(rendered_url, '/');
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
        fwrite(resp->body, 1, resp->body_len, f);
        fclose(f);
        if (ctx->verbose)
          TLOG_INFO("* Written to {}", path);
      } else {
        TLOG_ERROR("Failed to open output file: {}", path);
      }
    }

    http_response_free(resp);
    free(rendered_url);

    ctx->completed++;
  }

  http_client_destroy(client);
}

int turl_batch_download(const char *input_file, int64_t concurrency, char **headers,
                        uint32_t header_count, json_value_t *mustache_context,
                        const char *output_directory, int follow_redirects, int verbose) {
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

  batch_download_ctx_t batch_ctx = {
      .urls = urls,
      .url_count = url_count,
      .current_index = 0,
      .completed = 0,
      .verbose = verbose,
      .follow_redirects = follow_redirects,
      .headers = (header_count > 0) ? (const char **)rendered_headers : NULL,
      .header_count = header_count,
      .context = mustache_context,
      .output_directory = (char *)output_directory,
  };

  // Spawn worker tasks using the context API
  int worker_count = (int)(concurrency < url_count ? concurrency : url_count);
  coro_context_t *ctx = coro_context_current();
  coro_task_t **tasks = calloc(worker_count, sizeof(coro_task_t *));

  for (int i = 0; i < worker_count; i++) {
    tasks[i] = coro_task_create(ctx, batch_worker, &batch_ctx);
    coro_task_start(tasks[i]);
  }

  // Wait for all workers to finish (fan-in)
  coro_when_all(ctx, tasks, worker_count);

  // Cleanup tasks
  for (int i = 0; i < worker_count; i++)
    coro_task_destroy(tasks[i]);
  free(tasks);

  for (int i = 0; i < url_count; i++)
    free(urls[i]);
  free(urls);
  for (uint32_t i = 0; i < header_count; i++)
    free(rendered_headers[i]);

  if (verbose)
    TLOG_INFO("Batch download complete. {} files processed.", batch_ctx.completed);

  return 0;
}
