/**
 * @file rpc_server_example.c
 * @brief Simple RPC server example using Iris framework
 */
#include "iris.h"
#include "server.h" 
#include "rpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Math.add handler */
static int math_add_handler(Req *req, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
  (void)req;

  /* Get parameters */
  int64_t a = 0, b = 0;
  if (rpc_get_param_int(rpc_req, "a", &a) != 0 || rpc_get_param_int(rpc_req, "b", &b) != 0) {
    rpc_set_error(rpc_res, RPC_ERROR_INVALID_PARAMS, "Missing or invalid parameters 'a' and 'b'");
    return -1;
  }

  /* Calculate result */
  int64_t result = a + b;

  /* Build response */
  char *result_json = turbo_arena_sprintf(rpc_res->arena, "%lld", (long long)result);
  rpc_set_result(rpc_res, result_json);

  return 0;
}

/* Math.multiply handler */
static int math_multiply_handler(Req *req, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
  (void)req;

  /* Get parameters */
  int64_t a = 0, b = 0;
  if (rpc_get_param_int(rpc_req, "a", &a) != 0 || rpc_get_param_int(rpc_req, "b", &b) != 0) {
    rpc_set_error(rpc_res, RPC_ERROR_INVALID_PARAMS, "Missing or invalid parameters 'a' and 'b'");
    return -1;
  }

  /* Calculate result */
  int64_t result = a * b;

  /* Build response */
  char *result_json = turbo_arena_sprintf(rpc_res->arena, "%lld", (long long)result);
  rpc_set_result(rpc_res, result_json);

  return 0;
}

/* User.getInfo handler */
static int user_getinfo_handler(Req *req, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
  (void)req;

  /* Get username parameter */
  const char *username = rpc_get_param_string(rpc_req, "username");
  if (!username) {
    rpc_set_error(rpc_res, RPC_ERROR_INVALID_PARAMS, "Missing parameter 'username'");
    return -1;
  }

  /* Mock user data lookup */
  if (strcmp(username, "john") == 0) {
    char *result_json = turbo_arena_sprintf(
        rpc_res->arena,
        "{\"username\":\"%s\",\"email\":\"john@example.com\",\"name\":\"John Doe\",\"age\":30}",
        username);
    rpc_set_result(rpc_res, result_json);
  } else if (strcmp(username, "jane") == 0) {
    char *result_json = turbo_arena_sprintf(
        rpc_res->arena,
        "{\"username\":\"%s\",\"email\":\"jane@example.com\",\"name\":\"Jane Smith\",\"age\":28}",
        username);
    rpc_set_result(rpc_res, result_json);
  } else {
    rpc_set_error(rpc_res, RPC_ERROR_INVALID_PARAMS, "User not found");
    return -1;
  }

  return 0;
}

/* Log.message handler (notification) */
static int log_message_handler(Req *req, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
  (void)req;
  (void)rpc_res;

  /* Get message parameter */
  const char *message = rpc_get_param_string(rpc_req, "message");
  if (message) {
    printf("[LOG] %s\n", message);
  }

  /* Notifications don't send responses */
  return 0;
}

/* Echo handler - returns whatever is sent */
static int echo_handler(Req *req, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
  (void)req;

  /* Get message parameter */
  const char *message = rpc_get_param_string(rpc_req, "message");
  if (!message) {
    rpc_set_error(rpc_res, RPC_ERROR_INVALID_PARAMS, "Missing parameter 'message'");
    return -1;
  }

  /* Echo back */
  char *result_json = turbo_arena_sprintf(rpc_res->arena, "{\"echo\":\"%s\"}", message);
  rpc_set_result(rpc_res, result_json);

  return 0;
}

/* Status handler - returns server status */
static int status_handler(Req *req, rpc_request_t *rpc_req, rpc_response_t *rpc_res) {
  (void)req;
  (void)rpc_req;

  char *result_json = turbo_arena_sprintf(
      rpc_res->arena, "{\"status\":\"running\",\"version\":\"1.0.0\",\"uptime\":12345}");
  rpc_set_result(rpc_res, result_json);

  return 0;
}

