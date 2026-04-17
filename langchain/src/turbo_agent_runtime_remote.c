#include "turbo_agent_runtime_remote.h"

#include "turbo_agent_state.h"
#include "turbo_parser.h"

#include <stdlib.h>
#include <string.h>

struct turbo_agent_runtime_remote_s {
  turbo_agent_runtime_t *runtime;
  turbo_agent_runtime_remote_graph_resolver_fn graph_resolver;
  void *graph_resolver_user_data;
};

enum {
  TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST = -32600,
  TURBO_AGENT_RUNTIME_REMOTE_RPC_METHOD_NOT_FOUND = -32601,
  TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS = -32602,
  TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL = -32603
};

static const char *TURBO_AGENT_RUNTIME_REMOTE_HTTP_JSONRPC_PATH = "/v1/runtime/jsonrpc";

static json_value_t *turbo_agent_runtime_remote_response_base(const json_value_t *request_json) {
  json_value_t *response_json = turbo_json_create_object();
  const json_value_t *id_json = request_json ? turbo_json_object_get(request_json, "id") : NULL;

  if (!response_json) {
    return NULL;
  }
  turbo_json_object_set_string(response_json, "jsonrpc", "2.0");
  if (id_json) {
    turbo_json_object_add(response_json, "id", turbo_json_clone(id_json));
  } else {
    turbo_json_object_set_null(response_json, "id");
  }
  return response_json;
}

static int turbo_agent_runtime_remote_build_error_response(
    const json_value_t *request_json, int error_code, const char *message,
    json_value_t **out_response_json) {
  json_value_t *response_json;
  json_value_t *error_json;

  if (!out_response_json) {
    return -1;
  }
  *out_response_json = NULL;
  response_json = turbo_agent_runtime_remote_response_base(request_json);
  if (!response_json) {
    return -1;
  }
  error_json = turbo_json_create_object();
  if (!error_json) {
    turbo_free_json(&response_json);
    return -1;
  }
  turbo_json_object_set_number(error_json, "code", (double)error_code);
  turbo_json_object_set_string(error_json, "message", message ? message : "Internal error");
  turbo_json_object_add(response_json, "error", error_json);
  *out_response_json = response_json;
  return 0;
}

static int turbo_agent_runtime_remote_build_success_response(
    const json_value_t *request_json, json_value_t **inout_result_json,
    json_value_t **out_response_json) {
  json_value_t *response_json;

  if (!inout_result_json || !*inout_result_json || !out_response_json) {
    return -1;
  }
  *out_response_json = NULL;
  response_json = turbo_agent_runtime_remote_response_base(request_json);
  if (!response_json) {
    return -1;
  }
  turbo_json_object_add(response_json, "result", *inout_result_json);
  *inout_result_json = NULL;
  *out_response_json = response_json;
  return 0;
}

static int turbo_agent_runtime_remote_serialize_response_json(json_value_t *response_json,
                                                              char **out_response_json_text) {
  char *response_json_text;

  if (!response_json || !out_response_json_text) {
    turbo_free_json(&response_json);
    return -1;
  }
  *out_response_json_text = NULL;
  response_json_text = turbo_json_serialize(response_json, NULL);
  turbo_free_json(&response_json);
  if (!response_json_text) {
    return -1;
  }
  *out_response_json_text = response_json_text;
  return 0;
}

static int turbo_agent_runtime_remote_build_error_response_text(
    const json_value_t *request_json, int error_code, const char *message,
    char **out_response_json_text) {
  json_value_t *response_json = NULL;

  if (!out_response_json_text) {
    return -1;
  }
  *out_response_json_text = NULL;
  if (turbo_agent_runtime_remote_build_error_response(request_json, error_code, message,
                                                      &response_json) != 0 ||
      !response_json) {
    turbo_free_json(&response_json);
    return -1;
  }
  return turbo_agent_runtime_remote_serialize_response_json(response_json, out_response_json_text);
}

static http_response_t *turbo_agent_runtime_remote_http_response_create(
    int status_code, const char *headers, const char *body, http_error_code_t error_code,
    const char *error_text) {
  http_response_t *response = (http_response_t *)calloc(1, sizeof(*response));

  if (!response) {
    return NULL;
  }
  response->status_code = status_code;
  response->error_code = error_code;
  if (headers) {
    response->headers = tstr_dup(headers);
    if (!response->headers) {
      http_response_free(response);
      return NULL;
    }
    response->headers_len = strlen(response->headers);
  }
  if (body) {
    response->body = (char *)calloc(strlen(body) + 1, sizeof(*response->body));
    if (!response->body) {
      http_response_free(response);
      return NULL;
    }
    memcpy(response->body, body, strlen(body));
    response->body_len = strlen(body);
  }
  if (error_text) {
    response->error = tstr_dup(error_text);
    if (!response->error) {
      http_response_free(response);
      return NULL;
    }
  }
  return response;
}

