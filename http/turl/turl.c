/**
 * @file turl.c
 * @brief Main entry point for turl - turbonet HTTP/WebSocket client
 */

#include "cmd_arger.h"
#include "history/turl_collection.h"
#include "turl_batch.h"
#include "turl_common.h"
#include "turl_http.h"
#include "turl_websocket.h"
#include <dotenv.h>
#include <js_internal.h>
#include <js_module.h>
#include <json_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlog.h>


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

  // Handle batch mode if input_file is provided
  if (input_file) {
    int ret = turl_batch_download(input_file, concurrency, raw_headers, header_count,
                                  mustache_context, output_path, follow_redirects, verbose);
    if (mustache_context)
      json_free(mustache_context);
    turl_cleanup_logger();
    return ret;
  }

  // Handle collection mode
  if (collection_path) {
    turl_http_config_t global_cfg = {.headers = raw_headers,
                                     .header_count = header_count,
                                     .user_pass = user_pass,
                                     .bearer_token = bearer_token,
                                     .jwt_secret = jwt_secret,
                                     .jwt_claims = jwt_claims,
                                     .decode_jwt = (int)decode_jwt,
                                     .retry_count = (int)retry_count,
                                     .retry_delay_ms = (int)retry_delay_ms,
                                     .show_stats = (int)show_stats,
                                     .verbose = verbose,
                                     .mustache_context = mustache_context,
                                     .follow_redirects = (int)follow_redirects};
    int ret = turl_run_collection(collection_path, &global_cfg);
    if (mustache_context)
      json_free(mustache_context);
    turl_cleanup_logger();
    return ret;
  }

  // QuickJS integration for scripts
  JSRuntime *rt = NULL;
  JSContext *js_ctx = NULL;
  JSValue global_obj = JS_UNDEFINED;

  if (script_path || test_path) {
    rt = JS_NewRuntime();
    js_ctx = JS_NewContext(rt);
    js_init_turbo_module(js_ctx);
    js_turbo_init_state(js_ctx);

    global_obj = JS_GetGlobalObject(js_ctx);
    JSValue env_obj;

    if (mustache_context) {
      char *json_str = json_serialize(mustache_context, NULL);
      env_obj = JS_ParseJSON(js_ctx, json_str, strlen(json_str), "<env>");
      json_serialize_free(json_str);
    } else {
      env_obj = JS_NewObject(js_ctx);
    }
    JS_SetPropertyStr(js_ctx, global_obj, "env", env_obj);

    // Add assertion helper
    const char *assert_js = "globalThis.assert = function(cond, msg) { if (!cond) throw new "
                            "Error('Assertion Failed: ' + (msg || '')); }";
    JS_Eval(js_ctx, assert_js, strlen(assert_js), "<assert>", JS_EVAL_TYPE_GLOBAL);

    if (script_path) {
      FILE *f = fopen(script_path, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *script = (char *)malloc(size + 1);
        fread(script, 1, size, f);
        script[size] = '\0';
        fclose(f);

        JSValue val = JS_Eval(js_ctx, script, size, script_path, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(val)) {
          js_turbo_dump_error(js_ctx);
        }
        JS_FreeValue(js_ctx, val);
        free(script);

        // Extract modified context back
        JSValue env_val = JS_GetPropertyStr(js_ctx, global_obj, "env");
        JSValue env_json = JS_JSONStringify(js_ctx, env_val, JS_UNDEFINED, JS_UNDEFINED);
        const char *env_str = JS_ToCString(js_ctx, env_json);

        if (mustache_context)
          json_free(mustache_context);
        mustache_context = json_parse(env_str, strlen(env_str));

        JS_FreeCString(js_ctx, env_str);
        JS_FreeValue(js_ctx, env_json);
        JS_FreeValue(js_ctx, env_val);
      } else {
        TLOG_ERROR("Failed to open script file: {}", script_path);
      }
    }
  }

  // Handle binary body
  char *body_to_send = raw_body;
  size_t body_len = 0;

  if (raw_binary_body) {
    if (raw_binary_body[0] == '@') {
      FILE *f = fopen(raw_binary_body + 1, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        body_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        body_to_send = (char *)malloc(body_len);
        fread(body_to_send, 1, body_len, f);
        fclose(f);
      } else {
        TLOG_ERROR("Failed to open binary file: {}", raw_binary_body + 1);
        turl_cleanup_logger();
        if (js_ctx) {
          JS_FreeValue(js_ctx, global_obj);
          JS_FreeContext(js_ctx);
          JS_FreeRuntime(rt);
        }
        if (mustache_context)
          json_free(mustache_context);
        return 1;
      }
    } else {
      body_to_send = raw_binary_body;
      body_len = strlen(raw_binary_body);
    }
  }

  // Check for WebSocket
  if (strncmp(raw_url, "ws://", 5) == 0 || strncmp(raw_url, "wss://", 6) == 0) {
    int ret = turl_handle_websocket(raw_url, body_to_send, body_len, verbose, send_ping);

    // Cleanup
    if (mustache_context)
      json_free(mustache_context);
    if (raw_binary_body && raw_binary_body[0] == '@' && body_to_send != raw_binary_body)
      free(body_to_send);

    turl_cleanup_logger();
    return ret;
  }

  // Execute HTTP request
  turl_http_config_t http_config = {.url = raw_url,
                                    .method_str = method_str,
                                    .body = body_to_send,
                                    .body_len = body_len,
                                    .headers = raw_headers,
                                    .header_count = header_count,
                                    .forms = raw_forms,
                                    .form_count = form_count,
                                    .user_pass = user_pass,
                                    .bearer_token = bearer_token,
                                    .jwt_secret = jwt_secret,
                                    .jwt_claims = jwt_claims,
                                    .decode_jwt = decode_jwt,
                                    .retry_count = (int)retry_count,
                                    .retry_delay_ms = (int)retry_delay_ms,
                                    .show_stats = (int)show_stats,
                                    .output_path = output_path,
                                    .follow_redirects = follow_redirects,
                                    .verbose = verbose,
                                    .mustache_context = mustache_context,
                                    .js_ctx = js_ctx,
                                    .global_obj = global_obj,
                                    .test_path = test_path};

  int ret = turl_execute_http_request(&http_config);

  // Cleanup
  if (js_ctx) {
    JS_FreeValue(js_ctx, global_obj);
    JS_FreeContext(js_ctx);
    JS_FreeRuntime(rt);
  }

  if (mustache_context)
    json_free(mustache_context);

  if (raw_binary_body && raw_binary_body[0] == '@' && body_to_send != raw_binary_body)
    free(body_to_send);

  turl_cleanup_logger();
  return ret;
}