/* Home page handler */
static void home_handler(Req *req, Res *res) {
  (void)req;

  const char *html = "<!DOCTYPE html>\n"
                     "<html>\n"
                     "<head>\n"
                     "  <title>RPC Server Example</title>\n"
                     "  <style>\n"
                     "    body { font-family: Arial, sans-serif; margin: 40px; }\n"
                     "    h1 { color: #333; }\n"
                     "    .endpoint { background: #f4f4f4; padding: 10px; margin: 10px 0; }\n"
                     "    code { background: #e8e8e8; padding: 2px 6px; }\n"
                     "  </style>\n"
                     "</head>\n"
                     "<body>\n"
                     "  <h1>RPC Server Example</h1>\n"
                     "  <p>JSON-RPC 2.0 server running on Iris framework</p>\n"
                     "  \n"
                     "  <h2>Available Methods</h2>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>math.add</strong> - Add two numbers<br>\n"
                     "    Params: <code>{\"a\": number, \"b\": number}</code>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>math.multiply</strong> - Multiply two numbers<br>\n"
                     "    Params: <code>{\"a\": number, \"b\": number}</code>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>user.getInfo</strong> - Get user information<br>\n"
                     "    Params: <code>{\"username\": string}</code>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>log.message</strong> - Log a message (notification)<br>\n"
                     "    Params: <code>{\"message\": string}</code>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>echo</strong> - Echo back a message<br>\n"
                     "    Params: <code>{\"message\": string}</code>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>status</strong> - Get server status<br>\n"
                     "    Params: <code>{}</code>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>rpc.listMethods</strong> - List all available methods<br>\n"
                     "    Params: <code>{}</code>\n"
                     "  </div>\n"
                     "  \n"
                     "  <h2>RPC Endpoint</h2>\n"
                     "  <p>POST to <code>/rpc</code> with JSON-RPC 2.0 request</p>\n"
                     "  \n"
                     "  <h3>Example Request:</h3>\n"
                     "  <pre>\n"
                     "{\n"
                     "  \"jsonrpc\": \"2.0\",\n"
                     "  \"method\": \"math.add\",\n"
                     "  \"params\": {\"a\": 5, \"b\": 3},\n"
                     "  \"id\": 1\n"
                     "}\n"
                     "  </pre>\n"
                     "  \n"
                     "  <h3>Example Response:</h3>\n"
                     "  <pre>\n"
                     "{\n"
                     "  \"jsonrpc\": \"2.0\",\n"
                     "  \"result\": 8,\n"
                     "  \"id\": 1\n"
                     "}\n"
                     "  </pre>\n"
                     "</body>\n"
                     "</html>";

  send_html(res, 200, html);
}

int main(void) {
  printf("RPC Server Example\n");
  printf("==================\n\n");

  /* Create RPC configuration */
  rpc_config_t rpc_config;
  rpc_config.endpoint = "/rpc";
  rpc_config.default_protocol = RPC_PROTOCOL_JSON;
  rpc_config.enable_introspection = 1;
  rpc_config.enable_batch = 1;
  rpc_config.max_batch_size = 10;
  rpc_config.max_request_size = 1024 * 1024; /* 1MB */

  /* Initialize RPC context */
  rpc_context_t *rpc_ctx = rpc_init(&rpc_config);
  if (!rpc_ctx) {
    fprintf(stderr, "Failed to initialize RPC context\n");
    return 1;
  }

  printf("RPC context initialized\n");

  /* Register RPC methods */
  rpc_method_t method;

  method.name = "math.add";
  method.handler = math_add_handler;
  method.description = "Add two numbers";
  method.requires_auth = 0;
  rpc_register_method(rpc_ctx, &method);
  printf("Registered method: %s\n", method.name);

  method.name = "math.multiply";
  method.handler = math_multiply_handler;
  method.description = "Multiply two numbers";
  method.requires_auth = 0;
  rpc_register_method(rpc_ctx, &method);
  printf("Registered method: %s\n", method.name);

  method.name = "user.getInfo";
  method.handler = user_getinfo_handler;
  method.description = "Get user information";
  method.requires_auth = 0;
  rpc_register_method(rpc_ctx, &method);
  printf("Registered method: %s\n", method.name);

  method.name = "log.message";
  method.handler = log_message_handler;
  method.description = "Log a message";
  method.requires_auth = 0;
  rpc_register_method(rpc_ctx, &method);
  printf("Registered method: %s\n", method.name);

  method.name = "echo";
  method.handler = echo_handler;
  method.description = "Echo back a message";
  method.requires_auth = 0;
  rpc_register_method(rpc_ctx, &method);
  printf("Registered method: %s\n", method.name);

  method.name = "status";
  method.handler = status_handler;
  method.description = "Get server status";
  method.requires_auth = 0;
  rpc_register_method(rpc_ctx, &method);
  printf("Registered method: %s\n", method.name);

  /* Setup RPC endpoint */
  if (rpc_setup_endpoint(rpc_ctx) != 0) {
    fprintf(stderr, "Failed to setup RPC endpoint\n");
    rpc_destroy(rpc_ctx);
    return 1;
  }

  printf("RPC endpoint setup at %s\n", rpc_config.endpoint);

  /* Register home page */
  get("/", home_handler);

  printf("\nServer starting on http://localhost:8080\n");
  printf("RPC endpoint: http://localhost:8080/rpc\n");
  printf("Home page: http://localhost:8080/\n");
  printf("\nPress Ctrl+C to stop\n\n");

  /* Start server */
  int result = ecewo(8080);
  if (result != 0) {
    fprintf(stderr, "Server failed to start: %d\n", result);
  }

  /* Cleanup */
  rpc_destroy(rpc_ctx);

  return 0;
}
