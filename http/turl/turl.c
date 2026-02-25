/**
 * @file turl.c
 * @brief Main entry point for turl - turbonet HTTP/WebSocket client
 */

#include "cmd_arger.h"
#include "collection/turl_collection.h"
#include "turl_batch.h"
#include "turl_common.h"
#include "turl_http.h"
#include "turl_websocket.h"
#include <dotenv.h>
#include <json_parser.h>
#include <netcore/turbo_coro_context.h>
#include <turbo_coro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlog.h>

typedef struct {
  turbo_coro_context_t *coro_ctx;
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

static void turl_main_coro(turbo_coro_t *co, void *arg) {
  turl_coro_args_t *a = (turl_coro_args_t *)arg;

  // Handle batch mode
  if (a->input_file) {
    a->ret = turl_batch_download(a->input_file, a->concurrency, a->raw_headers,
                                  a->header_count, a->mustache_context,
                                  a->output_path, a->follow_redirects,
                                  a->verbose, a->coro_ctx);
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
                                      .follow_redirects = a->follow_redirects,
                                      .coro_ctx = a->coro_ctx};
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
                                     .follow_redirects = a->follow_redirects,
                                     .verbose = a->verbose,
                                     .mustache_context = a->mustache_context,
                                     .coro_ctx = a->coro_ctx};

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
        snprintf(env_path, sizeof(env_path), ".env.%s", env_name);
        dotenv_load(env_path, false);
        break;
      }
    }
  }

  CmdArgerBool verbose = cmd_arger_false;
  CmdArgerBool follow_redirects = cmd_arger_false;
  CmdArgerBool show_stats = cmd_arger_false;
  CmdArgerBool send_ping = cmd_arger_false;
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
  CmdArgerBool decode_jwt = cmd_arger_false;
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

  CmdArgerDesc optional_args[] = {
      // General Options
      cmd_arger_with_group(
          cmd_arger_with_env(
              cmd_arger_desc_flag_sh(&verbose, "verbose", "v", "Make the operation more talkative"),
              "TURL_VERBOSE"),
          "General Options"),
      cmd_arger_with_group(
          cmd_arger_desc_flag_sh(&follow_redirects, "location", "L", "Follow redirects"),
          "General Options"),

      cmd_arger_with_group(
          cmd_arger_desc_flag_sh(&show_stats, "stats", "s", "Show performance statistics"),
          "General Options"),

      cmd_arger_with_group(
          cmd_arger_desc_string_sh(&env_name, "use-env", "e", "Load environment from .env.<name>"),
          "General Options"),

      // Request Options
      cmd_arger_with_group(
          cmd_arger_with_choices(
              cmd_arger_desc_string_sh(&method_str, "request", "X", "Specify request method"),
              http_methods, 7),
          "Request Options"),
      cmd_arger_with_group(cmd_arger_desc_string_sh(&raw_body, "data", "d", "HTTP POST data"),
                           "Request Options"),
      cmd_arger_with_group(cmd_arger_desc_string(&raw_binary_body, "data-binary",
                                                 "HTTP POST binary data (no templating)"),
                           "Request Options"),
      cmd_arger_with_group(cmd_arger_desc_string_list_sh(raw_headers, &header_count, 100, "header",
                                                         "H", "Pass custom header(s) to server"),
                           "Request Options"),
      cmd_arger_with_group(cmd_arger_desc_string_list_sh(raw_forms, &form_count, 100, "form", "F",
                                                         "Specify multipart MIME data"),
                           "Request Options"),

      // Authentication
      cmd_arger_with_group(cmd_arger_with_env(cmd_arger_desc_string_sh(&user_pass, "user", "u",
                                                                       "Server user and password"),
                                              "TURL_USER"),
                           "Authentication"),
      cmd_arger_with_env(
          cmd_arger_desc_string(&bearer_token, "bearer", "Bearer token for authentication"),
          "TURL_BEARER_TOKEN"),
      cmd_arger_with_group(
          cmd_arger_desc_string(&jwt_secret, "jwt-secret", "Secret for generating/validating JWT"),
          "Authentication"),
      cmd_arger_with_group(
          cmd_arger_desc_string(&jwt_claims, "jwt-claims", "JSON claims for generating JWT"),
          "Authentication"),
      cmd_arger_with_group(cmd_arger_desc_flag(&decode_jwt, "decode-jwt",
                                               "Try to decode JWT from Authorization/Body"),
                           "Authentication"),

      // Retry Options
      cmd_arger_with_group(
          cmd_arger_desc_integer(&retry_count, "retry", "Number of retries on failure (default 0)"),
          "Retry Options"),
      cmd_arger_with_group(
          cmd_arger_desc_integer(&retry_delay_ms, "retry-delay", "Delay between retries in ms (default 1000)"),
          "Retry Options"),

      // Output Options
      cmd_arger_with_group(
          cmd_arger_desc_string_sh(&output_path, "output", "o", "Write to file instead of stdout"),
          "Output Options"),

      // Templating & Scripting
      cmd_arger_with_group(
          cmd_arger_with_env(cmd_arger_desc_string_sh(&context_str, "context", "j",
                                                      "JSON context for mustache templates"),
                             "TURL_CONTEXT"),
          "Templating & Scripting"),
      cmd_arger_with_group(cmd_arger_desc_string(&context_str, "env", "Alias for --context"),
                           "Templating & Scripting"),
      cmd_arger_with_group(
          cmd_arger_desc_string(&script_path, "script", "Pre-request JavaScript script"),
          "Templating & Scripting"),
      cmd_arger_with_group(
          cmd_arger_desc_string_sh(&test_path, "test", "t", "Post-request JavaScript test script"),
          "Templating & Scripting"),

      // WebSocket Options
      cmd_arger_with_group(cmd_arger_desc_flag(&send_ping, "ping", "Send a WebSocket ping"),
                           "WebSocket Options"),

      // Aria2c-like Batch Options
      cmd_arger_with_group(
          cmd_arger_desc_string_sh(&input_file, "input-file", "i", "Read URLs from a file"),
          "Batch Options"),
      cmd_arger_with_group(cmd_arger_desc_string(&collection_path, "collection",
                                                 "Run a JSON collection of requests"),
                           "Batch Options"),
      cmd_arger_with_group(cmd_arger_desc_integer_sh(&concurrency, "max-concurrent-downloads", "j",
                                                     "Maximum number of concurrent downloads"),
                           "Batch Options"),

      // Positional-as-Optional capture
      cmd_arger_with_group(cmd_arger_desc_string(&raw_url, "url", "The URL to request"),
                           "Request Options"),
  };

  cmd_arger_parse(optional_args, sizeof(optional_args) / sizeof(*optional_args), NULL, 0, argc,
                  argv, "turl 1.0 - turbonet HTTP/WebSocket client", cmd_arger_true);

  // Initialize logger early
  turl_setup_logger(verbose);

  if (!raw_url && !input_file && !collection_path) {
    TLOG_ERROR("Must provide either a URL, an input file (-i), or a collection");
    cmd_arger_show_help_and_exit(optional_args, sizeof(optional_args) / sizeof(*optional_args),
                                 NULL, 0, argv[0], "turl 1.0 - turbonet HTTP/WebSocket client",
                                 cmd_arger_true);
    turl_cleanup_logger();
    return 1;
  }

  // Initial context (empty)
  json_value_t *mustache_context = json_create_object();

  // Process context/env if provided
  if (context_str) {
    if (mustache_context)
      json_free(mustache_context);
    // Try parsing as string first
    mustache_context = json_parse(context_str, strlen(context_str));
    if (!mustache_context) {
      // Try parsing as file
      mustache_context = json_parse_file(context_str);
    }

    if (!mustache_context && verbose) {
      TLOG_WARN("Failed to parse mustache context: {}", json_get_error());
    }
  }

  // Check for WebSocket — handled outside coroutine (uses sync_client)
  if (raw_url && (strncmp(raw_url, "ws://", 5) == 0 || strncmp(raw_url, "wss://", 6) == 0)) {
    int ret = turl_handle_websocket(raw_url, raw_body, raw_body ? strlen(raw_body) : 0, verbose, send_ping);
    if (mustache_context)
      json_free(mustache_context);
    turl_cleanup_logger();
    return ret;
  }

  // Create coroutine context and run HTTP/batch/collection inside it
  turbo_coro_context_t *coro_ctx = turbo_coro_context_create(NULL);

  turl_coro_args_t coro_args = {
      .coro_ctx = coro_ctx,
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
      .context_str = context_str,
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

  turbo_coro_t *co = turbo_coro_create(turl_main_coro, &coro_args, NULL);
  turbo_coro_resume(co);
  turbo_coro_context_run(coro_ctx, TURBO_RUN_DEFAULT);
  turbo_coro_destroy(co);
  turbo_coro_context_destroy(coro_ctx);

  int ret = coro_args.ret;

  // Cleanup
  if (mustache_context)
    json_free(mustache_context);

  turl_cleanup_logger();
  return ret;
}
