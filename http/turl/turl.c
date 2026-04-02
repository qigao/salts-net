/**
 * @file turl.c
 * @brief Main entry point for turl - turbonet HTTP/WebSocket client
 */

#include "collection/turl_collection.h"
#include "turl_batch.h"
#include "turl_common.h"
#include "turl_http.h"
#include "turl_websocket.h"
#include <CoroNet.h>
#include <fmt.h>
#include <turbo_coro.h>
#include <turbo_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlog.h>

typedef struct {
  int verbose;
  int follow_redirects;
  int show_stats;
  int decode_jwt;
  int64_t retry_count;
  int64_t retry_delay_ms;
  int64_t concurrency;
  char *method_str;
  char *raw_body;
  char *raw_binary_body;
  char *context_str;
  char *output_path;
  char *user_pass;
  char *bearer_token;
  char *jwt_secret;
  char *jwt_claims;
  char *test_path;
  char *collection_path;
  char *input_file;
  char *raw_url;
  char **raw_headers;
  uint32_t header_count;
  char **raw_forms;
  uint32_t form_count;
  json_value_t *mustache_context;
  int ret;
} turl_coro_args_t;

static json_value_t *turl_parse_json_text(const char *text, size_t len) {
  json_value_t *json = NULL;

  if (!text)
    return NULL;

  if (turbo_parse_json((const uint8_t *)text, len, &json) != 0)
    return NULL;

  return json;
}

static json_value_t *turl_parse_json_file(const char *path) {
  FILE *file = NULL;
  char *buffer = NULL;
  long size = 0;
  json_value_t *json = NULL;

  if (!path)
    return NULL;

  file = fopen(path, "rb");
  if (!file)
    return NULL;

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }

  size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }

  buffer = (char *)malloc((size_t)size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }

  if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
    free(buffer);
    fclose(file);
    return NULL;
  }

  buffer[size] = '\0';
  fclose(file);

  json = turl_parse_json_text(buffer, (size_t)size);
  free(buffer);
  return json;
}

static void turl_main_coro(coro_t *co, void *arg) {
  turl_coro_args_t *a = (turl_coro_args_t *)arg;

  // Handle batch mode
  if (a->input_file) {
    a->ret = turl_batch_download(a->input_file, a->concurrency, a->raw_headers,
                                  a->header_count, a->mustache_context,
                                  a->output_path, a->follow_redirects,
                                  a->verbose);
    return;
  }

  // Handle collection mode
  if (a->collection_path) {
    turl_http_config_t global_cfg = {.headers = a->raw_headers,
                                      .header_count = a->header_count,
                                      .user_pass = a->user_pass,
                                      .bearer_token = a->bearer_token,
                                      .jwt_secret = a->jwt_secret,
                                      .jwt_claims = a->jwt_claims,
                                      .decode_jwt = a->decode_jwt,
                                      .retry_count = (int)a->retry_count,
                                      .retry_delay_ms = (int)a->retry_delay_ms,
                                      .show_stats = a->show_stats,
                                      .verbose = a->verbose,
                                      .mustache_context = a->mustache_context,
                                      .follow_redirects = a->follow_redirects};
    a->ret = turl_run_collection(a->collection_path, &global_cfg);
    return;
  }

  // Handle binary body
  char *body_to_send = a->raw_body;
  size_t body_len = 0;

  if (a->raw_binary_body) {
    if (a->raw_binary_body[0] == '@') {
      FILE *f = fopen(a->raw_binary_body + 1, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        body_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        body_to_send = (char *)malloc(body_len);
        fread(body_to_send, 1, body_len, f);
        fclose(f);
      } else {
        TLOG_ERROR("Failed to open binary file: {}", a->raw_binary_body + 1);
        a->ret = 1;
        return;
      }
    } else {
      body_to_send = a->raw_binary_body;
      body_len = strlen(a->raw_binary_body);
    }
  }

  // Execute HTTP request
  turl_http_config_t http_config = {.url = a->raw_url,
                                     .method_str = a->method_str,
                                     .body = body_to_send,
                                     .body_len = body_len,
                                     .headers = a->raw_headers,
                                     .header_count = a->header_count,
                                     .forms = a->raw_forms,
                                     .form_count = a->form_count,
                                     .user_pass = a->user_pass,
                                     .bearer_token = a->bearer_token,
                                     .jwt_secret = a->jwt_secret,
                                     .jwt_claims = a->jwt_claims,
                                     .decode_jwt = a->decode_jwt,
                                     .retry_count = (int)a->retry_count,
                                     .retry_delay_ms = (int)a->retry_delay_ms,
                                     .show_stats = a->show_stats,
                                     .output_path = a->output_path,
                                     .test_path = a->test_path,
                                     .follow_redirects = a->follow_redirects,
                                     .verbose = a->verbose,
                                     .mustache_context = a->mustache_context};

  a->ret = turl_execute_http_request(&http_config);

  if (a->raw_binary_body && a->raw_binary_body[0] == '@' && body_to_send != a->raw_binary_body)
    free(body_to_send);
}

