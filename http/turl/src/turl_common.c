/**
 * @file turl_common.c
 * @brief Common utilities implementation for turl
 */

#include "turl_common.h"
#include <json_parser.h>
#include <mustache_json.h>
#include <tlog.h>
#include <turbo_fs.h>
#include <stdio.h>
#include <string.h>

// Global logger for turl
tlog_t *g_turl_logger = NULL;

void turl_setup_logger(int verbose) {
    tlog_config_t config = {
        .min_level = verbose ? TURBO_LOG_LEVEL_DEBUG : TURBO_LOG_LEVEL_INFO,
        .async_mode = 0,  // Sync mode for CLI tool
        .buffer_size = 0,
        .pool_size = 0
    };
    
    g_turl_logger = tlog_create(&config);
    
    turbo_console_sink_opts_t sink_opts = {
        .output = stdout,
        .use_colors = 1,
        .pattern = "[{time}] [{level}] {message}",
        .include_timestamp = 1,
        .include_thread_id = 0,
        .include_file_line = 0
    };
    
    turbo_log_sink_t *console_sink = turbo_sink_console_create(&sink_opts);
    tlog_add_sink(g_turl_logger, console_sink);
    tlog_set_default(g_turl_logger);
}

void turl_cleanup_logger(void) {
    if (g_turl_logger) {
        tlog_destroy(g_turl_logger);
        g_turl_logger = NULL;
    }
}

void turl_ensure_directory_exists(const char *path) {
    if (!path) return;
    turbo_fs_stat_t stat;
    if (turbo_fs_stat_sync(path, &stat) != 0) {
        turbo_fs_mkdir_sync(path, 0755);
    }
}

char *turl_render_template(const char *template_str, json_value_t *context) {
  if (!template_str || !context) {
    return template_str ? strdup(template_str) : NULL;
  }

  MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
  if (!template) {
    return strdup(template_str);
  }

  MUSTACHE_STRING_RENDERER renderer;
  if (mustache_string_renderer_init(&renderer) != 0) {
    mustache_release(template);
    return strdup(template_str);
  }

  if (mustache_render_json(template, context, &renderer.base, &renderer, NULL, NULL) != 0) {
    mustache_string_renderer_free(&renderer);
    mustache_release(template);
    return strdup(template_str);
  }

  char *result = mustache_string_renderer_get(&renderer);
  mustache_string_renderer_free(&renderer);
  mustache_release(template);

  return result;
}

void turl_print_body(const char *body, size_t len, const char *content_type) {
  if (!body || len == 0)
    return;

  int is_json = (content_type && strstr(content_type, "application/json"));

  if (is_json) {
    json_value_t *json = json_parse(body, len);
    if (json) {
      char *pretty = json_serialize_pretty(json, NULL);
      if (pretty) {
        printf("%s\n", pretty);
        json_serialize_free(pretty);
        json_free(json);
        return;
      }
      json_free(json);
    }
  }

  // Fallback to verbatim
  fwrite(body, 1, len, stdout);
  printf("\n");
}
