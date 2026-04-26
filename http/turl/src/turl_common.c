/**
 * @file turl_common.c
 * @brief Common utilities implementation for turl
 */

#include "turl_common.h"
#include <mustache_json.h>
#include <stdio.h>
#include <string.h>
#include <tlog.h>
#include <turbo_fs.h>
#include <turbo_parser.h>


// Global logger for turl
tlog_t *g_turl_logger = NULL;

static int turl_template_has_mustache_tag(const char *template_str) {
  const char *cursor;

  if (template_str == NULL) {
    return 0;
  }

  cursor = template_str;
  while ((cursor = strstr(cursor, "{{")) != NULL) {
    return 1;
  }

  return 0;
}

void turl_setup_logger(int verbose) {
  tlog_config_t config = {.min_level = verbose ? TURBO_LOG_LEVEL_DEBUG : TURBO_LOG_LEVEL_INFO,
                          .buffer_size = 0,
                          .pool_size = 0};

  g_turl_logger = tlog_create(&config);

  turbo_console_sink_opts_t sink_opts = {.output = stdout,
                                         .use_colors = 1,
                                         .pattern = "[{time}] [{level}] {message}"};

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
  if (!path)
    return;
  turbo_fs_stat_t stat;
  if (turbo_fs_stat(path, &stat) != 0) {
    turbo_fs_mkdir(path, 0755);
  }
}

char *turl_render_template(const char *template_str, json_value_t *context) {
  if (!template_str)
    return NULL;
  if (!context || !turl_template_has_mustache_tag(template_str))
    return strdup(template_str);

  MUSTACHE_TEMPLATE *template = mustache_compile(template_str, strlen(template_str), NULL, NULL, 0);
  if (!template) {
    TLOG_ERROR("Failed to compile mustache template");
    return NULL;
  }

  MUSTACHE_STRING_RENDERER renderer;
  if (mustache_string_renderer_init(&renderer) != 0) {
    mustache_release(template);
    TLOG_ERROR("Failed to initialize mustache renderer");
    return NULL;
  }

  if (mustache_render_json(template, context, &renderer.base, &renderer, NULL, NULL) != 0) {
    mustache_string_renderer_free(&renderer);
    mustache_release(template);
    TLOG_ERROR("Failed to render mustache template");
    return NULL;
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
    json_value_t *json = NULL;
    if (turbo_parse_json((const uint8_t *)body, len, &json) == 0 && json) {
      char *pretty = turbo_json_serialize_pretty(json, NULL);
      if (pretty) {
        printf("%s\n", pretty);
        turbo_json_serialize_free(pretty);
        turbo_free_json(&json);
        return;
      }
      turbo_free_json(&json);
    }
  }

  // Fallback to verbatim
  fwrite(body, 1, len, stdout);
  printf("\n");
}