int main(int argc, char *argv[]) {
  char *env_name = NULL;

  // Pre-scan for environment switching to ensure env vars are available for CLI defaults
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--use-env") == 0) {
      if (i + 1 < argc) {
        env_name = argv[i + 1];
        char env_path[256];
        fmt(env_path, sizeof(env_path), ".env.{}", env_name);
        turbo_dotenv_load(env_path, false);
        break;
      }
    }
  }

  bool verbose = false;
  bool follow_redirects = false;
  bool show_stats = false;
  bool send_ping = false;
  int64_t retry_count = 0;
  int64_t retry_delay_ms = 1000;
  char *method_str = "GET";
  char *raw_body = NULL;
  char *raw_binary_body = NULL;
  char *context_str = NULL;
  char *output_path = NULL;
  char *user_pass = NULL;
  char *bearer_token = NULL;
  char *jwt_secret = NULL;
  char *jwt_claims = NULL;
  bool decode_jwt = false;
  char *script_path = NULL;
  char *test_path = NULL;
  char *collection_path = NULL;
  char *input_file = NULL;
  char *raw_url = NULL;
  int64_t concurrency = 5;

  char *raw_headers[100] = {0};
  uint32_t header_count = 0;
  char *raw_forms[100] = {0};
  uint32_t form_count = 0;

  // Define allowed HTTP methods
  const char *http_methods[] = {"GET", "POST", "PUT", "DELETE", "HEAD", "PATCH", "OPTIONS"};
  turbo_cmd_parser_t *parser = turbo_cmd_create("turl", "1.0 - turbonet HTTP/WebSocket client");
  uint32_t arg_index = 0;

  turbo_cmd_add_flag(parser, &verbose, "verbose", "v", "Make the operation more talkative");
  arg_index = turbo_cmd_last_index(parser);
  turbo_cmd_set_env(parser, arg_index, "TURL_VERBOSE");
  turbo_cmd_set_group(parser, arg_index, "General Options");

  turbo_cmd_add_flag(parser, &follow_redirects, "location", "L", "Follow redirects");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "General Options");

  turbo_cmd_add_flag(parser, &show_stats, "stats", "s", "Show performance statistics");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "General Options");

  turbo_cmd_add_string(parser, &env_name, "use-env", "e", "Load environment from .env.<name>");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "General Options");

  turbo_cmd_add_string(parser, &method_str, "request", "X", "Specify request method");
  arg_index = turbo_cmd_last_index(parser);
  turbo_cmd_set_choices(parser, arg_index, http_methods, 7);
  turbo_cmd_set_group(parser, arg_index, "Request Options");

  turbo_cmd_add_string(parser, &raw_body, "data", "d", "HTTP POST data");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Request Options");

  turbo_cmd_add_string(parser, &raw_binary_body, "data-binary", NULL,
                       "HTTP POST binary data (no templating)");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Request Options");

  turbo_cmd_add_string_list(parser, raw_headers, &header_count, 100, "header", "H",
                            "Pass custom header(s) to server");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Request Options");

  turbo_cmd_add_string_list(parser, raw_forms, &form_count, 100, "form", "F",
                            "Specify multipart MIME data");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Request Options");

  turbo_cmd_add_string(parser, &user_pass, "user", "u", "Server user and password");
  arg_index = turbo_cmd_last_index(parser);
  turbo_cmd_set_env(parser, arg_index, "TURL_USER");
  turbo_cmd_set_group(parser, arg_index, "Authentication");

  turbo_cmd_add_string(parser, &bearer_token, "bearer", NULL,
                       "Bearer token for authentication");
  turbo_cmd_set_env(parser, turbo_cmd_last_index(parser), "TURL_BEARER_TOKEN");

  turbo_cmd_add_string(parser, &jwt_secret, "jwt-secret", NULL,
                       "Secret for generating/validating JWT");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Authentication");

  turbo_cmd_add_string(parser, &jwt_claims, "jwt-claims", NULL,
                       "JSON claims for generating JWT");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Authentication");

  turbo_cmd_add_flag(parser, &decode_jwt, "decode-jwt", NULL,
                     "Try to decode JWT from Authorization/Body");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Authentication");

  turbo_cmd_add_integer(parser, &retry_count, "retry", NULL,
                        "Number of retries on failure (default 0)");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Retry Options");

  turbo_cmd_add_integer(parser, &retry_delay_ms, "retry-delay", NULL,
                        "Delay between retries in ms (default 1000)");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Retry Options");

  turbo_cmd_add_string(parser, &output_path, "output", "o", "Write to file instead of stdout");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Output Options");

  turbo_cmd_add_string(parser, &context_str, "context", "j",
                       "JSON context for mustache templates");
  arg_index = turbo_cmd_last_index(parser);
  turbo_cmd_set_env(parser, arg_index, "TURL_CONTEXT");
  turbo_cmd_set_group(parser, arg_index, "Templating & Scripting");

  turbo_cmd_add_string(parser, &context_str, "env", NULL, "Alias for --context");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Templating & Scripting");

  turbo_cmd_add_string(parser, &script_path, "script", NULL, "Pre-request JavaScript script");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Templating & Scripting");

  turbo_cmd_add_string(parser, &test_path, "test", "t", "Post-request JavaScript test script");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Templating & Scripting");

  turbo_cmd_add_flag(parser, &send_ping, "ping", NULL, "Send a WebSocket ping");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "WebSocket Options");

  turbo_cmd_add_string(parser, &input_file, "input-file", "i", "Read URLs from a file");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Batch Options");

  turbo_cmd_add_string(parser, &collection_path, "collection", NULL,
                       "Run a JSON collection of requests");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Batch Options");

  turbo_cmd_add_integer(parser, &concurrency, "max-concurrent-downloads", "j",
                        "Maximum number of concurrent downloads");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Batch Options");

  turbo_cmd_add_string(parser, &raw_url, "url", NULL, "The URL to request");
  turbo_cmd_set_group(parser, turbo_cmd_last_index(parser), "Request Options");

  turbo_cmd_parse(parser, argc, argv, true);

  // Initialize logger early
  turl_setup_logger(verbose);

  if (!raw_url && !input_file && !collection_path) {
    TLOG_ERROR("Must provide either a URL, an input file (-i), or a collection");
    turbo_cmd_show_help(parser, true);
    turl_cleanup_logger();
    return 1;
  }

  turbo_cmd_destroy(parser);

  // Initial context (empty)
  json_value_t *mustache_context = turbo_json_create_object();

  // Process context/env if provided
  if (context_str) {
    turbo_free_json(&mustache_context);
    mustache_context = turl_parse_json_text(context_str, strlen(context_str));
    if (!mustache_context) {
      mustache_context = turl_parse_json_file(context_str);
    }

    if (!mustache_context && verbose) {
      TLOG_WARN("Failed to parse mustache context: {}", context_str);
    }
  }

  // Check for WebSocket — handled outside coroutine (uses sync_client)
  if (raw_url && (strncmp(raw_url, "ws://", 5) == 0 || strncmp(raw_url, "wss://", 6) == 0)) {
    int ret = turl_handle_websocket(raw_url, raw_body, raw_body ? strlen(raw_body) : 0, verbose, send_ping);
    turbo_free_json(&mustache_context);
    turl_cleanup_logger();
    return ret;
  }

  // Create coroutine context and run HTTP/batch/collection inside it
  coro_context_t *coro_ctx = coro_context_create(NULL);

  turl_coro_args_t coro_args = {
      .verbose = verbose,
      .follow_redirects = follow_redirects,
      .show_stats = show_stats,
      .decode_jwt = decode_jwt,
      .retry_count = retry_count,
      .retry_delay_ms = retry_delay_ms,
      .concurrency = concurrency,
      .method_str = method_str,
      .raw_body = raw_body,
      .raw_binary_body = raw_binary_body,
      .output_path = output_path,
      .user_pass = user_pass,
      .bearer_token = bearer_token,
      .jwt_secret = jwt_secret,
      .jwt_claims = jwt_claims,
      .test_path = test_path,
      .collection_path = collection_path,
      .input_file = input_file,
      .raw_url = raw_url,
      .raw_headers = raw_headers,
      .header_count = header_count,
      .raw_forms = raw_forms,
      .form_count = form_count,
      .mustache_context = mustache_context,
      .ret = 0,
  };

  coro_context_spawn(coro_ctx, turl_main_coro, &coro_args);
  coro_context_run(coro_ctx, TURBO_RUN_DEFAULT);
  coro_context_destroy(coro_ctx);

  int ret = coro_args.ret;

  // Cleanup
  turbo_free_json(&mustache_context);

  turl_cleanup_logger();
  return ret;
}