static http_response_t *turbo_agent_runtime_remote_http_error_response(
    int status_code, int rpc_error_code, const char *rpc_message) {
  char *body = NULL;
  http_response_t *response;

  if (turbo_agent_runtime_remote_build_error_response_text(NULL, rpc_error_code, rpc_message,
                                                           &body) != 0 ||
      !body) {
    turbo_json_serialize_free(body);
    return turbo_agent_runtime_remote_http_response_create(
        500, "Content-Type: application/json\r\n", NULL, HTTP_ERROR_PARSE_FAILED,
        "Failed to build JSON-RPC error response");
  }
  response = turbo_agent_runtime_remote_http_response_create(
      status_code, "Content-Type: application/json\r\n", body, HTTP_ERROR_NONE, NULL);
  turbo_json_serialize_free(body);
  return response;
}

static int turbo_agent_runtime_remote_validate_request(const json_value_t *request_json,
                                                       const json_value_t **out_params_json,
                                                       const char **out_method) {
  const char *jsonrpc;
  const json_value_t *params_json;
  const char *method;

  if (!request_json || !out_params_json || !out_method ||
      turbo_json_type(request_json) != TURBO_JSON_OBJECT) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST;
  }
  jsonrpc = turbo_json_get_string(request_json, "jsonrpc");
  method = turbo_json_get_string(request_json, "method");
  params_json = turbo_json_object_get(request_json, "params");
  if (!jsonrpc || strcmp(jsonrpc, "2.0") != 0 || !method || method[0] == '\0') {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST;
  }
  if (params_json && turbo_json_type(params_json) != TURBO_JSON_OBJECT &&
      turbo_json_type(params_json) != TURBO_JSON_NULL) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_params_json = (params_json && turbo_json_type(params_json) == TURBO_JSON_OBJECT) ? params_json
                                                                                        : NULL;
  *out_method = method;
  return 0;
}

static int turbo_agent_runtime_remote_collect_string_array(const json_value_t *array_json,
                                                           const char ***out_values,
                                                           size_t *out_count) {
  const char **strings = NULL;
  size_t count;
  size_t i;

  if (!out_values || !out_count) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  *out_values = NULL;
  *out_count = 0;
  if (!array_json) {
    return 0;
  }
  if (turbo_json_type(array_json) != TURBO_JSON_ARRAY) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  count = turbo_json_array_size(array_json);
  if (count == 0) {
    return 0;
  }
  strings = (const char **)calloc(count, sizeof(*strings));
  if (!strings) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  for (i = 0; i < count; ++i) {
    const json_value_t *item = turbo_json_array_get(array_json, i);
    const char *text = turbo_json_string(item);

    if (!text || !text[0]) {
      free(strings);
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
    }
    strings[i] = text;
  }
  *out_values = strings;
  *out_count = count;
  return 0;
}

static void turbo_agent_runtime_remote_free_string_array(const char **values) { free((void *)values); }

static int turbo_agent_runtime_remote_parse_run_options(const json_value_t *params_json,
                                                        turbo_graph_run_options_t *options,
                                                        const char ***out_interrupt_before_nodes) {
  const json_value_t *options_json;
  const json_value_t *max_steps_json;
  const json_value_t *start_node_json;
  const json_value_t *skip_initial_interrupt_json;
  const json_value_t *interrupt_before_nodes_json;
  const char **interrupt_before_nodes = NULL;
  size_t interrupt_before_count = 0;
  double max_steps_value;
  int rc;

  if (!options || !out_interrupt_before_nodes) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  *out_interrupt_before_nodes = NULL;
  if (!params_json) {
    return 0;
  }
  options_json = turbo_json_object_get(params_json, "options");
  if (!options_json) {
    return 0;
  }
  if (turbo_json_type(options_json) != TURBO_JSON_OBJECT) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }

  max_steps_json = turbo_json_object_get(options_json, "max_steps");
  if (max_steps_json) {
    if (turbo_json_type(max_steps_json) != TURBO_JSON_NUMBER) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
    }
    max_steps_value = turbo_json_number(max_steps_json);
    if (max_steps_value < 0.0) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
    }
    options->max_steps = (size_t)max_steps_value;
  }

  start_node_json = turbo_json_object_get(options_json, "start_node");
  if (start_node_json) {
    if (turbo_json_type(start_node_json) != TURBO_JSON_STRING) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
    }
    options->start_node = turbo_json_string(start_node_json);
  }
  if (options->start_node && options->start_node[0] == '\0') {
    options->start_node = NULL;
  }
  skip_initial_interrupt_json = turbo_json_object_get(options_json, "skip_initial_interrupt");
  if (skip_initial_interrupt_json) {
    if (turbo_json_type(skip_initial_interrupt_json) != TURBO_JSON_BOOL) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
    }
    options->skip_initial_interrupt = turbo_json_bool(skip_initial_interrupt_json);
  }

  interrupt_before_nodes_json = turbo_json_object_get(options_json, "interrupt_before_nodes");
  if (interrupt_before_nodes_json) {
    rc = turbo_agent_runtime_remote_collect_string_array(interrupt_before_nodes_json,
                                                         &interrupt_before_nodes,
                                                         &interrupt_before_count);
    if (rc != 0) {
      return rc;
    }
    options->interrupt_before_nodes = (const char *const *)interrupt_before_nodes;
    options->interrupt_before_count = interrupt_before_count;
    *out_interrupt_before_nodes = interrupt_before_nodes;
  }

  return 0;
}

