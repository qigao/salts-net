#include "cmd_arger.h"
#include <http_client.h>
#include <js_internal.h>
#include <js_module.h>
#include <json_parser.h>
#include <mustache_json.h>
#include <turbo_sync_client.h>


// ANSI Colors (Using octal escape codes for broader compatibility)
#define CLR_RESET "\033[0m"
#define CLR_RED "\033[31m"
#define CLR_GREEN "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_BLUE "\033[34m"
#define CLR_MAGENTA "\033[35m"
#define CLR_CYAN "\033[36m"
#define CLR_BOLD "\033[1m"
#define CLR_BOLD_BLUE "\033[1;34m"
#define CLR_BOLD_GREEN "\033[1;32m"
#define CLR_BOLD_YELLOW "\033[1;33m"

#ifdef _WIN32
  #include <windows.h>
static void setup_console(void) {
  HANDLE handles[] = {GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
  for (int i = 0; i < 2; i++) {
    if (handles[i] != INVALID_HANDLE_VALUE) {
      DWORD dwMode = 0;
      if (GetConsoleMode(handles[i], &dwMode)) {
        dwMode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
        SetConsoleMode(handles[i], dwMode);
      }
    }
  }
}
#else
static void setup_console(void) {}
#endif

/**
 * Helper to render a template string with JSON context.
 * Returns an allocated string that must be freed by the caller.
 */
static char *render_template(const char *template_str, json_value_t *context) {
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

/**
 * Helper to pretty-print JSON if possible.
 */
static void print_body(const char *body, size_t len, const char *content_type) {
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

/**
 * Handle WebSocket communication.
 */
static int handle_websocket(const char *url, const char *body, size_t body_len, int verbose,
                            int send_ping) {
  char host[256];
  int port = 80;
  char path[512] = "/";
  int use_tls = 0;

  // Simple URL parsing
  if (strncmp(url, "ws://", 5) == 0) {
    url += 5;
  } else if (strncmp(url, "wss://", 6) == 0) {
    url += 6;
    port = 443;
    use_tls = 1;
  } else {
    return -1;
  }

  const char *slash = strchr(url, '/');
  if (slash) {
    strncpy(path, slash, sizeof(path) - 1);
    size_t host_len = slash - url;
    if (host_len >= sizeof(host))
      host_len = sizeof(host) - 1;
    strncpy(host, url, host_len);
    host[host_len] = '\0';
  } else {
    strncpy(host, url, sizeof(host) - 1);
  }

  char *colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
    port = atoi(colon + 1);
  }

  if (verbose) {
    printf(CLR_BOLD_BLUE "* WebSocket: Connecting to %s:%d%s (%s)..." CLR_RESET "\n", host, port,
           path, use_tls ? "TLS" : "Plain");
  }

  sync_client_t *client = sync_client_create_with_transport(SYNC_CLIENT_TRANSPORT_WEBSOCKET);
  if (!client)
    return -1;

  sync_client_ws_config_t ws_config = {0};
  ws_config.path = path;
  ws_config.use_tls = use_tls;
  sync_client_set_ws_config(client, &ws_config);

  char connect_url[512];
  snprintf(connect_url, sizeof(connect_url), "%s://%s:%d", use_tls ? "wss" : "ws", host, port);

  if (sync_client_connect(client, connect_url) != SYNC_CLIENT_STATUS_OK) {
    fprintf(stderr, CLR_RED "WebSocket connection failed: %s" CLR_RESET "\n",
            sync_client_last_message(client));
    sync_client_destroy(client);
    return 1;
  }

  if (verbose)
    printf(CLR_BOLD_GREEN "* WebSocket Connected." CLR_RESET "\n");

  if (body && body_len > 0) {
    if (verbose)
      printf(CLR_CYAN "> Sending %zu bytes..." CLR_RESET "\n", body_len);
    sync_client_send(client, body, body_len);
  }

  if (send_ping) {
    // sync_client doesn't have a specific send_ping yet in the header I saw,
    // but it might be handled via a special opcode or we can just skip for now.
    // Actually turbo_websocket_client has it, let's see if sync_client exposes it.
    // For now, let's just receive.
  }

  // Receive loop (simple version for CLI)
  char *response = NULL;
  size_t len = 0;
  while (sync_client_receive_timeout(client, &response, &len, 5000) == SYNC_CLIENT_STATUS_OK) {
    if (verbose)
      printf(CLR_MAGENTA "< Received %zu bytes:" CLR_RESET "\n", len);
    print_body(response, len, "application/json"); // Try pretty print if it's JSON
    free(response);
    response = NULL;

    // In a real turl we might want to stay open, but for a single-shot tool,
    // maybe one message is enough or we wait for a bit.
    // Let's keep reading until the other side closes or we timeout.
  }

  if (verbose)
    printf(CLR_BOLD_BLUE "* WebSocket connection closed." CLR_RESET "\n");
  sync_client_destroy(client);
  return 0;
}

int main(int argc, char *argv[]) {
  setup_console();

  CmdArgerBool verbose = cmd_arger_false;
  CmdArgerBool follow_redirects = cmd_arger_false;
  CmdArgerBool send_ping = cmd_arger_false;
  char *method_str = "GET";
  char *raw_body = NULL;
  char *raw_binary_body = NULL;
  char *context_str = NULL;
  char *output_path = NULL;
  char *user_pass = NULL;
  char *bearer_token = NULL;
  char *script_path = NULL;
  char *test_path = NULL;

  char *raw_headers[100];
  uint32_t header_count = 0;
  char *raw_forms[100];
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
      cmd_arger_with_group(
          cmd_arger_with_env(
              cmd_arger_desc_string(&bearer_token, "bearer", "Bearer token for authentication"),
              "TURL_BEARER_TOKEN"),
          "Authentication"),

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
  };

  char *raw_url = NULL;
  CmdArgerDesc required_args[] = {
      cmd_arger_desc_string(&raw_url, "URL", "The URL to request"),
  };

  cmd_arger_parse(optional_args, sizeof(optional_args) / sizeof(*optional_args), required_args,
                  sizeof(required_args) / sizeof(*required_args), argc, argv,
                  "turl 1.0 - turbonet HTTP/WebSocket client", cmd_arger_true);

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
      fprintf(stderr, "* Warning: Failed to parse mustache context: %s\n", json_get_error());
    }
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
        fprintf(stderr, CLR_RED "Failed to open script file: %s" CLR_RESET "\n", script_path);
      }
    }
  }

  // Render templates (only for text body)
  char *rendered_url = render_template(raw_url, mustache_context);
  char *rendered_body = render_template(raw_body, mustache_context);
  size_t body_len = rendered_body ? strlen(rendered_body) : 0;

  char *rendered_headers[100];
  for (uint32_t i = 0; i < header_count; i++) {
    rendered_headers[i] = render_template(raw_headers[i], mustache_context);
  }

  // Handle binary body
  if (raw_binary_body) {
    if (rendered_body)
      free(rendered_body);
    if (raw_binary_body[0] == '@') {
      FILE *f = fopen(raw_binary_body + 1, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        body_len = ftell(f);
        fseek(f, 0, SEEK_SET);
        rendered_body = (char *)malloc(body_len);
        fread(rendered_body, 1, body_len, f);
        fclose(f);
      } else {
        fprintf(stderr, CLR_RED "Failed to open binary file: %s" CLR_RESET "\n",
                raw_binary_body + 1);
        return 1;
      }
    } else {
      rendered_body = strdup(raw_binary_body);
      body_len = strlen(rendered_body);
    }
  }

  // Check for WebSocket
  if (strncmp(rendered_url, "ws://", 5) == 0 || strncmp(rendered_url, "wss://", 6) == 0) {
    int ret = handle_websocket(rendered_url, rendered_body, body_len, verbose, send_ping);

    // Cleanup templates
    if (mustache_context)
      json_free(mustache_context);
    free(rendered_url);
    if (rendered_body)
      free(rendered_body);
    for (uint32_t i = 0; i < header_count; i++)
      free(rendered_headers[i]);

    return ret;
  }

  http_client_t *client = http_client_create();
  if (!client) {
    fprintf(stderr, "Failed to create HTTP client\n");
    return 1;
  }

  if (follow_redirects) {
    http_client_follow_redirects(client, 1);
  }

  if (user_pass) {
    const char *colon = strchr(user_pass, ':');
    if (colon) {
      size_t user_len = colon - user_pass;
      char *user = (char *)malloc(user_len + 1);
      memcpy(user, user_pass, user_len);
      user[user_len] = '\0';
      http_client_set_basic_auth(client, user, colon + 1);
      free(user);
    }
  }

  if (bearer_token) {
    http_client_set_bearer_token(client, bearer_token);
  }

  // Prepare method
  http_method_t method = HTTP_GET;
  const char *actual_method = method_str;
  if (strcmp(method_str, "POST") == 0)
    method = HTTP_POST;
  else if (strcmp(method_str, "PUT") == 0)
    method = HTTP_PUT;
  else if (strcmp(method_str, "DELETE") == 0)
    method = HTTP_DELETE;
  else if (strcmp(method_str, "HEAD") == 0)
    method = HTTP_HEAD;
  else if (strcmp(method_str, "PATCH") == 0)
    method = HTTP_PATCH;
  else if (strcmp(method_str, "OPTIONS") == 0)
    method = HTTP_OPTIONS;

  // Handle default POST if data is provided
  if (rendered_body && method == HTTP_GET) {
    method = HTTP_POST;
    actual_method = "POST";
  }

  if (verbose) {
    printf(CLR_BOLD_BLUE "* Trying to %s %s..." CLR_RESET "\n", actual_method, rendered_url);
    if (mustache_context) {
      printf(CLR_MAGENTA "* Using mustache context for templating" CLR_RESET "\n");
    }
  }

  http_response_t *response = NULL;

  if (form_count > 0) {
    http_multipart_form_t *form = http_multipart_form_create();
    for (int i = 0; i < form_count; i++) {
      char *rendered_form = render_template(raw_forms[i], mustache_context);
      char *equal = strchr(rendered_form, '=');
      if (equal) {
        *equal = '\0';
        const char *name = rendered_form;
        const char *value = equal + 1;

        if (value[0] == '@') {
          // File upload
          http_multipart_form_add_file_path(form, name, value + 1, NULL);
        } else {
          // Text field
          http_multipart_form_add_field(form, name, value);
        }
      }
      free(rendered_form);
    }
    response = http_post_multipart(client, rendered_url, form);
    http_multipart_form_destroy(form);
  } else {
    response = http_request(client, method, rendered_url,
                            header_count > 0 ? (const char **)rendered_headers : NULL, header_count,
                            rendered_body, body_len);
  }

  if (!response) {
    fprintf(stderr, CLR_RED "Request failed: No response" CLR_RESET "\n");
  } else if (response->error) {
    fprintf(stderr, CLR_RED "Error: %s (code: %d)" CLR_RESET "\n", response->error,
            response->error_code);
  } else {
    if (verbose) {
      printf(CLR_BOLD_GREEN "< HTTP/1.1 %d" CLR_RESET "\n", response->status_code);
      if (response->headers) {
        printf(CLR_BOLD_YELLOW "%s" CLR_RESET "\n", response->headers);
      }
    }

    if (output_path) {
      FILE *f = fopen(output_path, "wb");
      if (f) {
        fwrite(response->body, 1, response->body_len, f);
        fclose(f);
        if (verbose) {
          printf(CLR_CYAN "* Content written to %s" CLR_RESET "\n", output_path);
        }
      } else {
        fprintf(stderr, CLR_RED "Failed to open output file: %s" CLR_RESET "\n", output_path);
      }
    } else {
      char *content_type = http_response_get_header(response, "Content-Type");
      print_body(response->body, response->body_len, content_type);
      if (content_type)
        free(content_type);
    }

    // Run post-request script (tests)
    if (test_path && js_ctx) {
      JSValue resp_obj = JS_NewObject(js_ctx);
      JS_SetPropertyStr(js_ctx, resp_obj, "status", JS_NewInt32(js_ctx, response->status_code));
      JS_SetPropertyStr(js_ctx, resp_obj, "body",
                        JS_NewStringLen(js_ctx, response->body, response->body_len));

      // Add .json() method helper
      JS_Eval(js_ctx,
              "globalThis.Response = function(b) { this.body = b; }; Response.prototype.json = "
              "function() { return JSON.parse(this.body); };",
              -1, "<resp_setup>", JS_EVAL_TYPE_GLOBAL);
      JSValue resp_proto =
          JS_Eval(js_ctx, "Response.prototype", -1, "<proto>", JS_EVAL_TYPE_GLOBAL);
      JS_SetPrototype(js_ctx, resp_obj, resp_proto);
      JS_FreeValue(js_ctx, resp_proto);

      // Parse headers into an object
      JSValue headers_obj = JS_NewObject(js_ctx);
      if (response->headers) {
        const char *p = response->headers;
        while (*p) {
          const char *end = strstr(p, "\r\n");
          if (!end)
            end = p + strlen(p);
          const char *colon = (const char *)memchr(p, ':', end - p);
          if (colon) {
            size_t key_len = colon - p;
            const char *val_start = colon + 1;
            while (*val_start == ' ')
              val_start++;
            size_t val_len = end - val_start;

            char *key = (char *)malloc(key_len + 1);
            memcpy(key, p, key_len);
            key[key_len] = '\0';

            JS_SetPropertyStr(js_ctx, headers_obj, key,
                              JS_NewStringLen(js_ctx, val_start, val_len));
            free(key);
          }
          if (!*end)
            break;
          p = end + 2;
        }
      }
      JS_SetPropertyStr(js_ctx, resp_obj, "headers", headers_obj);
      JS_SetPropertyStr(js_ctx, global_obj, "response", resp_obj);

      FILE *f = fopen(test_path, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *script = (char *)malloc(size + 1);
        fread(script, 1, size, f);
        script[size] = '\0';
        fclose(f);

        JSValue val = JS_Eval(js_ctx, script, size, test_path, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(val)) {
          js_turbo_dump_error(js_ctx);
        }
        JS_FreeValue(js_ctx, val);
        free(script);
      }
    }
  }

  // Cleanup
  if (js_ctx) {
    JS_FreeValue(js_ctx, global_obj);
    JS_FreeContext(js_ctx);
    JS_FreeRuntime(rt);
  }
  if (response)
    http_response_free(response);
  http_client_destroy(client);

  if (mustache_context)
    json_free(mustache_context);
  free(rendered_url);
  if (rendered_body)
    free(rendered_body);
  for (uint32_t i = 0; i < header_count; i++)
    free(rendered_headers[i]);

  return 0;
}