static void turbo_agent_runtime_remote_cleanup_run_options(const char **interrupt_before_nodes) {
  turbo_agent_runtime_remote_free_string_array(interrupt_before_nodes);
}

static turbo_graph_t *turbo_agent_runtime_remote_resolve_graph(turbo_agent_runtime_remote_t *remote,
                                                              const char *graph_name) {
  if (!remote || !graph_name || !graph_name[0] || !remote->graph_resolver) {
    return NULL;
  }
  return remote->graph_resolver(graph_name, remote->graph_resolver_user_data);
}

static int turbo_agent_runtime_remote_wrap_object(json_value_t *payload,
                                                  const char *field_name,
                                                  json_value_t **out_result_json) {
  json_value_t *result_json;

  if (!payload || !field_name || !out_result_json) {
    turbo_free_json(&payload);
    return -1;
  }
  result_json = turbo_json_create_object();
  if (!result_json) {
    turbo_free_json(&payload);
    return -1;
  }
  turbo_json_object_add(result_json, field_name, payload);
  *out_result_json = result_json;
  return 0;
}

static int turbo_agent_runtime_remote_wrap_summary_state(json_value_t *summary_json,
                                                         json_value_t *state_json,
                                                         json_value_t **out_result_json) {
  json_value_t *result_json;

  if (!summary_json || !state_json || !out_result_json) {
    turbo_free_json(&summary_json);
    turbo_free_json(&state_json);
    return -1;
  }
  result_json = turbo_json_create_object();
  if (!result_json) {
    turbo_free_json(&summary_json);
    turbo_free_json(&state_json);
    return -1;
  }
  turbo_json_object_add(result_json, "summary", summary_json);
  turbo_json_object_add(result_json, "state", state_json);
  *out_result_json = result_json;
  return 0;
}

static int turbo_agent_runtime_remote_parse_command_object(
    const json_value_t *params_json, turbo_runtime_data_bind_value_t **out_command_bind) {
  const json_value_t *command_json;
  const char *kind;
  turbo_runtime_data_bind_value_t *command_bind;

  if (!params_json || !out_command_bind) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_command_bind = NULL;
  command_json = turbo_json_object_get(params_json, "command");
  kind = command_json ? turbo_json_get_string(command_json, "kind") : NULL;
  if (!command_json || turbo_json_type(command_json) != TURBO_JSON_OBJECT || !kind || !kind[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  command_bind = turbo_runtime_data_bind_value_from_json(command_json);
  if (!command_bind) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  *out_command_bind = command_bind;
  return 0;
}

static int turbo_agent_runtime_remote_dispatch_start(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json,
    json_value_t **out_result_json) {
  const char *graph_name;
  const char *thread_id;
  const json_value_t *state_json;
  turbo_runtime_data_bind_value_t *state_bind = NULL;
  turbo_runtime_data_bind_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  turbo_graph_t *graph = NULL;
  turbo_graph_run_options_t options = {0};
  const char **owned_interrupt_before_nodes = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  graph_name = turbo_json_get_string(params_json, "graph_name");
  thread_id = turbo_json_get_string(params_json, "thread_id");
  state_json = turbo_json_object_get(params_json, "state");
  if (turbo_json_object_get(params_json, "graph_name") &&
      turbo_json_type(turbo_json_object_get(params_json, "graph_name")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (turbo_json_object_get(params_json, "thread_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "thread_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!graph_name || !graph_name[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (state_json && turbo_json_type(state_json) != TURBO_JSON_NULL) {
    state_bind = turbo_runtime_data_bind_value_from_json(state_json);
    if (!state_bind) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
    }
  } else {
    state_bind = turbo_agent_state_create_bind();
    if (!state_bind) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
    }
  }

  rc = turbo_agent_runtime_remote_parse_run_options(params_json, &options, &owned_interrupt_before_nodes);
  if (rc != 0) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(state_bind);
    return rc;
  }

  graph = turbo_agent_runtime_remote_resolve_graph(remote, graph_name);
  if (!graph) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(state_bind);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  rc = turbo_agent_runtime_start_bind_graph(remote->runtime, graph, state_bind, &options,
                                            thread_id, &summary_json, &result_state);
  turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
  turbo_runtime_data_bind_value_destroy(state_bind);
  if (rc != 0 || !summary_json || !result_state) {
    turbo_free_json(&summary_json);
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (turbo_agent_runtime_remote_wrap_summary_state(
          summary_json, turbo_runtime_data_bind_value_to_json(result_state), out_result_json) != 0) {
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  turbo_runtime_data_bind_value_destroy(result_state);
  return 0;
}

static int turbo_agent_runtime_remote_dispatch_resume_or_fork(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json, int fork_mode,
    json_value_t **out_result_json) {
  const char *graph_name;
  const char *checkpoint_id;
  const json_value_t *state_json;
  turbo_runtime_data_bind_value_t *state_override = NULL;
  turbo_runtime_data_bind_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  turbo_graph_t *graph = NULL;
  turbo_graph_run_options_t options = {0};
  const char **owned_interrupt_before_nodes = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  graph_name = turbo_json_get_string(params_json, "graph_name");
  checkpoint_id = turbo_json_get_string(params_json, "checkpoint_id");
  state_json = turbo_json_object_get(params_json, "state");
  if (turbo_json_object_get(params_json, "graph_name") &&
      turbo_json_type(turbo_json_object_get(params_json, "graph_name")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (turbo_json_object_get(params_json, "checkpoint_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "checkpoint_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!graph_name || !graph_name[0] || !checkpoint_id || !checkpoint_id[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (state_json && turbo_json_type(state_json) != TURBO_JSON_NULL) {
    state_override = turbo_runtime_data_bind_value_from_json(state_json);
    if (!state_override) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
    }
  }

  rc = turbo_agent_runtime_remote_parse_run_options(params_json, &options, &owned_interrupt_before_nodes);
  if (rc != 0) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(state_override);
    return rc;
  }

  graph = turbo_agent_runtime_remote_resolve_graph(remote, graph_name);
  if (!graph) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(state_override);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (fork_mode) {
    rc = turbo_agent_runtime_fork_bind_graph(remote->runtime, graph, checkpoint_id, state_override,
                                             &options, &summary_json, &result_state);
  } else {
    rc = turbo_agent_runtime_resume_bind_graph(remote->runtime, graph, checkpoint_id,
                                               state_override, &options, &summary_json,
                                               &result_state);
  }
  turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
  turbo_runtime_data_bind_value_destroy(state_override);
  if (rc != 0 || !summary_json || !result_state) {
    turbo_free_json(&summary_json);
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (turbo_agent_runtime_remote_wrap_summary_state(
          summary_json, turbo_runtime_data_bind_value_to_json(result_state), out_result_json) != 0) {
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  turbo_runtime_data_bind_value_destroy(result_state);
  return 0;
}

static int turbo_agent_runtime_remote_dispatch_apply_command(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json,
    json_value_t **out_result_json) {
  const char *checkpoint_id;
  turbo_runtime_data_bind_value_t *command_bind = NULL;
  turbo_runtime_data_bind_value_t *state_override = NULL;
  json_value_t *state_json = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  checkpoint_id = turbo_json_get_string(params_json, "checkpoint_id");
  if (turbo_json_object_get(params_json, "checkpoint_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "checkpoint_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!checkpoint_id || !checkpoint_id[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }

  rc = turbo_agent_runtime_remote_parse_command_object(params_json, &command_bind);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_agent_runtime_apply_command_bind(remote->runtime, checkpoint_id, command_bind,
                                              &state_override);
  turbo_runtime_data_bind_value_destroy(command_bind);
  if (rc != 0 || !state_override) {
    turbo_runtime_data_bind_value_destroy(state_override);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  state_json = turbo_runtime_data_bind_value_to_json(state_override);
  turbo_runtime_data_bind_value_destroy(state_override);
  if (!state_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  if (turbo_agent_runtime_remote_wrap_object(state_json, "state", out_result_json) != 0) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  return 0;
}

static int turbo_agent_runtime_remote_dispatch_thread_command_run(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json, int fork_mode,
    json_value_t **out_result_json) {
  const char *graph_name;
  const char *thread_id;
  turbo_runtime_data_bind_value_t *command_bind = NULL;
  turbo_runtime_data_bind_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  turbo_graph_t *graph = NULL;
  turbo_graph_run_options_t options = {0};
  const char **owned_interrupt_before_nodes = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  graph_name = turbo_json_get_string(params_json, "graph_name");
  thread_id = turbo_json_get_string(params_json, "thread_id");
  if (turbo_json_object_get(params_json, "graph_name") &&
      turbo_json_type(turbo_json_object_get(params_json, "graph_name")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (turbo_json_object_get(params_json, "thread_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "thread_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!graph_name || !graph_name[0] || !thread_id || !thread_id[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }

  rc = turbo_agent_runtime_remote_parse_command_object(params_json, &command_bind);
  if (rc != 0) {
    return rc;
  }
  rc = turbo_agent_runtime_remote_parse_run_options(params_json, &options, &owned_interrupt_before_nodes);
  if (rc != 0) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(command_bind);
    return rc;
  }

  graph = turbo_agent_runtime_remote_resolve_graph(remote, graph_name);
  if (!graph) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(command_bind);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (fork_mode) {
    rc = turbo_agent_runtime_fork_thread_command_bind(remote->runtime, graph, thread_id, command_bind,
                                                      &options, &summary_json, &result_state);
  } else {
    rc = turbo_agent_runtime_resume_thread_command_bind(remote->runtime, graph, thread_id,
                                                        command_bind, &options, &summary_json,
                                                        &result_state);
  }
  turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
  turbo_runtime_data_bind_value_destroy(command_bind);
  if (rc != 0 || !summary_json || !result_state) {
    turbo_free_json(&summary_json);
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (turbo_agent_runtime_remote_wrap_summary_state(
          summary_json, turbo_runtime_data_bind_value_to_json(result_state), out_result_json) != 0) {
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  turbo_runtime_data_bind_value_destroy(result_state);
  return 0;
}

static int turbo_agent_runtime_remote_dispatch_thread_state_run(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json, int fork_mode,
    json_value_t **out_result_json) {
  const char *graph_name;
  const char *thread_id;
  const json_value_t *state_json;
  turbo_runtime_data_bind_value_t *state_override = NULL;
  turbo_runtime_data_bind_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  turbo_graph_t *graph = NULL;
  turbo_graph_run_options_t options = {0};
  const char **owned_interrupt_before_nodes = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  graph_name = turbo_json_get_string(params_json, "graph_name");
  thread_id = turbo_json_get_string(params_json, "thread_id");
  state_json = turbo_json_object_get(params_json, "state");
  if (turbo_json_object_get(params_json, "graph_name") &&
      turbo_json_type(turbo_json_object_get(params_json, "graph_name")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (turbo_json_object_get(params_json, "thread_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "thread_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!graph_name || !graph_name[0] || !thread_id || !thread_id[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (state_json && turbo_json_type(state_json) != TURBO_JSON_NULL) {
    state_override = turbo_runtime_data_bind_value_from_json(state_json);
    if (!state_override) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
    }
  }

  rc = turbo_agent_runtime_remote_parse_run_options(params_json, &options, &owned_interrupt_before_nodes);
  if (rc != 0) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(state_override);
    return rc;
  }

  graph = turbo_agent_runtime_remote_resolve_graph(remote, graph_name);
  if (!graph) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(state_override);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (fork_mode) {
    rc = turbo_agent_runtime_fork_thread_bind_graph(remote->runtime, graph, thread_id, state_override,
                                                    &options, &summary_json, &result_state);
  } else {
    rc = turbo_agent_runtime_resume_thread_bind_graph(remote->runtime, graph, thread_id,
                                                      state_override, &options, &summary_json,
                                                      &result_state);
  }
  turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
  turbo_runtime_data_bind_value_destroy(state_override);
  if (rc != 0 || !summary_json || !result_state) {
    turbo_free_json(&summary_json);
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (turbo_agent_runtime_remote_wrap_summary_state(
          summary_json, turbo_runtime_data_bind_value_to_json(result_state), out_result_json) != 0) {
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  turbo_runtime_data_bind_value_destroy(result_state);
  return 0;
}

static int turbo_agent_runtime_remote_dispatch_get_thread_state(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json,
    json_value_t **out_result_json) {
  const char *thread_id;
  turbo_runtime_data_bind_value_t *state_bind = NULL;
  json_value_t *state_json = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  thread_id = turbo_json_get_string(params_json, "thread_id");
  if (turbo_json_object_get(params_json, "thread_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "thread_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!thread_id || !thread_id[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }

  rc = turbo_agent_runtime_get_thread_state_bind(remote->runtime, thread_id, &state_bind);
  if (rc != 0 || !state_bind) {
    turbo_runtime_data_bind_value_destroy(state_bind);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  state_json = turbo_runtime_data_bind_value_to_json(state_bind);
  turbo_runtime_data_bind_value_destroy(state_bind);
  if (!state_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  return turbo_agent_runtime_remote_wrap_object(state_json, "state", out_result_json);
}

static int turbo_agent_runtime_remote_dispatch_update_thread_state(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json,
    json_value_t **out_result_json) {
  const char *thread_id;
  const json_value_t *state_patch_json;
  turbo_runtime_data_bind_value_t *state_patch_bind = NULL;
  turbo_runtime_data_bind_value_t *state_override = NULL;
  json_value_t *state_json = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  thread_id = turbo_json_get_string(params_json, "thread_id");
  state_patch_json = turbo_json_object_get(params_json, "state_patch");
  if (turbo_json_object_get(params_json, "thread_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "thread_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!thread_id || !thread_id[0] || !state_patch_json ||
      turbo_json_type(state_patch_json) != TURBO_JSON_OBJECT) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }

  state_patch_bind = turbo_runtime_data_bind_value_from_json(state_patch_json);
  if (!state_patch_bind) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  rc = turbo_agent_runtime_update_thread_state_bind(remote->runtime, thread_id, state_patch_bind,
                                                    &state_override);
  turbo_runtime_data_bind_value_destroy(state_patch_bind);
  if (rc != 0 || !state_override) {
    turbo_runtime_data_bind_value_destroy(state_override);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  state_json = turbo_runtime_data_bind_value_to_json(state_override);
  turbo_runtime_data_bind_value_destroy(state_override);
  if (!state_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  return turbo_agent_runtime_remote_wrap_object(state_json, "state", out_result_json);
}

static int turbo_agent_runtime_remote_dispatch_apply_thread_state_patch(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json,
    json_value_t **out_result_json) {
  return turbo_agent_runtime_remote_dispatch_update_thread_state(remote, params_json,
                                                                 out_result_json);
}

static int turbo_agent_runtime_remote_dispatch_get_checkpoint_context(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json,
    json_value_t **out_result_json) {
  const char *checkpoint_id;
  json_value_t *context_json = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  checkpoint_id = turbo_json_get_string(params_json, "checkpoint_id");
  if (turbo_json_object_get(params_json, "checkpoint_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "checkpoint_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!checkpoint_id || !checkpoint_id[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }

  rc = turbo_agent_runtime_get_checkpoint_context(remote->runtime, checkpoint_id, &context_json);
  if (rc != 0 || !context_json) {
    turbo_free_json(&context_json);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  return turbo_agent_runtime_remote_wrap_object(context_json, "context", out_result_json);
}

static int turbo_agent_runtime_remote_dispatch_thread_state_patch_run(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json, int fork_mode,
    json_value_t **out_result_json) {
  const char *graph_name;
  const char *thread_id;
  const json_value_t *state_patch_json;
  turbo_runtime_data_bind_value_t *state_patch_bind = NULL;
  turbo_runtime_data_bind_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  turbo_graph_t *graph = NULL;
  turbo_graph_run_options_t options = {0};
  const char **owned_interrupt_before_nodes = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;

  graph_name = turbo_json_get_string(params_json, "graph_name");
  thread_id = turbo_json_get_string(params_json, "thread_id");
  state_patch_json = turbo_json_object_get(params_json, "state_patch");
  if (turbo_json_object_get(params_json, "graph_name") &&
      turbo_json_type(turbo_json_object_get(params_json, "graph_name")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (turbo_json_object_get(params_json, "thread_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "thread_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!graph_name || !graph_name[0] || !thread_id || !thread_id[0] || !state_patch_json ||
      turbo_json_type(state_patch_json) != TURBO_JSON_OBJECT) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }

  state_patch_bind = turbo_runtime_data_bind_value_from_json(state_patch_json);
  if (!state_patch_bind) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  rc = turbo_agent_runtime_remote_parse_run_options(params_json, &options, &owned_interrupt_before_nodes);
  if (rc != 0) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(state_patch_bind);
    return rc;
  }

  graph = turbo_agent_runtime_remote_resolve_graph(remote, graph_name);
  if (!graph) {
    turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
    turbo_runtime_data_bind_value_destroy(state_patch_bind);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (fork_mode) {
    rc = turbo_agent_runtime_fork_thread_state_bind_graph(remote->runtime, graph, thread_id,
                                                          state_patch_bind, &options, &summary_json,
                                                          &result_state);
  } else {
    rc = turbo_agent_runtime_resume_thread_state_bind_graph(remote->runtime, graph, thread_id,
                                                            state_patch_bind, &options, &summary_json,
                                                            &result_state);
  }
  turbo_agent_runtime_remote_cleanup_run_options(owned_interrupt_before_nodes);
  turbo_runtime_data_bind_value_destroy(state_patch_bind);
  if (rc != 0 || !summary_json || !result_state) {
    turbo_free_json(&summary_json);
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  if (turbo_agent_runtime_remote_wrap_summary_state(
          summary_json, turbo_runtime_data_bind_value_to_json(result_state), out_result_json) != 0) {
    turbo_runtime_data_bind_value_destroy(result_state);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }

  turbo_runtime_data_bind_value_destroy(result_state);
  return 0;
}

static int turbo_agent_runtime_remote_dispatch_get_thread_observability_index(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json,
    json_value_t **out_result_json) {
  const char *thread_id;
  json_value_t *index_json = NULL;

  if (!remote || !remote->runtime || !out_result_json || !params_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  *out_result_json = NULL;
  thread_id = turbo_json_get_string(params_json, "thread_id");
  if (turbo_json_object_get(params_json, "thread_id") &&
      turbo_json_type(turbo_json_object_get(params_json, "thread_id")) != TURBO_JSON_STRING) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (!thread_id || !thread_id[0]) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
  }
  if (turbo_agent_runtime_get_thread_observability_index(remote->runtime, thread_id, &index_json) !=
      0 || !index_json) {
    turbo_free_json(&index_json);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  return turbo_agent_runtime_remote_wrap_object(index_json, "index", out_result_json);
}

static int turbo_agent_runtime_remote_dispatch_list_observability_indexes_filtered(
    turbo_agent_runtime_remote_t *remote, const json_value_t *params_json,
    json_value_t **out_result_json) {
  const json_value_t *filters_source;
  const json_value_t *nested_filters_json;
  turbo_runtime_data_bind_value_t *filters_bind = NULL;
  json_value_t *normalized_filters_json = NULL;
  json_value_t *indexes_json = NULL;
  json_value_t *owned_empty_filters_json = NULL;
  int rc;

  if (!remote || !remote->runtime || !out_result_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  *out_result_json = NULL;

  filters_source = params_json;
  if (params_json) {
    nested_filters_json = turbo_json_object_get(params_json, "filters");
    if (nested_filters_json) {
      if (turbo_json_type(nested_filters_json) != TURBO_JSON_OBJECT) {
        return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS;
      }
      filters_source = nested_filters_json;
    }
  }
  if (!filters_source) {
    owned_empty_filters_json = turbo_json_create_object();
    if (!owned_empty_filters_json) {
      return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
    }
    filters_source = owned_empty_filters_json;
  }
  filters_bind = turbo_runtime_data_bind_value_from_json(filters_source);
  if (!filters_bind) {
    turbo_free_json(&owned_empty_filters_json);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  normalized_filters_json = turbo_runtime_data_bind_value_to_json(filters_bind);
  turbo_runtime_data_bind_value_destroy(filters_bind);
  if (!normalized_filters_json) {
    turbo_free_json(&owned_empty_filters_json);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  turbo_free_json(&owned_empty_filters_json);

  rc = turbo_agent_runtime_list_observability_indexes_filtered(remote->runtime,
                                                               normalized_filters_json,
                                                               &indexes_json);
  turbo_free_json(&normalized_filters_json);
  if (rc != 0 || !indexes_json) {
    turbo_free_json(&indexes_json);
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL;
  }
  return turbo_agent_runtime_remote_wrap_object(indexes_json, "indexes", out_result_json);
}

static int turbo_agent_runtime_remote_dispatch_method(
    turbo_agent_runtime_remote_t *remote, const char *method, const json_value_t *params_json,
    json_value_t **out_result_json) {
  if (!method || !out_result_json) {
    return TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST;
  }
  if (strcmp(method, "runtime.start") == 0) {
    return turbo_agent_runtime_remote_dispatch_start(remote, params_json, out_result_json);
  }
  if (strcmp(method, "runtime.resume") == 0) {
    return turbo_agent_runtime_remote_dispatch_resume_or_fork(remote, params_json, 0,
                                                              out_result_json);
  }
  if (strcmp(method, "runtime.fork") == 0) {
    return turbo_agent_runtime_remote_dispatch_resume_or_fork(remote, params_json, 1,
                                                              out_result_json);
  }
  if (strcmp(method, "runtime.resumeThreadCommandBindGraph") == 0) {
    return turbo_agent_runtime_remote_dispatch_thread_command_run(remote, params_json, 0,
                                                                  out_result_json);
  }
  if (strcmp(method, "runtime.forkThreadCommandBindGraph") == 0) {
    return turbo_agent_runtime_remote_dispatch_thread_command_run(remote, params_json, 1,
                                                                  out_result_json);
  }
  if (strcmp(method, "runtime.resumeThreadBindGraph") == 0) {
    return turbo_agent_runtime_remote_dispatch_thread_state_run(remote, params_json, 0,
                                                                out_result_json);
  }
  if (strcmp(method, "runtime.forkThreadBindGraph") == 0) {
    return turbo_agent_runtime_remote_dispatch_thread_state_run(remote, params_json, 1,
                                                                out_result_json);
  }
  if (strcmp(method, "runtime.applyCommand") == 0) {
    return turbo_agent_runtime_remote_dispatch_apply_command(remote, params_json,
                                                             out_result_json);
  }
  if (strcmp(method, "runtime.applyThreadStatePatch") == 0) {
    return turbo_agent_runtime_remote_dispatch_apply_thread_state_patch(remote, params_json,
                                                                        out_result_json);
  }
  if (strcmp(method, "runtime.getThreadState") == 0) {
    return turbo_agent_runtime_remote_dispatch_get_thread_state(remote, params_json,
                                                                out_result_json);
  }
  if (strcmp(method, "runtime.updateThreadState") == 0) {
    return turbo_agent_runtime_remote_dispatch_update_thread_state(remote, params_json,
                                                                   out_result_json);
  }
  if (strcmp(method, "runtime.getCheckpointContext") == 0) {
    return turbo_agent_runtime_remote_dispatch_get_checkpoint_context(remote, params_json,
                                                                      out_result_json);
  }
  if (strcmp(method, "runtime.resumeThreadStatePatchBindGraph") == 0) {
    return turbo_agent_runtime_remote_dispatch_thread_state_patch_run(remote, params_json, 0,
                                                                      out_result_json);
  }
  if (strcmp(method, "runtime.forkThreadStatePatchBindGraph") == 0) {
    return turbo_agent_runtime_remote_dispatch_thread_state_patch_run(remote, params_json, 1,
                                                                      out_result_json);
  }
  if (strcmp(method, "runtime.getThreadObservabilityIndex") == 0) {
    return turbo_agent_runtime_remote_dispatch_get_thread_observability_index(
        remote, params_json, out_result_json);
  }
  if (strcmp(method, "runtime.listObservabilityIndexesFiltered") == 0) {
    return turbo_agent_runtime_remote_dispatch_list_observability_indexes_filtered(
        remote, params_json, out_result_json);
  }
  return TURBO_AGENT_RUNTIME_REMOTE_RPC_METHOD_NOT_FOUND;
}

CXX_C_API turbo_agent_runtime_remote_t *turbo_agent_runtime_remote_create(
    const turbo_agent_runtime_remote_config_t *config) {
  turbo_agent_runtime_remote_t *remote;

  if (!config || !config->runtime) {
    return NULL;
  }
  remote = (turbo_agent_runtime_remote_t *)calloc(1, sizeof(*remote));
  if (!remote) {
    return NULL;
  }
  remote->runtime = config->runtime;
  remote->graph_resolver = config->graph_resolver;
  remote->graph_resolver_user_data = config->graph_resolver_user_data;
  return remote;
}

CXX_C_API void turbo_agent_runtime_remote_destroy(turbo_agent_runtime_remote_t *remote) {
  free(remote);
}

CXX_C_API int turbo_agent_runtime_remote_dispatch_jsonrpc(
    turbo_agent_runtime_remote_t *remote, const json_value_t *request_json,
    json_value_t **out_response_json) {
  const json_value_t *params_json = NULL;
  const char *method = NULL;
  json_value_t *result_json = NULL;
  int rc;

  if (!out_response_json) {
    return -1;
  }
  *out_response_json = NULL;
  if (!remote || !remote->runtime) {
    return turbo_agent_runtime_remote_build_error_response(
        request_json, TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL, "Runtime remote not initialized",
        out_response_json);
  }

  rc = turbo_agent_runtime_remote_validate_request(request_json, &params_json, &method);
  if (rc != 0) {
    return turbo_agent_runtime_remote_build_error_response(
        request_json, rc,
        rc == TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS
            ? "Invalid params"
            : "Invalid request",
        out_response_json);
  }

  rc = turbo_agent_runtime_remote_dispatch_method(remote, method, params_json, &result_json);
  if (rc != 0) {
    return turbo_agent_runtime_remote_build_error_response(
        request_json, rc,
        rc == TURBO_AGENT_RUNTIME_REMOTE_RPC_METHOD_NOT_FOUND
            ? "Method not found"
            : (rc == TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_PARAMS ? "Invalid params"
                                                                   : "Internal error"),
        out_response_json);
  }
  if (!result_json) {
    return turbo_agent_runtime_remote_build_error_response(
        request_json, TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL, "Internal error",
        out_response_json);
  }
  if (turbo_agent_runtime_remote_build_success_response(request_json, &result_json,
                                                        out_response_json) != 0) {
    turbo_free_json(&result_json);
    return turbo_agent_runtime_remote_build_error_response(
        request_json, TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL, "Internal error",
        out_response_json);
  }
  return 0;
}

CXX_C_API int turbo_agent_runtime_remote_dispatch_jsonrpc_text(
    turbo_agent_runtime_remote_t *remote, const char *request_json_text,
    char **out_response_json_text) {
  json_value_t *request_json = NULL;
  json_value_t *response_json = NULL;
  int rc;

  if (!out_response_json_text) {
    return -1;
  }
  *out_response_json_text = NULL;

  if (!request_json_text ||
      turbo_parse_json((const uint8_t *)request_json_text, strlen(request_json_text), &request_json) !=
          0 ||
      !request_json) {
    turbo_free_json(&request_json);
    return turbo_agent_runtime_remote_build_error_response_text(
        NULL, TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST, "Invalid request",
        out_response_json_text);
  }

  rc = turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json);
  turbo_free_json(&request_json);
  if (rc != 0 || !response_json) {
    turbo_free_json(&response_json);
    return turbo_agent_runtime_remote_build_error_response_text(
        NULL, TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL, "Internal error",
        out_response_json_text);
  }
  return turbo_agent_runtime_remote_serialize_response_json(response_json, out_response_json_text);
}

CXX_C_API http_response_t *turbo_agent_runtime_remote_handle_http_jsonrpc(
    turbo_agent_runtime_remote_t *remote, http_method_t method, const char *path,
    const char *request_json_text) {
  char *response_json_text = NULL;
  http_response_t *response;
  json_value_t *request_json = NULL;
  int rc;

  if (!remote || !remote->runtime) {
    return turbo_agent_runtime_remote_http_error_response(
        500, TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL, "Internal error");
  }
  if (method != HTTP_POST) {
    return turbo_agent_runtime_remote_http_error_response(
        405, TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST, "Method not allowed");
  }
  if (!path || strcmp(path, TURBO_AGENT_RUNTIME_REMOTE_HTTP_JSONRPC_PATH) != 0) {
    return turbo_agent_runtime_remote_http_error_response(
        404, TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST, "Not found");
  }
  if (!request_json_text) {
    return turbo_agent_runtime_remote_http_error_response(
        400, TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST, "Invalid request");
  }
  if (turbo_parse_json((const uint8_t *)request_json_text, strlen(request_json_text), &request_json) !=
          0 ||
      !request_json) {
    turbo_free_json(&request_json);
    return turbo_agent_runtime_remote_http_error_response(
        400, TURBO_AGENT_RUNTIME_REMOTE_RPC_INVALID_REQUEST, "Invalid request");
  }
  turbo_free_json(&request_json);

  rc = turbo_agent_runtime_remote_dispatch_jsonrpc_text(remote, request_json_text,
                                                        &response_json_text);
  if (rc != 0 || !response_json_text) {
    turbo_json_serialize_free(response_json_text);
    return turbo_agent_runtime_remote_http_error_response(
        500, TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL, "Internal error");
  }

  response = turbo_agent_runtime_remote_http_response_create(
      200, "Content-Type: application/json\r\n", response_json_text, HTTP_ERROR_NONE, NULL);
  turbo_json_serialize_free(response_json_text);
  if (!response) {
    return turbo_agent_runtime_remote_http_error_response(
        500, TURBO_AGENT_RUNTIME_REMOTE_RPC_INTERNAL, "Internal error");
  }
  return response;
}
