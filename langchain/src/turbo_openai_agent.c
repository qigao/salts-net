#include "turbo_openai_agent.h"
#include "turbo_openai_agent_graph.h"
#include "turbo_openai_agent_internal.h"
#include "turbo_openai_agent_workflow.h"

#include "turbo_action_tool.h"
#include "turbo_agent_policy.h"
#include "turbo_model_provider.h"
#include "turbo_parser.h"
#include "turbo_prompt.h"
#include "turbo_tool_registry.h"
#include "turbo_tool_schema.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct turbo_openai_agent_s {
  char *api_key;
  char *model;
  char *base_url;
  char *endpoint_path;
  char *instructions;
  char *structured_output_name;
  char *structured_output_schema_json;
  int structured_output_strict;
  size_t structured_output_max_retries;
  turbo_openai_api_mode_t api_mode;
  int stream_response;
  int parallel_tool_calls;
  int owns_http_client;
  int owns_tool_registry;
  http_client_t *http_client;
  turbo_tool_registry_t *tool_registry;
  turbo_openai_transport_fn transport_fn;
  void *transport_user_data;
  void *owned_resource;
  turbo_openai_agent_owned_resource_free_fn owned_resource_free;
  const turbo_model_provider_t *provider;
  turbo_openai_agent_middleware_t *middlewares;
  size_t middleware_count;
  size_t middleware_capacity;
  turbo_openai_agent_guardrail_t *guardrails;
  size_t guardrail_count;
  size_t guardrail_capacity;
  turbo_openai_agent_trace_sink_t *trace_sinks;
  size_t trace_sink_count;
  size_t trace_sink_capacity;
  turbo_openai_agent_trace_bind_sink_t *trace_bind_sinks;
  size_t trace_bind_sink_count;
  size_t trace_bind_sink_capacity;
  int capture_trace_history;
  char *last_stream_sse;
  size_t last_stream_sse_len;
  turbo_openai_agent_store_t store;
  int has_store;
};

static json_value_t *
turbo_openai_agent_state_bind_to_json_object(const turbo_runtime_data_bind_value_t *state) {
  json_value_t *json_state;

  if (!state) {
    return NULL;
  }

  json_state = turbo_runtime_data_bind_value_to_json(state);
  if (!json_state) {
    return NULL;
  }

  if (turbo_json_type(json_state) != TURBO_JSON_OBJECT) {
    turbo_free_json(&json_state);
    return NULL;
  }

  return json_state;
}

#define TURBO_OPENAI_AGENT_STATE_SCHEMA_VERSION 1

typedef enum {
  TURBO_OPENAI_RUNNABLE_CALLBACK = 0,
  TURBO_OPENAI_RUNNABLE_PIPE = 1
} turbo_openai_runnable_kind_t;

struct turbo_openai_runnable_s {
  turbo_openai_runnable_kind_t kind;
  turbo_openai_runnable_fn invoke;
  void *user_data;
  turbo_openai_runnable_user_data_free_fn user_data_free;
};

typedef struct {
  const turbo_openai_runnable_t *first;
  const turbo_openai_runnable_t *second;
} turbo_openai_runnable_pipe_t;

typedef struct {
  char *key;
  char *value_json;
} turbo_openai_memory_store_entry_t;

typedef struct {
  turbo_openai_memory_store_entry_t *entries;
  size_t count;
  size_t capacity;
} turbo_openai_memory_store_t;

typedef struct {
  const turbo_action_tool_registry_t *action_registry;
  turbo_agent_policy_t policy;
} turbo_openai_action_policy_guardrail_t;

typedef struct turbo_openai_stream_tool_call_s {
  char *id;
  char *type;
  char *name;
  char *arguments;
} turbo_openai_stream_tool_call_t;

typedef struct turbo_openai_chat_stream_state_s {
  char *id;
  char *role;
  char *content;
  char *finish_reason;
  turbo_openai_stream_tool_call_t *tool_calls;
  size_t tool_call_count;
} turbo_openai_chat_stream_state_t;

typedef struct turbo_openai_anthropic_stream_state_s {
  char *id;
  char *role;
  char *content;
  char *stop_reason;
  turbo_openai_stream_tool_call_t *tool_calls;
  size_t tool_call_count;
} turbo_openai_anthropic_stream_state_t;

static int turbo_openai_agent_invoke_before_model_middlewares(turbo_openai_agent_t *agent,
                                                              json_value_t *state,
                                                              char **inout_request_json);
static int turbo_openai_agent_invoke_after_model_middlewares(turbo_openai_agent_t *agent,
                                                             json_value_t *state,
                                                             const char *request_json,
                                                             char **inout_response_json,
                                                             int transport_status);
static int turbo_openai_agent_invoke_before_tool_middlewares(turbo_openai_agent_t *agent,
                                                             json_value_t *state,
                                                             const char *call_id,
                                                             const char *tool_name,
                                                             char **inout_arguments_json);
static int turbo_openai_agent_invoke_after_tool_middlewares(turbo_openai_agent_t *agent,
                                                            json_value_t *state,
                                                            const char *call_id,
                                                            const char *tool_name,
                                                            const char *arguments_json,
                                                            char **inout_output,
                                                            turbo_tool_status_t tool_status);
static int turbo_openai_agent_guardrail_reserve(turbo_openai_agent_t *agent);
static int turbo_openai_agent_invoke_before_model_guardrails(turbo_openai_agent_t *agent,
                                                             const json_value_t *state,
                                                             const char *request_json,
                                                             char **out_reason);
static int turbo_openai_agent_invoke_after_model_guardrails(turbo_openai_agent_t *agent,
                                                            const json_value_t *state,
                                                            const char *response_json,
                                                            const json_value_t *response,
                                                            char **out_reason);
static int turbo_openai_agent_invoke_before_tool_guardrails(turbo_openai_agent_t *agent,
                                                            const json_value_t *state,
                                                            const char *call_id,
                                                            const char *tool_name,
                                                            const char *arguments_json,
                                                            char **out_reason);
static int turbo_openai_agent_invoke_after_tool_guardrails(turbo_openai_agent_t *agent,
                                                           const json_value_t *state,
                                                           const char *call_id,
                                                           const char *tool_name,
                                                           const char *arguments_json,
                                                           const char *output,
                                                           turbo_tool_status_t tool_status,
                                                           char **out_reason);
static int turbo_openai_agent_action_policy_before_tool(
    turbo_openai_agent_t *agent, const json_value_t *state, const char *call_id,
    const char *tool_name, const char *arguments_json, const char **out_reason,
    void *user_data);
static int turbo_openai_agent_trace_sink_reserve(turbo_openai_agent_t *agent);
static const char *turbo_openai_agent_trace_kind_name(turbo_openai_agent_trace_event_kind_t kind);
static int turbo_openai_agent_append_trace_history(
    json_value_t *state, turbo_openai_agent_trace_event_kind_t kind, const char *name,
    const char *detail, const char *payload, int status);
static void turbo_openai_agent_emit_trace(turbo_openai_agent_t *agent, json_value_t *state,
                                          turbo_openai_agent_trace_event_kind_t kind,
                                          const char *name, const char *detail,
                                          const char *payload, int status);
static int turbo_openai_runnable_pipe_invoke(const json_value_t *input, json_value_t **out_output,
                                             void *user_data);
static int turbo_openai_memory_store_get(void *user_data, const char *key, char **out_value_json);
static int turbo_openai_memory_store_put(void *user_data, const char *key,
                                         const char *value_json);
static int turbo_openai_memory_store_delete(void *user_data, const char *key);
static void turbo_openai_memory_store_destroy(void *user_data);

size_t turbo_openai_agent_state_replan_count(const json_value_t *state);
size_t turbo_openai_agent_state_replan_limit(const json_value_t *state);
const char *turbo_openai_agent_state_replan_reason(const json_value_t *state);
static int turbo_openai_http_transport(const char *request_json, char **out_response_json,
                                       void *user_data);
static const char *turbo_openai_event_output_text(const json_value_t *event);
static const json_value_t *turbo_openai_model_event_tool_calls(const json_value_t *event);
static size_t turbo_openai_model_event_tool_call_count(const json_value_t *event);
static int turbo_openai_tool_call_record_fields(const json_value_t *tool_call,
                                                const char **out_call_id,
                                                const char **out_name,
                                                const char **out_arguments);
static int turbo_openai_tool_result_output_fields(const json_value_t *output_item,
                                                  const char **out_call_id,
                                                  const char **out_output);
static int turbo_openai_message_event_inputs_valid(const json_value_t *event,
                                                   const json_value_t *messages);
static const json_value_t *turbo_openai_tool_results_event_outputs(const json_value_t *event);
static int turbo_openai_request_add_structured_output(
    const turbo_openai_agent_t *agent, json_value_t *request, int chat_mode,
    int compatible_mode, int anthropic_mode);
static int turbo_openai_agent_structured_output_valid_for_state(
    const turbo_openai_agent_t *agent, const json_value_t *state, char **out_reason);
static json_value_t *turbo_openai_agent_build_structured_retry_state(
    const json_value_t *state, size_t attempt_index, const char *reason);
static char *turbo_openai_http_transport_error_detail(const http_response_t *response,
                                                      const char *stream_data,
                                                      size_t stream_len);
static int turbo_openai_json_schema_validate(const json_value_t *value, const json_value_t *schema,
                                             char **out_reason);
static json_value_t *turbo_openai_state_get_or_create_array(json_value_t *state, const char *key);
static char *turbo_openai_agent_memory_context_text_internal(const json_value_t *state);
static char *turbo_openai_agent_build_effective_instructions(const turbo_openai_agent_t *agent,
                                                             const json_value_t *state);

static char *turbo_openai_strdup(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }

  len = strlen(src) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, src, len);
  return copy;
}

static void turbo_openai_free_user_data(void *user_data) { free(user_data); }

static char *turbo_openai_normalize_base_url(const char *src) {
  const char *scheme;
  const char *path;
  size_t min_len = 0;
  size_t len;
  char *copy;
  const char *suffix = "";
  size_t suffix_len = 0;
  char *extended;

  if (!src) {
    return NULL;
  }

  copy = turbo_openai_strdup(src);
  if (!copy) {
    return NULL;
  }

  scheme = strstr(src, "://");
  if (scheme) {
    min_len = (size_t)(scheme - src) + 3;
  }

  len = strlen(copy);
  while (len > min_len && len > 0 && copy[len - 1] == '/') {
    copy[len - 1] = '\0';
    --len;
  }

  path = strchr(copy + min_len, '/');
  if (!path) {
    suffix = "/v1";
    suffix_len = 3;
  } else if (path[1] == '\0') {
    suffix = "v1";
    suffix_len = 2;
  }

  if (suffix_len == 0) {
    return copy;
  }

  extended = (char *)realloc(copy, len + suffix_len + 1);
  if (!extended) {
    free(copy);
    return NULL;
  }

  memcpy(extended + len, suffix, suffix_len);
  extended[len + suffix_len] = '\0';
  return extended;
}

static char *turbo_openai_normalize_endpoint_path(const char *src) {
  size_t start = 0;
  size_t end;
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }

  len = strlen(src);
  while (start < len && src[start] == '/') {
    ++start;
  }

  end = len;
  while (end > start && src[end - 1] == '/') {
    --end;
  }

  copy = (char *)malloc(end - start + 1);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, src + start, end - start);
  copy[end - start] = '\0';
  return copy;
}

static int turbo_openai_build_responses_turn_request(const turbo_openai_agent_t *agent,
                                                     json_value_t *state,
                                                     char **out_request_json);
static int turbo_openai_build_chat_turn_request(const turbo_openai_agent_t *agent,
                                                json_value_t *state, char **out_request_json);
static int turbo_openai_build_compatible_chat_turn_request(const turbo_openai_agent_t *agent,
                                                           json_value_t *state,
                                                           char **out_request_json);
static int turbo_openai_build_anthropic_messages_turn_request(const turbo_openai_agent_t *agent,
                                                              json_value_t *state,
                                                              char **out_request_json);
int turbo_openai_chat_sse_to_json(const char *sse_data, size_t sse_len,
                                  char **out_response_json);
int turbo_openai_responses_sse_to_json(const char *sse_data, size_t sse_len,
                                       char **out_response_json);
int turbo_openai_anthropic_messages_sse_to_json(const char *sse_data, size_t sse_len,
                                                char **out_response_json);
static char *turbo_openai_join_text_blocks(const char *left, const char *separator,
                                           const char *right);
static int turbo_openai_configure_http_client_openai(const turbo_openai_agent_t *agent,
                                                     http_client_t *http_client);
static int turbo_openai_configure_http_client_anthropic(const turbo_openai_agent_t *agent,
                                                        http_client_t *http_client);
static const turbo_model_provider_t turbo_openai_responses_provider = {
    "openai_responses", "responses", turbo_openai_build_responses_turn_request,
    turbo_openai_configure_http_client_openai};

static const turbo_model_provider_t turbo_openai_chat_completions_provider = {
    "openai_chat_completions", "chat/completions", turbo_openai_build_chat_turn_request,
    turbo_openai_configure_http_client_openai};

static const turbo_model_provider_t turbo_openai_compatible_chat_completions_provider = {
    "openai_compatible_chat_completions", "chat/completions",
    turbo_openai_build_compatible_chat_turn_request,
    turbo_openai_configure_http_client_openai};

static const turbo_model_provider_t turbo_anthropic_messages_provider = {
    "anthropic_messages", "messages", turbo_openai_build_anthropic_messages_turn_request,
    turbo_openai_configure_http_client_anthropic};

static const turbo_model_provider_t *
turbo_openai_provider_from_api_mode(turbo_openai_api_mode_t api_mode) {
  return api_mode == TURBO_OPENAI_API_CHAT_COMPLETIONS
             ? turbo_model_provider_openai_chat_completions()
             : turbo_model_provider_openai_responses();
}

static turbo_openai_api_mode_t
turbo_openai_provider_legacy_api_mode(const turbo_model_provider_t *provider) {
  return turbo_model_provider_is_legacy_chat(provider) ? TURBO_OPENAI_API_CHAT_COMPLETIONS
                                                       : TURBO_OPENAI_API_RESPONSES;
}

const turbo_model_provider_t *turbo_model_provider_openai_responses(void) {
  return &turbo_openai_responses_provider;
}

const turbo_model_provider_t *turbo_model_provider_openai_chat_completions(void) {
  return &turbo_openai_chat_completions_provider;
}

const turbo_model_provider_t *turbo_model_provider_openai_compatible_chat_completions(void) {
  return &turbo_openai_compatible_chat_completions_provider;
}

const turbo_model_provider_t *turbo_model_provider_anthropic_messages(void) {
  return &turbo_anthropic_messages_provider;
}

static turbo_graph_exec_status_t turbo_openai_clone_json(const json_value_t *value,
                                                         json_value_t **out_value);

static int turbo_openai_parse_api_mode_env(const char *value,
                                           turbo_openai_api_mode_t *out_mode) {
  if (!out_mode) {
    return -1;
  }

  if (!value || value[0] == '\0') {
    return -1;
  }

  if (strcmp(value, "chat") == 0 || strcmp(value, "chat_completions") == 0 ||
      strcmp(value, "chat-completions") == 0) {
    *out_mode = TURBO_OPENAI_API_CHAT_COMPLETIONS;
    return 0;
  }

  if (strcmp(value, "responses") == 0) {
    *out_mode = TURBO_OPENAI_API_RESPONSES;
    return 0;
  }

  return -1;
}

static int turbo_openai_parse_bool_env(const char *value, int default_value) {
  if (!value || value[0] == '\0') {
    return default_value;
  }

  if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
      strcmp(value, "on") == 0) {
    return 1;
  }

  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
      strcmp(value, "off") == 0) {
    return 0;
  }

  return default_value;
}

static const turbo_model_provider_t *turbo_openai_config_pick_env_provider(
    turbo_openai_agent_config_t *config, const char *provider_env) {
  const turbo_model_provider_t *provider;

  provider = config ? config->provider : NULL;
  if ((!provider || provider->name == NULL) && provider_env && provider_env[0] != '\0') {
    provider = turbo_model_provider_by_name(provider_env);
    if (provider && config) {
      config->provider = provider;
    }
  }

  return provider;
}

static void turbo_openai_config_apply_auth_defaults(turbo_openai_agent_config_t *config,
                                                    const turbo_model_provider_t *provider,
                                                    const char *api_key,
                                                    const char *anthropic_api_key,
                                                    const char *base_url,
                                                    const char *anthropic_base_url,
                                                    const char *model) {
  if (!config) {
    return;
  }

  if (!config->api_key || config->api_key[0] == '\0') {
    config->api_key =
        turbo_model_provider_select_api_key(provider, api_key, anthropic_api_key);
  }

  if (!config->model || config->model[0] == '\0') {
    config->model = (model && model[0] != '\0') ? model : "gpt-5.4";
  }

  if (!config->base_url || config->base_url[0] == '\0') {
    config->base_url = turbo_model_provider_select_base_url(
        provider, base_url, anthropic_base_url, "https://api.openai.com/v1");
  }
}

static const turbo_model_provider_t *turbo_openai_resolve_provider(
    const turbo_model_provider_t *provider, turbo_openai_api_mode_t api_mode,
    turbo_openai_api_mode_t *out_api_mode) {
  const turbo_model_provider_t *resolved;

  resolved = provider ? provider : turbo_openai_provider_from_api_mode(api_mode);
  if (out_api_mode) {
    *out_api_mode = turbo_openai_provider_legacy_api_mode(resolved);
  }
  return resolved;
}

static const turbo_model_provider_t *turbo_openai_config_finalize_provider(
    turbo_openai_agent_config_t *config, const char *api_mode_env) {
  turbo_openai_api_mode_t api_mode;
  const turbo_model_provider_t *provider;

  if (!config) {
    return NULL;
  }

  api_mode = config->api_mode;
  if (!config->provider && turbo_openai_parse_api_mode_env(api_mode_env, &api_mode) == 0) {
    config->api_mode = api_mode;
  }

  provider = turbo_openai_resolve_provider(config->provider, config->api_mode, &config->api_mode);
  config->provider = provider;
  return provider;
}

static int turbo_openai_agent_apply_core_config(turbo_openai_agent_t *agent,
                                                const turbo_openai_agent_config_t *config,
                                                const turbo_model_provider_t *provider) {
  if (!agent || !config || !provider) {
    return -1;
  }

  agent->provider = provider;
  agent->api_mode = turbo_openai_provider_legacy_api_mode(provider);
  agent->model = turbo_openai_strdup(config->model);
  agent->base_url = turbo_openai_normalize_base_url(config->base_url
                                                        ? config->base_url
                                                        : "https://api.openai.com/v1");
  agent->endpoint_path =
      turbo_openai_normalize_endpoint_path(
          turbo_model_provider_select_endpoint_path(provider, config->endpoint_path));
  agent->instructions =
      config->instructions ? turbo_openai_strdup(config->instructions) : NULL;
  agent->structured_output_name =
      config->structured_output_name ? turbo_openai_strdup(config->structured_output_name) : NULL;
  agent->structured_output_schema_json = config->structured_output_schema_json
                                             ? turbo_openai_strdup(
                                                   config->structured_output_schema_json)
                                             : NULL;
  agent->structured_output_strict = config->structured_output_strict ? 1 : 0;
  agent->structured_output_max_retries = config->structured_output_max_retries;
  agent->api_key = config->api_key ? turbo_openai_strdup(config->api_key) : NULL;

  if (!provider->build_request || !agent->model ||
      !agent->base_url || !agent->endpoint_path ||
      (config->instructions && !agent->instructions) ||
      (config->structured_output_name && !agent->structured_output_name) ||
      (config->structured_output_schema_json && !agent->structured_output_schema_json) ||
      (config->api_key && !agent->api_key)) {
    return -1;
  }

  agent->parallel_tool_calls = config->parallel_tool_calls ? 1 : 0;
  agent->stream_response = config->stream_response ? 1 : 0;
  agent->tool_registry = config->tool_registry;
  agent->transport_fn = config->transport_fn;
  agent->transport_user_data = config->transport_user_data;
  return 0;
}

static int turbo_openai_agent_attach_http_client(turbo_openai_agent_t *agent,
                                                 const turbo_openai_agent_config_t *config,
                                                 const turbo_model_provider_t *provider) {
  if (!agent || !config || !provider) {
    return -1;
  }

  if (config->http_client) {
    agent->http_client = config->http_client;
    agent->owns_http_client = 0;
  } else if (!config->transport_fn) {
    agent->http_client = http_client_create(agent->base_url);
    if (!agent->http_client) {
      return -1;
    }
    agent->owns_http_client = 1;
  }

  if (agent->http_client && provider->configure_http_client) {
    if (provider->configure_http_client(agent, agent->http_client) != 0) {
      return -1;
    }
  }

  return 0;
}

static int turbo_openai_agent_parallel_tool_calls_enabled(const turbo_openai_agent_t *agent) {
  if (!agent || !agent->parallel_tool_calls) {
    return 0;
  }

  /*
   * Tool execution still mutates shared state through middleware, guardrails, and traces.
   * Keep requests serial until the runtime can isolate and merge per-tool state safely.
   */
  return 0;
}

static void turbo_openai_agent_clear_last_stream_sse(turbo_openai_agent_t *agent) {
  if (!agent) {
    return;
  }
  free(agent->last_stream_sse);
  agent->last_stream_sse = NULL;
  agent->last_stream_sse_len = 0;
}

static int turbo_openai_agent_capture_last_stream_sse(turbo_openai_agent_t *agent,
                                                      const char *data, size_t len) {
  char *copy;

  if (!agent || !data) {
    return -1;
  }
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, data, len);
  copy[len] = '\0';
  turbo_openai_agent_clear_last_stream_sse(agent);
  agent->last_stream_sse = copy;
  agent->last_stream_sse_len = len;
  return 0;
}

static void turbo_openai_agent_finalize_transport(turbo_openai_agent_t *agent) {
  if (agent && !agent->transport_fn) {
    agent->transport_fn = turbo_openai_http_transport;
    agent->transport_user_data = agent;
  }
}

static void turbo_openai_agent_take_owned_tool_registry(turbo_openai_agent_t *agent,
                                                        turbo_tool_registry_t *tool_registry) {
  if (!agent) {
    return;
  }

  agent->tool_registry = tool_registry;
  agent->owns_tool_registry = tool_registry ? 1 : 0;
}

CXX_C_API int turbo_openai_agent_attach_owned_resource(
    turbo_openai_agent_t *agent, void *resource,
    turbo_openai_agent_owned_resource_free_fn free_resource) {
  if (!agent || !resource || !free_resource) {
    return -1;
  }

  if (agent->owned_resource || agent->owned_resource_free) {
    return -1;
  }

  agent->owned_resource = resource;
  agent->owned_resource_free = free_resource;
  return 0;
}

static void turbo_openai_agent_release_owned_resources(turbo_openai_agent_t *agent) {
  size_t i;

  if (!agent) {
    return;
  }

  if (agent->owns_http_client && agent->http_client) {
    http_client_destroy(agent->http_client);
  }
  if (agent->owns_tool_registry && agent->tool_registry) {
    turbo_tool_registry_destroy(agent->tool_registry);
  }
  if (agent->owned_resource_free) {
    agent->owned_resource_free(agent->owned_resource);
  }
  for (i = 0; i < agent->middleware_count; ++i) {
    if (agent->middlewares[i].user_data_free) {
      agent->middlewares[i].user_data_free(agent->middlewares[i].user_data);
    }
  }
  for (i = 0; i < agent->guardrail_count; ++i) {
    if (agent->guardrails[i].user_data_free) {
      agent->guardrails[i].user_data_free(agent->guardrails[i].user_data);
    }
  }
  for (i = 0; i < agent->trace_sink_count; ++i) {
    if (agent->trace_sinks[i].user_data_free) {
      agent->trace_sinks[i].user_data_free(agent->trace_sinks[i].user_data);
    }
  }
  if (agent->has_store && agent->store.user_data_free) {
    agent->store.user_data_free(agent->store.user_data);
  }
  free(agent->middlewares);
  free(agent->guardrails);
  free(agent->trace_sinks);
  for (i = 0; i < agent->trace_bind_sink_count; ++i) {
    if (agent->trace_bind_sinks[i].user_data_free) {
      agent->trace_bind_sinks[i].user_data_free(agent->trace_bind_sinks[i].user_data);
    }
  }
  free(agent->trace_bind_sinks);
}

static void turbo_openai_agent_free_strings(turbo_openai_agent_t *agent) {
  if (!agent) {
    return;
  }

  free(agent->api_key);
  free(agent->model);
  free(agent->base_url);
  free(agent->endpoint_path);
  free(agent->instructions);
  free(agent->structured_output_name);
  free(agent->structured_output_schema_json);
  turbo_openai_agent_clear_last_stream_sse(agent);
}

static int turbo_openai_agent_middleware_reserve(turbo_openai_agent_t *agent) {
  turbo_openai_agent_middleware_t *resized;
  size_t new_capacity;

  if (!agent) {
    return -1;
  }

  if (agent->middleware_count < agent->middleware_capacity) {
    return 0;
  }

  new_capacity = agent->middleware_capacity == 0 ? 4 : agent->middleware_capacity * 2;
  resized = (turbo_openai_agent_middleware_t *)realloc(agent->middlewares,
                                                       new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  agent->middlewares = resized;
  agent->middleware_capacity = new_capacity;
  return 0;
}

int turbo_openai_agent_add_middleware(turbo_openai_agent_t *agent,
                                      const turbo_openai_agent_middleware_t *middleware) {
  if (!agent || !middleware) {
    return -1;
  }

  if (turbo_openai_agent_middleware_reserve(agent) != 0) {
    return -1;
  }

  agent->middlewares[agent->middleware_count++] = *middleware;
  return 0;
}

static int turbo_openai_agent_guardrail_reserve(turbo_openai_agent_t *agent) {
  turbo_openai_agent_guardrail_t *resized;
  size_t new_capacity;

  if (!agent) {
    return -1;
  }

  if (agent->guardrail_count < agent->guardrail_capacity) {
    return 0;
  }

  new_capacity = agent->guardrail_capacity == 0 ? 4 : agent->guardrail_capacity * 2;
  resized = (turbo_openai_agent_guardrail_t *)realloc(agent->guardrails,
                                                       new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  agent->guardrails = resized;
  agent->guardrail_capacity = new_capacity;
  return 0;
}

int turbo_openai_agent_add_guardrail(turbo_openai_agent_t *agent,
                                     const turbo_openai_agent_guardrail_t *guardrail) {
  if (!agent || !guardrail) {
    return -1;
  }

  if (turbo_openai_agent_guardrail_reserve(agent) != 0) {
    return -1;
  }

  agent->guardrails[agent->guardrail_count++] = *guardrail;
  return 0;
}

static int turbo_openai_agent_action_policy_before_tool(
    turbo_openai_agent_t *agent, const json_value_t *state, const char *call_id,
    const char *tool_name, const char *arguments_json, const char **out_reason,
    void *user_data) {
  turbo_openai_action_policy_guardrail_t *guardrail_data =
      (turbo_openai_action_policy_guardrail_t *)user_data;
  const turbo_action_tool_definition_t *definition;
  turbo_agent_policy_decision_t decision;
  json_value_t *args = NULL;
  const char *reason = NULL;

  (void)agent;
  (void)state;
  (void)call_id;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!guardrail_data || !guardrail_data->action_registry || !tool_name || !arguments_json) {
    return -1;
  }

  definition = turbo_action_tool_registry_find(guardrail_data->action_registry, tool_name);
  if (!definition) {
    if (out_reason) {
      *out_reason = "action_not_found";
    }
    return -1;
  }

  if (turbo_parse_json((const uint8_t *)arguments_json, strlen(arguments_json), &args) != 0) {
    if (out_reason) {
      *out_reason = "invalid_tool_arguments";
    }
    return -1;
  }

  decision = turbo_agent_policy_check_action(&guardrail_data->policy, definition, args, &reason);
  turbo_free_json(&args);
  if (decision == TURBO_AGENT_POLICY_ALLOW) {
    return 0;
  }

  if (out_reason) {
    if (decision == TURBO_AGENT_POLICY_REQUIRE_APPROVAL) {
      *out_reason = reason && reason[0] != '\0' ? reason : "approval_required";
    } else {
      *out_reason = reason && reason[0] != '\0' ? reason : "policy_denied";
    }
  }
  return -1;
}

int turbo_openai_agent_add_action_policy_guardrail(
    turbo_openai_agent_t *agent, const turbo_action_tool_registry_t *action_tool_registry,
    const turbo_agent_policy_t *policy) {
  turbo_openai_action_policy_guardrail_t *guardrail_data;
  turbo_openai_agent_guardrail_t guardrail;

  if (!agent || !action_tool_registry) {
    return -1;
  }

  guardrail_data =
      (turbo_openai_action_policy_guardrail_t *)calloc(1, sizeof(*guardrail_data));
  if (!guardrail_data) {
    return -1;
  }

  guardrail_data->action_registry = action_tool_registry;
  guardrail_data->policy = policy ? *policy : turbo_agent_policy_default();
  memset(&guardrail, 0, sizeof(guardrail));
  guardrail.before_tool = turbo_openai_agent_action_policy_before_tool;
  guardrail.user_data = guardrail_data;
  guardrail.user_data_free = turbo_openai_free_user_data;
  if (turbo_openai_agent_add_guardrail(agent, &guardrail) != 0) {
    free(guardrail_data);
    return -1;
  }

  return 0;
}

int turbo_openai_agent_set_store(turbo_openai_agent_t *agent,
                                 const turbo_openai_agent_store_t *store) {
  if (!agent) {
    return -1;
  }

  if (agent->has_store && agent->store.user_data_free) {
    agent->store.user_data_free(agent->store.user_data);
  }

  memset(&agent->store, 0, sizeof(agent->store));
  agent->has_store = 0;
  if (!store) {
    return 0;
  }

  agent->store = *store;
  agent->has_store = 1;
  return 0;
}

static int turbo_openai_agent_trace_sink_reserve(turbo_openai_agent_t *agent) {
  turbo_openai_agent_trace_sink_t *resized;
  size_t new_capacity;

  if (!agent) {
    return -1;
  }

  if (agent->trace_sink_count < agent->trace_sink_capacity) {
    return 0;
  }

  new_capacity = agent->trace_sink_capacity == 0 ? 2 : agent->trace_sink_capacity * 2;
  resized = (turbo_openai_agent_trace_sink_t *)realloc(agent->trace_sinks,
                                                        new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  agent->trace_sinks = resized;
  agent->trace_sink_capacity = new_capacity;
  return 0;
}

static int turbo_openai_agent_trace_bind_sink_reserve(turbo_openai_agent_t *agent) {
  turbo_openai_agent_trace_bind_sink_t *resized;
  size_t new_capacity;

  if (!agent) {
    return -1;
  }

  if (agent->trace_bind_sink_count < agent->trace_bind_sink_capacity) {
    return 0;
  }

  new_capacity = agent->trace_bind_sink_capacity == 0 ? 2 : agent->trace_bind_sink_capacity * 2;
  resized = (turbo_openai_agent_trace_bind_sink_t *)realloc(agent->trace_bind_sinks,
                                                            new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  agent->trace_bind_sinks = resized;
  agent->trace_bind_sink_capacity = new_capacity;
  return 0;
}

int turbo_openai_agent_add_trace_sink(turbo_openai_agent_t *agent,
                                      const turbo_openai_agent_trace_sink_t *sink) {
  if (!agent || !sink || !sink->callback) {
    return -1;
  }

  if (turbo_openai_agent_trace_sink_reserve(agent) != 0) {
    return -1;
  }

  agent->trace_sinks[agent->trace_sink_count++] = *sink;
  return 0;
}

int turbo_openai_agent_add_trace_bind_sink(
    turbo_openai_agent_t *agent, const turbo_openai_agent_trace_bind_sink_t *sink) {
  if (!agent || !sink || !sink->callback) {
    return -1;
  }

  if (turbo_openai_agent_trace_bind_sink_reserve(agent) != 0) {
    return -1;
  }

  agent->trace_bind_sinks[agent->trace_bind_sink_count++] = *sink;
  return 0;
}

int turbo_openai_agent_set_trace_history_enabled(turbo_openai_agent_t *agent, int enabled) {
  if (!agent) {
    return -1;
  }

  agent->capture_trace_history = enabled ? 1 : 0;
  return 0;
}

static const char *turbo_openai_agent_trace_kind_name(turbo_openai_agent_trace_event_kind_t kind) {
  switch (kind) {
    case TURBO_OPENAI_AGENT_TRACE_MODEL_REQUEST:
      return "model_request";
    case TURBO_OPENAI_AGENT_TRACE_MODEL_RESPONSE:
      return "model_response";
    case TURBO_OPENAI_AGENT_TRACE_TOOL_DISPATCH:
      return "tool_dispatch";
    case TURBO_OPENAI_AGENT_TRACE_TOOL_RESULT:
      return "tool_result";
    case TURBO_OPENAI_AGENT_TRACE_STRUCTURED_RETRY:
      return "structured_retry";
    case TURBO_OPENAI_AGENT_TRACE_REPLAN_REQUESTED:
      return "replan_requested";
    case TURBO_OPENAI_AGENT_TRACE_REVIEW_REQUIRED:
      return "review_required";
    case TURBO_OPENAI_AGENT_TRACE_REVIEW_APPROVED:
      return "review_approved";
    case TURBO_OPENAI_AGENT_TRACE_GUARDRAIL_REJECTED:
      return "guardrail_rejected";
    case TURBO_OPENAI_AGENT_TRACE_MEMORY_LOAD:
      return "memory_load";
    case TURBO_OPENAI_AGENT_TRACE_MEMORY_SAVE:
      return "memory_save";
    default:
      return "unknown";
  }
}

static int turbo_openai_agent_append_trace_history(
    json_value_t *state, turbo_openai_agent_trace_event_kind_t kind, const char *name,
    const char *detail, const char *payload, int status) {
  json_value_t *events;
  turbo_runtime_data_bind_value_t *event_bind;
  json_value_t *event;

  if (!state) {
    return -1;
  }

  events = turbo_openai_state_get_or_create_array(state, "trace_events");
  if (!events) {
    return -1;
  }

  event_bind = turbo_event_trace_create_bind(turbo_openai_agent_trace_kind_name(kind),
                                             detail ? detail : "", payload ? payload : "",
                                             status);
  if (!event_bind) {
    return -1;
  }
  event = turbo_runtime_data_bind_value_to_json(event_bind);
  turbo_runtime_data_bind_value_destroy(event_bind);
  if (!event) {
    return -1;
  }
  turbo_json_object_set_number(event, "kind_code", (double)kind);
  turbo_json_object_set_string(event, "name", name ? name : "");
  turbo_json_array_add(events, event);
  return 0;
}

static void turbo_openai_agent_emit_trace(turbo_openai_agent_t *agent, json_value_t *state,
                                          turbo_openai_agent_trace_event_kind_t kind,
                                          const char *name, const char *detail,
                                          const char *payload, int status) {
  size_t i;

  if (!agent) {
    return;
  }

  if (agent->capture_trace_history) {
    turbo_openai_agent_append_trace_history(state, kind, name, detail, payload, status);
  }

  for (i = 0; i < agent->trace_sink_count; ++i) {
    turbo_openai_agent_trace_sink_t *sink = &agent->trace_sinks[i];
    if (sink->callback) {
      sink->callback(agent, state, kind, name, detail, payload, status, sink->user_data);
    }
  }

  if (agent->trace_bind_sink_count > 0) {
    turbo_runtime_data_bind_value_t *event =
        turbo_event_trace_create_bind(turbo_openai_agent_trace_kind_name(kind), detail, payload,
                                      status);
    if (event) {
      for (i = 0; i < agent->trace_bind_sink_count; ++i) {
        turbo_openai_agent_trace_bind_sink_t *sink = &agent->trace_bind_sinks[i];
        if (sink->callback) {
          sink->callback(agent, event, sink->user_data);
        }
      }
      turbo_runtime_data_bind_value_destroy(event);
    }
  }
}

static int turbo_openai_agent_invoke_before_model_guardrails(turbo_openai_agent_t *agent,
                                                             const json_value_t *state,
                                                             const char *request_json,
                                                             char **out_reason) {
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!agent || !request_json) {
    return -1;
  }

  for (i = 0; i < agent->guardrail_count; ++i) {
    turbo_openai_agent_guardrail_t *guardrail = &agent->guardrails[i];
    const char *reason = NULL;
    if (guardrail->before_model &&
        guardrail->before_model(agent, state, request_json, &reason, guardrail->user_data) != 0) {
      if (out_reason && reason) {
        *out_reason = turbo_openai_strdup(reason);
      }
      return -1;
    }
  }

  return 0;
}

static int turbo_openai_agent_invoke_after_model_guardrails(turbo_openai_agent_t *agent,
                                                            const json_value_t *state,
                                                            const char *response_json,
                                                            const json_value_t *response,
                                                            char **out_reason) {
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!agent) {
    return -1;
  }

  for (i = 0; i < agent->guardrail_count; ++i) {
    turbo_openai_agent_guardrail_t *guardrail = &agent->guardrails[i];
    const char *reason = NULL;
    if (guardrail->after_model &&
        guardrail->after_model(agent, state, response_json, response, &reason,
                               guardrail->user_data) != 0) {
      if (out_reason && reason) {
        *out_reason = turbo_openai_strdup(reason);
      }
      return -1;
    }
  }

  return 0;
}

static int turbo_openai_agent_invoke_before_tool_guardrails(turbo_openai_agent_t *agent,
                                                            const json_value_t *state,
                                                            const char *call_id,
                                                            const char *tool_name,
                                                            const char *arguments_json,
                                                            char **out_reason) {
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!agent || !tool_name || !arguments_json) {
    return -1;
  }

  for (i = 0; i < agent->guardrail_count; ++i) {
    turbo_openai_agent_guardrail_t *guardrail = &agent->guardrails[i];
    const char *reason = NULL;
    if (guardrail->before_tool &&
        guardrail->before_tool(agent, state, call_id, tool_name, arguments_json, &reason,
                               guardrail->user_data) != 0) {
      if (out_reason && reason) {
        *out_reason = turbo_openai_strdup(reason);
      }
      return -1;
    }
  }

  return 0;
}

static int turbo_openai_agent_invoke_after_tool_guardrails(turbo_openai_agent_t *agent,
                                                           const json_value_t *state,
                                                           const char *call_id,
                                                           const char *tool_name,
                                                           const char *arguments_json,
                                                           const char *output,
                                                           turbo_tool_status_t tool_status,
                                                           char **out_reason) {
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!agent || !tool_name || !arguments_json || !output) {
    return -1;
  }

  for (i = 0; i < agent->guardrail_count; ++i) {
    turbo_openai_agent_guardrail_t *guardrail = &agent->guardrails[i];
    const char *reason = NULL;
    if (guardrail->after_tool &&
        guardrail->after_tool(agent, state, call_id, tool_name, arguments_json, output,
                              tool_status, &reason, guardrail->user_data) != 0) {
      if (out_reason && reason) {
        *out_reason = turbo_openai_strdup(reason);
      }
      return -1;
    }
  }

  return 0;
}

turbo_openai_runnable_t *
turbo_openai_runnable_create(const turbo_openai_runnable_config_t *config) {
  turbo_openai_runnable_t *runnable;

  if (!config || !config->invoke) {
    return NULL;
  }

  runnable = (turbo_openai_runnable_t *)calloc(1, sizeof(*runnable));
  if (!runnable) {
    return NULL;
  }

  runnable->kind = TURBO_OPENAI_RUNNABLE_CALLBACK;
  runnable->invoke = config->invoke;
  runnable->user_data = config->user_data;
  runnable->user_data_free = config->user_data_free;
  return runnable;
}

void turbo_openai_runnable_destroy(turbo_openai_runnable_t *runnable) {
  if (!runnable) {
    return;
  }

  if (runnable->user_data_free) {
    runnable->user_data_free(runnable->user_data);
  }
  free(runnable);
}

int turbo_openai_runnable_invoke(const turbo_openai_runnable_t *runnable, const json_value_t *input,
                                 json_value_t **out_output) {
  if (!runnable || !runnable->invoke || !out_output) {
    return -1;
  }

  *out_output = NULL;
  return runnable->invoke(input, out_output, runnable->user_data);
}

static int turbo_openai_runnable_pipe_invoke(const json_value_t *input, json_value_t **out_output,
                                             void *user_data) {
  turbo_openai_runnable_pipe_t *pipe = (turbo_openai_runnable_pipe_t *)user_data;
  json_value_t *middle = NULL;
  int rc;

  if (!pipe || !pipe->first || !pipe->second || !out_output) {
    return -1;
  }

  rc = turbo_openai_runnable_invoke(pipe->first, input, &middle);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_openai_runnable_invoke(pipe->second, middle, out_output);
  turbo_free_json(&middle);
  return rc;
}

turbo_openai_runnable_t *
turbo_openai_runnable_pipe(const turbo_openai_runnable_t *first,
                           const turbo_openai_runnable_t *second) {
  turbo_openai_runnable_pipe_t *pipe;
  turbo_openai_runnable_config_t config;

  if (!first || !second) {
    return NULL;
  }

  pipe = (turbo_openai_runnable_pipe_t *)calloc(1, sizeof(*pipe));
  if (!pipe) {
    return NULL;
  }

  pipe->first = first;
  pipe->second = second;
  memset(&config, 0, sizeof(config));
  config.invoke = turbo_openai_runnable_pipe_invoke;
  config.user_data = pipe;
  config.user_data_free = turbo_openai_free_user_data;
  return turbo_openai_runnable_create(&config);
}

static int turbo_openai_memory_store_find_index(const turbo_openai_memory_store_t *store,
                                                const char *key) {
  size_t i;

  if (!store || !key) {
    return -1;
  }

  for (i = 0; i < store->count; ++i) {
    if (store->entries[i].key && strcmp(store->entries[i].key, key) == 0) {
      return (int)i;
    }
  }

  return -1;
}

static int turbo_openai_memory_store_reserve(turbo_openai_memory_store_t *store) {
  turbo_openai_memory_store_entry_t *resized;
  size_t new_capacity;

  if (!store) {
    return -1;
  }
  if (store->count < store->capacity) {
    return 0;
  }

  new_capacity = store->capacity == 0 ? 8 : store->capacity * 2;
  resized = (turbo_openai_memory_store_entry_t *)realloc(store->entries,
                                                          new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  memset(resized + store->capacity, 0,
         (new_capacity - store->capacity) * sizeof(*resized));
  store->entries = resized;
  store->capacity = new_capacity;
  return 0;
}

static int turbo_openai_memory_store_get(void *user_data, const char *key, char **out_value_json) {
  turbo_openai_memory_store_t *store = (turbo_openai_memory_store_t *)user_data;
  int index;

  if (!store || !key || !out_value_json) {
    return -1;
  }

  *out_value_json = NULL;
  index = turbo_openai_memory_store_find_index(store, key);
  if (index < 0 || !store->entries[index].value_json) {
    return -1;
  }

  *out_value_json = turbo_openai_strdup(store->entries[index].value_json);
  return *out_value_json ? 0 : -1;
}

static int turbo_openai_memory_store_put(void *user_data, const char *key,
                                         const char *value_json) {
  turbo_openai_memory_store_t *store = (turbo_openai_memory_store_t *)user_data;
  int index;
  char *key_copy;
  char *value_copy;

  if (!store || !key || !value_json) {
    return -1;
  }

  index = turbo_openai_memory_store_find_index(store, key);
  value_copy = turbo_openai_strdup(value_json);
  if (!value_copy) {
    return -1;
  }

  if (index >= 0) {
    free(store->entries[index].value_json);
    store->entries[index].value_json = value_copy;
    return 0;
  }

  if (turbo_openai_memory_store_reserve(store) != 0) {
    free(value_copy);
    return -1;
  }

  key_copy = turbo_openai_strdup(key);
  if (!key_copy) {
    free(value_copy);
    return -1;
  }

  store->entries[store->count].key = key_copy;
  store->entries[store->count].value_json = value_copy;
  store->count++;
  return 0;
}

static int turbo_openai_memory_store_delete(void *user_data, const char *key) {
  turbo_openai_memory_store_t *store = (turbo_openai_memory_store_t *)user_data;
  int index;
  size_t i;

  if (!store || !key) {
    return -1;
  }

  index = turbo_openai_memory_store_find_index(store, key);
  if (index < 0) {
    return -1;
  }

  free(store->entries[index].key);
  free(store->entries[index].value_json);
  for (i = (size_t)index + 1; i < store->count; ++i) {
    store->entries[i - 1] = store->entries[i];
  }
  store->count--;
  if (store->count < store->capacity) {
    memset(&store->entries[store->count], 0, sizeof(store->entries[store->count]));
  }
  return 0;
}

static void turbo_openai_memory_store_destroy(void *user_data) {
  turbo_openai_memory_store_t *store = (turbo_openai_memory_store_t *)user_data;
  size_t i;

  if (!store) {
    return;
  }

  for (i = 0; i < store->count; ++i) {
    free(store->entries[i].key);
    free(store->entries[i].value_json);
  }
  free(store->entries);
  free(store);
}

turbo_openai_agent_store_t turbo_openai_agent_store_memory_create(void) {
  turbo_openai_agent_store_t store = {0};
  turbo_openai_memory_store_t *memory_store =
      (turbo_openai_memory_store_t *)calloc(1, sizeof(*memory_store));

  if (!memory_store) {
    return store;
  }

  store.get = turbo_openai_memory_store_get;
  store.put = turbo_openai_memory_store_put;
  store.remove = turbo_openai_memory_store_delete;
  store.user_data = memory_store;
  store.user_data_free = turbo_openai_memory_store_destroy;
  return store;
}

static int turbo_openai_agent_invoke_before_model_middlewares(turbo_openai_agent_t *agent,
                                                              json_value_t *state,
                                                              char **inout_request_json) {
  size_t i;

  if (!agent || !inout_request_json || !*inout_request_json) {
    return -1;
  }

  for (i = 0; i < agent->middleware_count; ++i) {
    turbo_openai_agent_middleware_t *middleware = &agent->middlewares[i];

    if (middleware->before_model &&
        middleware->before_model(agent, state, inout_request_json, middleware->user_data) != 0) {
      return -1;
    }
    if (!*inout_request_json) {
      return -1;
    }
  }

  return 0;
}

static int turbo_openai_agent_invoke_after_model_middlewares(turbo_openai_agent_t *agent,
                                                             json_value_t *state,
                                                             const char *request_json,
                                                             char **inout_response_json,
                                                             int transport_status) {
  size_t i;

  if (!agent || !inout_response_json) {
    return -1;
  }

  for (i = 0; i < agent->middleware_count; ++i) {
    turbo_openai_agent_middleware_t *middleware = &agent->middlewares[i];

    if (middleware->after_model &&
        middleware->after_model(agent, state, request_json, inout_response_json,
                                transport_status, middleware->user_data) != 0) {
      return -1;
    }
    if (transport_status == 0 && !*inout_response_json) {
      return -1;
    }
  }

  return 0;
}

static int turbo_openai_agent_invoke_before_tool_middlewares(turbo_openai_agent_t *agent,
                                                             json_value_t *state,
                                                             const char *call_id,
                                                             const char *tool_name,
                                                             char **inout_arguments_json) {
  size_t i;

  if (!agent || !tool_name || !inout_arguments_json || !*inout_arguments_json) {
    return -1;
  }

  for (i = 0; i < agent->middleware_count; ++i) {
    turbo_openai_agent_middleware_t *middleware = &agent->middlewares[i];

    if (middleware->before_tool &&
        middleware->before_tool(agent, state, call_id, tool_name, inout_arguments_json,
                                middleware->user_data) != 0) {
      return -1;
    }
    if (!*inout_arguments_json) {
      return -1;
    }
  }

  return 0;
}

static int turbo_openai_agent_invoke_after_tool_middlewares(turbo_openai_agent_t *agent,
                                                            json_value_t *state,
                                                            const char *call_id,
                                                            const char *tool_name,
                                                            const char *arguments_json,
                                                            char **inout_output,
                                                            turbo_tool_status_t tool_status) {
  size_t i;

  if (!agent || !tool_name || !arguments_json || !inout_output || !*inout_output) {
    return -1;
  }

  for (i = 0; i < agent->middleware_count; ++i) {
    turbo_openai_agent_middleware_t *middleware = &agent->middlewares[i];

    if (middleware->after_tool &&
        middleware->after_tool(agent, state, call_id, tool_name, arguments_json, inout_output,
                               tool_status, middleware->user_data) != 0) {
      return -1;
    }
    if (!*inout_output) {
      return -1;
    }
  }

  return 0;
}

static turbo_openai_agent_t *turbo_openai_agent_create_with_owned_tool_registry(
    const turbo_openai_agent_config_t *config, turbo_tool_registry_t *tool_registry) {
  turbo_openai_agent_config_t bridged_config;
  turbo_openai_agent_t *agent;

  if (!config || !tool_registry) {
    return NULL;
  }

  bridged_config = *config;
  bridged_config.tool_registry = tool_registry;
  agent = turbo_openai_agent_create(&bridged_config);
  if (!agent) {
    turbo_tool_registry_destroy(tool_registry);
    return NULL;
  }

  turbo_openai_agent_take_owned_tool_registry(agent, tool_registry);
  return agent;
}

static char *turbo_openai_extract_first_json_value(const char *text) {
  size_t i;
  size_t start = 0;
  size_t len;
  char *copy;
  int seen_start = 0;
  int depth = 0;
  int in_string = 0;
  int escaped = 0;

  if (!text) {
    return NULL;
  }

  for (i = 0; text[i] != '\0'; ++i) {
    char ch = text[i];

    if (!seen_start) {
      if (ch == '{' || ch == '[') {
        seen_start = 1;
        start = i;
        depth = 1;
      }
      continue;
    }

    if (in_string) {
      if (escaped) {
        escaped = 0;
      } else if (ch == '\\') {
        escaped = 1;
      } else if (ch == '"') {
        in_string = 0;
      }
      continue;
    }

    if (ch == '"') {
      in_string = 1;
    } else if (ch == '{' || ch == '[') {
      depth++;
    } else if (ch == '}' || ch == ']') {
      depth--;
      if (depth == 0) {
        len = (i + 1) - start;
        copy = (char *)malloc(len + 1);
        if (!copy) {
          return NULL;
        }
        memcpy(copy, text + start, len);
        copy[len] = '\0';
        return copy;
      }
    }
  }

  return NULL;
}

int turbo_openai_agent_config_apply_env(turbo_openai_agent_config_t *config,
                                        const char *env_path,
                                        int overwrite_env) {
  const char *api_key;
  const char *base_url;
  const char *anthropic_api_key;
  const char *anthropic_base_url;
  const char *model;
  const char *provider_env;
  const char *api_mode_env;
  const char *endpoint_path;
  const char *stream_env;
  int env_rc;
  const turbo_model_provider_t *provider;

  if (!config) {
    return -1;
  }

  if (env_path && env_path[0] != '\0') {
    env_rc = turbo_dotenv_load(env_path, overwrite_env ? true : false);
  } else {
    env_rc = turbo_dotenv_load_default(overwrite_env ? true : false);
  }

  api_key = getenv("OPENAI_API_KEY");
  base_url = getenv("OPENAI_BASE_URL");
  anthropic_api_key = getenv("ANTHROPIC_AUTH_TOKEN");
  anthropic_base_url = getenv("ANTHROPIC_BASE_URL");
  model = getenv("OPENAI_MODEL");
  provider_env = getenv("OPENAI_PROVIDER");
  api_mode_env = getenv("OPENAI_API_MODE");
  endpoint_path = getenv("OPENAI_ENDPOINT_PATH");
  stream_env = getenv("OPENAI_STREAM");

  provider = turbo_openai_config_pick_env_provider(config, provider_env);
  turbo_openai_config_apply_auth_defaults(config, provider, api_key, anthropic_api_key, base_url,
                                          anthropic_base_url, model);
  provider = turbo_openai_config_finalize_provider(config, api_mode_env);

  if (!config->endpoint_path || config->endpoint_path[0] == '\0') {
    config->endpoint_path = turbo_model_provider_select_endpoint_path(provider, endpoint_path);
  }

  config->stream_response = turbo_openai_parse_bool_env(stream_env, config->stream_response);
  return env_rc;
}

static turbo_graph_exec_status_t turbo_openai_clone_json(const json_value_t *value,
                                                         json_value_t **out_value);

static int turbo_openai_array_add_clone(json_value_t *array, const json_value_t *value) {
  json_value_t *clone = NULL;

  if (!array || !value) {
    return -1;
  }

  if (turbo_openai_clone_json(value, &clone) != TURBO_GRAPH_EXEC_OK) {
    return -1;
  }

  turbo_json_array_add(array, clone);
  return 0;
}

static char *turbo_openai_join_text_blocks(const char *left, const char *separator,
                                           const char *right) {
  size_t left_len = left ? strlen(left) : 0;
  size_t separator_len = separator ? strlen(separator) : 0;
  size_t right_len = right ? strlen(right) : 0;
  char *joined = (char *)malloc(left_len + separator_len + right_len + 1);

  if (!joined) {
    return NULL;
  }
  if (left_len > 0) {
    memcpy(joined, left, left_len);
  }
  if (separator_len > 0) {
    memcpy(joined + left_len, separator, separator_len);
  }
  if (right_len > 0) {
    memcpy(joined + left_len + separator_len, right, right_len);
  }
  joined[left_len + separator_len + right_len] = '\0';
  return joined;
}

static int turbo_openai_provider_sse_to_response_json(const turbo_model_provider_t *provider,
                                                      const char *sse_data, size_t sse_len,
                                                      char **out_response_json) {
  if (!provider || !sse_data || !out_response_json) {
    return -1;
  }

  *out_response_json = NULL;
  if (provider == turbo_model_provider_openai_responses()) {
    return turbo_openai_responses_sse_to_json(sse_data, sse_len, out_response_json);
  }
  if (turbo_model_provider_is_legacy_chat(provider)) {
    return turbo_openai_chat_sse_to_json(sse_data, sse_len, out_response_json);
  }
  if (turbo_model_provider_is_anthropic_messages(provider)) {
    return turbo_openai_anthropic_messages_sse_to_json(sse_data, sse_len, out_response_json);
  }
  return -1;
}

static int turbo_openai_append_chat_model_event_message(json_value_t *messages,
                                                        const json_value_t *event) {
  const json_value_t *tool_calls = turbo_openai_model_event_tool_calls(event);
  const char *output_text = turbo_openai_event_output_text(event);
  size_t tool_call_count = turbo_openai_model_event_tool_call_count(event);

  if (!turbo_openai_message_event_inputs_valid(event, messages)) {
    return -1;
  }

  if (tool_call_count > 0) {
    json_value_t *assistant_tool_calls = turbo_json_create_array();
    json_value_t *assistant = NULL;
    size_t j;

    if (!assistant_tool_calls) {
      turbo_free_json(&assistant_tool_calls);
      return -1;
    }

    for (j = 0; j < tool_call_count; ++j) {
      const json_value_t *tool_call = turbo_json_array_get(tool_calls, j);
      const char *call_id;
      const char *name;
      const char *arguments;
      json_value_t *chat_tool_call;

      if (!turbo_openai_tool_call_record_fields(tool_call, &call_id, &name, &arguments)) {
        continue;
      }

      chat_tool_call = turbo_prompt_chat_tool_call_create(call_id, "function", name, arguments);
      if (!chat_tool_call) {
        turbo_free_json(&assistant);
        turbo_free_json(&assistant_tool_calls);
        return -1;
      }
      turbo_json_array_add(assistant_tool_calls, chat_tool_call);
    }

    assistant = turbo_prompt_chat_assistant_message_create(output_text, assistant_tool_calls);
    if (!assistant) {
      return -1;
    }

    turbo_json_array_add(messages, assistant);
    return 0;
  }

  if (output_text) {
    json_value_t *assistant = turbo_prompt_message_create("assistant", output_text);
    if (!assistant) {
      return -1;
    }
    turbo_json_array_add(messages, assistant);
  }

  return 0;
}

static int turbo_openai_append_chat_tool_results_messages(json_value_t *messages,
                                                          const json_value_t *event) {
  const json_value_t *outputs = turbo_openai_tool_results_event_outputs(event);
  size_t j;

  if (!turbo_openai_message_event_inputs_valid(event, messages)) {
    return -1;
  }

  if (!outputs) {
    return 0;
  }

  for (j = 0; j < turbo_json_array_size(outputs); ++j) {
    const json_value_t *output = turbo_json_array_get(outputs, j);
    const char *call_id;
    const char *content;
    json_value_t *tool_message;

    if (!turbo_openai_tool_result_output_fields(output, &call_id, &content)) {
      continue;
    }

    tool_message = turbo_prompt_chat_tool_message_create(call_id, content);
    if (!tool_message) {
      return -1;
    }
    turbo_json_array_add(messages, tool_message);
  }

  return 0;
}

static int turbo_openai_append_anthropic_model_event_message(json_value_t *messages,
                                                             const json_value_t *event) {
  const json_value_t *tool_calls = turbo_openai_model_event_tool_calls(event);
  const char *output_text = turbo_openai_event_output_text(event);
  size_t tool_call_count = turbo_openai_model_event_tool_call_count(event);
  json_value_t *assistant;
  json_value_t *content;
  size_t j;

  if (!turbo_openai_message_event_inputs_valid(event, messages)) {
    return -1;
  }

  if ((!output_text || output_text[0] == '\0') && tool_call_count == 0) {
    return 0;
  }

  content = turbo_json_create_array();
  if (!content) {
    turbo_free_json(&content);
    return -1;
  }

  if (output_text && output_text[0] != '\0') {
    json_value_t *text_part = turbo_prompt_content_text_part_create(output_text);
    if (!text_part) {
      turbo_free_json(&content);
      return -1;
    }
    turbo_json_array_add(content, text_part);
  }

  for (j = 0; j < tool_call_count; ++j) {
    const json_value_t *tool_call = turbo_json_array_get(tool_calls, j);
    const char *call_id;
    const char *name;
    const char *arguments;
    json_value_t *tool_use;
    json_value_t *input_object = NULL;

    if (!turbo_openai_tool_call_record_fields(tool_call, &call_id, &name, &arguments)) {
      continue;
    }

    if (turbo_parse_json((const uint8_t *)arguments, strlen(arguments), &input_object) != 0 ||
        !input_object) {
      turbo_free_json(&input_object);
      turbo_free_json(&content);
      return -1;
    }

    tool_use = turbo_prompt_tool_use_part_create(call_id, name, input_object);
    if (!tool_use) {
      turbo_free_json(&content);
      return -1;
    }
    turbo_json_array_add(content, tool_use);
  }

  if (turbo_json_array_size(content) == 0) {
    turbo_free_json(&content);
    return 0;
  }

  assistant = turbo_prompt_message_with_content_create("assistant", content);
  if (!assistant) {
    return -1;
  }

  turbo_json_array_add(messages, assistant);
  return 0;
}

static int turbo_openai_append_anthropic_tool_results_message(json_value_t *messages,
                                                              const json_value_t *event) {
  const json_value_t *outputs = turbo_openai_tool_results_event_outputs(event);
  json_value_t *user_message;
  json_value_t *content;
  size_t j;

  if (!turbo_openai_message_event_inputs_valid(event, messages)) {
    return -1;
  }

  if (!outputs) {
    return 0;
  }

  content = turbo_json_create_array();
  if (!content) {
    turbo_free_json(&content);
    return -1;
  }

  for (j = 0; j < turbo_json_array_size(outputs); ++j) {
    const json_value_t *output = turbo_json_array_get(outputs, j);
    const char *call_id;
    const char *result_text;
    json_value_t *tool_result;

    if (!turbo_openai_tool_result_output_fields(output, &call_id, &result_text)) {
      continue;
    }

    tool_result = turbo_prompt_tool_result_part_create(call_id, result_text);
    if (!tool_result) {
      turbo_free_json(&content);
      return -1;
    }
    turbo_json_array_add(content, tool_result);
  }

  if (turbo_json_array_size(content) == 0) {
    turbo_free_json(&content);
    return 0;
  }

  user_message = turbo_prompt_message_with_content_create("user", content);
  if (!user_message) {
    return -1;
  }

  turbo_json_array_add(messages, user_message);
  return 0;
}

static turbo_graph_exec_status_t turbo_openai_clone_json(const json_value_t *value,
                                                         json_value_t **out_value) {
  if (!out_value) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  *out_value = NULL;
  if (!value) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  *out_value = turbo_json_clone(value);
  return *out_value ? TURBO_GRAPH_EXEC_OK : TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
}

static json_value_t *turbo_openai_state_get_array(json_value_t *state, const char *key) {
  json_value_t *value;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  value = turbo_json_object_get(state, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_ARRAY) {
    return NULL;
  }

  return value;
}

static json_value_t *turbo_openai_state_get_or_create_object(json_value_t *state, const char *key) {
  json_value_t *object;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  object = turbo_json_object_get(state, key);
  if (!object) {
    object = turbo_json_create_object();
    if (!object) {
      return NULL;
    }
    turbo_json_object_add(state, key, object);
  }

  return turbo_json_type(object) == TURBO_JSON_OBJECT ? object : NULL;
}

static json_value_t *turbo_openai_state_get_object(json_value_t *state, const char *key) {
  json_value_t *value;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  value = turbo_json_object_get(state, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  return value;
}

static const json_value_t *turbo_openai_state_get_object_const(const json_value_t *state,
                                                               const char *key) {
  return turbo_openai_state_get_object((json_value_t *)state, key);
}

static const json_value_t *turbo_openai_state_get_array_const(const json_value_t *state,
                                                              const char *key) {
  return turbo_openai_state_get_array((json_value_t *)state, key);
}

static const json_value_t *turbo_openai_array_last_const(const json_value_t *array) {
  size_t count;

  if (!array || turbo_json_type(array) != TURBO_JSON_ARRAY) {
    return NULL;
  }

  count = turbo_json_array_size(array);
  if (count == 0) {
    return NULL;
  }

  return turbo_json_array_get(array, count - 1);
}

static json_value_t *turbo_openai_array_last(json_value_t *array) {
  return (json_value_t *)turbo_openai_array_last_const(array);
}

static int turbo_openai_event_kind_is(const json_value_t *event, const char *kind) {
  const char *event_kind;

  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT || !kind) {
    return 0;
  }

  event_kind = turbo_json_get_string(event, "kind");
  return event_kind && strcmp(event_kind, kind) == 0 ? 1 : 0;
}

static const char *turbo_openai_event_output_text(const json_value_t *event) {
  return event && turbo_json_type(event) == TURBO_JSON_OBJECT
             ? turbo_json_get_string(event, "output_text")
             : NULL;
}

static const json_value_t *turbo_openai_model_event_tool_calls(const json_value_t *event) {
  const json_value_t *tool_calls;

  if (!turbo_openai_event_kind_is(event, "model")) {
    return NULL;
  }

  tool_calls = turbo_json_object_get(event, "tool_calls");
  return tool_calls && turbo_json_type(tool_calls) == TURBO_JSON_ARRAY ? tool_calls : NULL;
}

static size_t turbo_openai_model_event_tool_call_count(const json_value_t *event) {
  const json_value_t *tool_calls = turbo_openai_model_event_tool_calls(event);
  return tool_calls ? turbo_json_array_size(tool_calls) : 0;
}

static const char *turbo_openai_model_event_response_id(const json_value_t *event) {
  return turbo_openai_event_kind_is(event, "model") ? turbo_json_get_string(event, "response_id")
                                                     : NULL;
}

static int turbo_openai_tool_call_record_fields(const json_value_t *tool_call,
                                                const char **call_id,
                                                const char **name,
                                                const char **arguments) {
  const char *call_id_value;
  const char *name_value;
  const char *arguments_value;

  if (call_id) {
    *call_id = NULL;
  }
  if (name) {
    *name = NULL;
  }
  if (arguments) {
    *arguments = NULL;
  }

  if (!tool_call || turbo_json_type(tool_call) != TURBO_JSON_OBJECT) {
    return 0;
  }

  call_id_value = turbo_json_get_string(tool_call, "call_id");
  name_value = turbo_json_get_string(tool_call, "name");
  arguments_value = turbo_json_get_string(tool_call, "arguments");
  if (!call_id_value || !name_value || !arguments_value) {
    return 0;
  }

  if (call_id) {
    *call_id = call_id_value;
  }
  if (name) {
    *name = name_value;
  }
  if (arguments) {
    *arguments = arguments_value;
  }
  return 1;
}

static int turbo_openai_tool_result_output_fields(const json_value_t *output_item,
                                                  const char **call_id,
                                                  const char **output) {
  const char *call_id_value;
  const char *output_value;

  if (call_id) {
    *call_id = NULL;
  }
  if (output) {
    *output = NULL;
  }

  if (!output_item || turbo_json_type(output_item) != TURBO_JSON_OBJECT) {
    return 0;
  }

  call_id_value = turbo_json_get_string(output_item, "call_id");
  output_value = turbo_json_get_string(output_item, "output");
  if (!call_id_value || !output_value) {
    return 0;
  }

  if (call_id) {
    *call_id = call_id_value;
  }
  if (output) {
    *output = output_value;
  }
  return 1;
}

static int turbo_openai_message_event_inputs_valid(const json_value_t *event,
                                                   const json_value_t *messages) {
  return messages && event && turbo_json_type(event) == TURBO_JSON_OBJECT ? 1 : 0;
}

static const json_value_t *turbo_openai_events_last_of_kind(const json_value_t *events,
                                                            const char *kind) {
  size_t i;

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY || !kind) {
    return NULL;
  }

  for (i = turbo_json_array_size(events); i > 0; --i) {
    const json_value_t *event = turbo_json_array_get(events, i - 1);
    if (turbo_openai_event_kind_is(event, kind)) {
      return event;
    }
  }

  return NULL;
}

static const json_value_t *
turbo_openai_state_last_event_of_kind(const json_value_t *state, const char *kind) {
  const json_value_t *events;

  events = turbo_openai_state_get_array_const(state, "events");
  return turbo_openai_events_last_of_kind(events, kind);
}

static const json_value_t *turbo_openai_state_last_event(const json_value_t *state) {
  const json_value_t *events;

  events = turbo_openai_state_get_array_const(state, "events");
  return turbo_openai_array_last_const(events);
}

static int turbo_openai_append_event(json_value_t *state, json_value_t *event) {
  json_value_t *events = turbo_openai_state_get_array(state, "events");
  if (!events || !event) {
    return -1;
  }

  turbo_json_array_add(events, event);
  return 0;
}

static int turbo_openai_append_event_clone(json_value_t *state, const json_value_t *event) {
  json_value_t *clone = NULL;

  if (!state || !event) {
    return -1;
  }

  if (turbo_openai_clone_json(event, &clone) != TURBO_GRAPH_EXEC_OK) {
    return -1;
  }

  return turbo_openai_append_event(state, clone);
}

static int turbo_openai_append_text(char **buffer, size_t *length, const char *text);
static int turbo_openai_agent_copy_model_error(json_value_t *dst_state,
                                               const json_value_t *src_state);
static int turbo_openai_agent_copy_guardrail_rejection(json_value_t *dst_state,
                                                       const json_value_t *src_state);
static int turbo_openai_agent_contains_failure_marker(const char *text);
static const json_value_t *turbo_openai_tool_results_event_outputs(const json_value_t *event);
typedef int (*turbo_openai_message_event_append_fn)(json_value_t *messages,
                                                    const json_value_t *event);

static json_value_t *turbo_openai_tool_call_record_create(const char *call_id, const char *name,
                                                          const char *arguments) {
  json_value_t *call;

  if (!call_id || !name || !arguments) {
    return NULL;
  }

  call = turbo_json_create_object();
  if (!call) {
    return NULL;
  }

  turbo_json_object_set_string(call, "call_id", call_id);
  turbo_json_object_set_string(call, "name", name);
  turbo_json_object_set_string(call, "arguments", arguments);
  return call;
}

static json_value_t *turbo_openai_tool_result_output_item_create(const char *call_id,
                                                                 const char *output) {
  json_value_t *output_item;

  if (!call_id || !output) {
    return NULL;
  }

  output_item = turbo_json_create_object();
  if (!output_item) {
    return NULL;
  }

  turbo_json_object_set_string(output_item, "type", "function_call_output");
  turbo_json_object_set_string(output_item, "call_id", call_id);
  turbo_json_object_set_string(output_item, "output", output);
  return output_item;
}

static json_value_t *turbo_openai_event_create(const char *kind) {
  json_value_t *event;

  if (!kind) {
    return NULL;
  }

  event = turbo_json_create_object();
  if (!event) {
    return NULL;
  }

  turbo_json_object_set_string(event, "kind", kind);
  return event;
}

static json_value_t *turbo_openai_request_create(const turbo_openai_agent_t *agent) {
  json_value_t *request;

  if (!agent || !agent->model) {
    return NULL;
  }

  request = turbo_json_create_object();
  if (!request) {
    return NULL;
  }

  turbo_json_object_set_string(request, "model", agent->model);
  if (agent->stream_response) {
    turbo_json_object_set_bool(request, "stream", true);
  }
  return request;
}

static const char *turbo_openai_response_id(const json_value_t *response) {
  return response && turbo_json_type(response) == TURBO_JSON_OBJECT
             ? turbo_json_get_string(response, "id")
             : NULL;
}

static int turbo_openai_request_add_tools_if_any(json_value_t *request, json_value_t *tools) {
  if (!request || !tools || turbo_json_type(tools) != TURBO_JSON_ARRAY) {
    turbo_free_json(&tools);
    return -1;
  }

  if (turbo_json_array_size(tools) > 0) {
    turbo_json_object_add(request, "tools", tools);
  } else {
    turbo_free_json(&tools);
  }

  return 0;
}

static int turbo_openai_request_serialize_into_output(json_value_t *request,
                                                      char **out_request_json) {
  char *serialized;

  if (!request || !out_request_json) {
    return -1;
  }

  serialized = turbo_json_serialize(request, NULL);
  turbo_free_json(&request);
  if (!serialized) {
    return -1;
  }

  *out_request_json = serialized;
  return 0;
}

static int turbo_openai_request_add_structured_output(
    const turbo_openai_agent_t *agent, json_value_t *request, int chat_mode,
    int compatible_mode, int anthropic_mode) {
  json_value_t *schema = NULL;
  json_value_t *response_format = NULL;
  json_value_t *json_schema = NULL;
  json_value_t *text = NULL;
  json_value_t *format = NULL;

  if (!agent || !request) {
    return -1;
  }

  if (!agent->structured_output_schema_json || agent->structured_output_schema_json[0] == '\0') {
    return 0;
  }

  if (anthropic_mode) {
    return 0;
  }

  if (turbo_parse_json((const uint8_t *)agent->structured_output_schema_json,
                       strlen(agent->structured_output_schema_json), &schema) != 0 ||
      !schema || turbo_json_type(schema) != TURBO_JSON_OBJECT) {
    turbo_free_json(&schema);
    return -1;
  }

  if (chat_mode) {
    response_format = turbo_json_create_object();
    json_schema = turbo_json_create_object();
    if (!response_format || !json_schema) {
      turbo_free_json(&schema);
      turbo_free_json(&response_format);
      turbo_free_json(&json_schema);
      return -1;
    }

    turbo_json_object_set_string(response_format, "type", "json_schema");
    turbo_json_object_set_string(json_schema, "name",
                                 (agent->structured_output_name &&
                                  agent->structured_output_name[0] != '\0')
                                     ? agent->structured_output_name
                                     : "structured_output");
    turbo_json_object_add(json_schema, "schema", schema);
    turbo_json_object_set_bool(json_schema, "strict",
                               agent->structured_output_strict ? true : false);
    turbo_json_object_add(response_format, "json_schema", json_schema);
    turbo_json_object_add(request, compatible_mode ? "response_format" : "response_format",
                          response_format);
    return 0;
  }

  text = turbo_json_create_object();
  format = turbo_json_create_object();
  if (!text || !format) {
    turbo_free_json(&schema);
    turbo_free_json(&text);
    turbo_free_json(&format);
    return -1;
  }

  turbo_json_object_set_string(format, "type", "json_schema");
  turbo_json_object_set_string(format, "name",
                               (agent->structured_output_name &&
                                agent->structured_output_name[0] != '\0')
                                   ? agent->structured_output_name
                                   : "structured_output");
  turbo_json_object_add(format, "schema", schema);
  turbo_json_object_set_bool(format, "strict", agent->structured_output_strict ? true : false);
  turbo_json_object_add(text, "format", format);
  turbo_json_object_add(request, "text", text);
  return 0;
}

static char *turbo_openai_serialize_json_and_free(json_value_t *value) {
  char *serialized;

  if (!value) {
    return NULL;
  }

  serialized = turbo_json_serialize(value, NULL);
  turbo_free_json(&value);
  return serialized;
}

static json_value_t *turbo_openai_build_chat_stream_tool_calls_array(
    const turbo_openai_chat_stream_state_t *state) {
  json_value_t *tool_calls;
  size_t i;

  if (!state || state->tool_call_count == 0) {
    return NULL;
  }

  tool_calls = turbo_json_create_array();
  if (!tool_calls) {
    return NULL;
  }

  for (i = 0; i < state->tool_call_count; ++i) {
    const turbo_openai_stream_tool_call_t *tool_call = &state->tool_calls[i];
    json_value_t *tool_call_object;

    if (!tool_call->id || !tool_call->name || !tool_call->arguments) {
      continue;
    }

    tool_call_object = turbo_prompt_chat_tool_call_create(
        tool_call->id, tool_call->type ? tool_call->type : "function", tool_call->name,
        tool_call->arguments);
    if (!tool_call_object) {
      turbo_free_json(&tool_calls);
      return NULL;
    }

    turbo_json_array_add(tool_calls, tool_call_object);
  }

  return tool_calls;
}

static json_value_t *turbo_openai_stream_tool_call_input_object(
    const turbo_openai_stream_tool_call_t *tool_call) {
  json_value_t *input = NULL;

  if (!tool_call) {
    return NULL;
  }

  if (tool_call->arguments && tool_call->arguments[0] != '\0') {
    if (turbo_parse_json((const uint8_t *)tool_call->arguments, strlen(tool_call->arguments),
                         &input) != 0) {
      turbo_free_json(&input);
      return NULL;
    }
  } else {
    input = turbo_json_create_object();
  }

  return input;
}

static json_value_t *turbo_openai_chat_stream_response_shell_create(
    const turbo_openai_chat_stream_state_t *state, json_value_t **out_choice,
    json_value_t **out_message) {
  json_value_t *response;
  json_value_t *choices;
  json_value_t *choice;
  json_value_t *message;

  if (!state || !state->id || !out_choice || !out_message) {
    return NULL;
  }

  response = turbo_json_create_object();
  choices = turbo_json_create_array();
  choice = turbo_json_create_object();
  message = turbo_prompt_message_create(state->role && state->role[0] != '\0' ? state->role
                                                                               : "assistant",
                                        state->content ? state->content : "");
  if (!response || !choices || !choice || !message) {
    turbo_free_json(&response);
    turbo_free_json(&choices);
    turbo_free_json(&choice);
    turbo_free_json(&message);
    return NULL;
  }

  turbo_json_object_set_string(response, "id", state->id);
  turbo_json_object_set_string(response, "object", "chat.completion");
  turbo_json_object_set_number(choice, "index", 0);
  turbo_json_object_add(choice, "message", message);
  turbo_json_array_add(choices, choice);
  turbo_json_object_add(response, "choices", choices);

  *out_choice = choice;
  *out_message = message;
  return response;
}

static json_value_t *turbo_openai_anthropic_stream_response_shell_create(
    const turbo_openai_anthropic_stream_state_t *state, json_value_t **out_content) {
  json_value_t *response;
  json_value_t *content;

  if (!state || !state->id || !out_content) {
    return NULL;
  }

  response = turbo_json_create_object();
  content = turbo_json_create_array();
  if (!response || !content) {
    turbo_free_json(&response);
    turbo_free_json(&content);
    return NULL;
  }

  turbo_json_object_set_string(response, "id", state->id);
  turbo_json_object_set_string(response, "role",
                               state->role && state->role[0] != '\0' ? state->role
                                                                      : "assistant");
  turbo_json_object_add(response, "content", content);
  *out_content = content;
  return response;
}

static json_value_t *turbo_openai_state_get_or_create_array(json_value_t *state, const char *key) {
  json_value_t *array;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  array = turbo_json_object_get(state, key);
  if (!array) {
    array = turbo_json_create_array();
    if (!array) {
      return NULL;
    }
    turbo_json_object_add(state, key, array);
  }

  return turbo_json_type(array) == TURBO_JSON_ARRAY ? array : NULL;
}

static char *turbo_openai_agent_memory_context_text_internal(const json_value_t *state) {
  const json_value_t *layers;
  size_t i;
  size_t total = 0;
  int found = 0;
  char *buffer;
  size_t offset = 0;
  static const char *prefix = "Persistent memory:\n";

  layers = turbo_openai_agent_state_memory_layers(state);
  if (!layers || turbo_json_type(layers) != TURBO_JSON_ARRAY || turbo_json_array_size(layers) == 0) {
    return NULL;
  }

  total += strlen(prefix);
  for (i = 0; i < turbo_json_array_size(layers); ++i) {
    const json_value_t *layer = turbo_json_array_get(layers, i);
    const char *scope;
    const char *path;
    const char *text;

    if (!layer || turbo_json_type(layer) != TURBO_JSON_OBJECT) {
      continue;
    }

    scope = turbo_json_get_string(layer, "scope");
    path = turbo_json_get_string(layer, "path");
    text = turbo_json_get_string(layer, "text");
    if (!scope || !text || text[0] == '\0') {
      continue;
    }

    total += 2 + strlen(scope) + 2;
    if (path && path[0] != '\0') {
      total += 1 + strlen(path);
    }
    total += 1 + strlen(text) + 2;
    found = 1;
  }

  if (!found) {
    return NULL;
  }

  buffer = (char *)malloc(total + 1);
  if (!buffer) {
    return NULL;
  }

  memcpy(buffer + offset, prefix, strlen(prefix));
  offset += strlen(prefix);
  for (i = 0; i < turbo_json_array_size(layers); ++i) {
    const json_value_t *layer = turbo_json_array_get(layers, i);
    const char *scope;
    const char *path;
    const char *text;
    int written;

    if (!layer || turbo_json_type(layer) != TURBO_JSON_OBJECT) {
      continue;
    }

    scope = turbo_json_get_string(layer, "scope");
    path = turbo_json_get_string(layer, "path");
    text = turbo_json_get_string(layer, "text");
    if (!scope || !text || text[0] == '\0') {
      continue;
    }

    if (path && path[0] != '\0') {
      written = snprintf(buffer + offset, total + 1 - offset, "[%s] %s\n%s\n\n", scope, path, text);
    } else {
      written = snprintf(buffer + offset, total + 1 - offset, "[%s]\n%s\n\n", scope, text);
    }
    if (written < 0) {
      free(buffer);
      return NULL;
    }
    offset += (size_t)written;
  }

  if (offset >= 2 && buffer[offset - 1] == '\n' && buffer[offset - 2] == '\n') {
    offset -= 1;
  }
  buffer[offset] = '\0';
  return buffer;
}

static char *turbo_openai_agent_build_effective_instructions(const turbo_openai_agent_t *agent,
                                                             const json_value_t *state) {
  const char *base = agent ? agent->instructions : NULL;
  char *memory_text = turbo_openai_agent_memory_context_text_internal(state);
  size_t base_len = base ? strlen(base) : 0;
  size_t memory_len = memory_text ? strlen(memory_text) : 0;
  char *buffer;

  if (base_len == 0 && memory_len == 0) {
    free(memory_text);
    return NULL;
  }
  if (base_len == 0) {
    return memory_text;
  }
  if (memory_len == 0) {
    return turbo_openai_strdup(base);
  }

  buffer = (char *)malloc(base_len + 2 + memory_len + 1);
  if (!buffer) {
    free(memory_text);
    return NULL;
  }

  memcpy(buffer, base, base_len);
  memcpy(buffer + base_len, "\n\n", 2);
  memcpy(buffer + base_len + 2, memory_text, memory_len + 1);
  free(memory_text);
  return buffer;
}

static int turbo_openai_replay_message_events(
    const json_value_t *events, json_value_t *messages,
    turbo_openai_message_event_append_fn append_model_event,
    turbo_openai_message_event_append_fn append_tool_results_event) {
  size_t i;

  if (!messages) {
    return -1;
  }

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY) {
    return 0;
  }

  for (i = 0; i < turbo_json_array_size(events); ++i) {
    const json_value_t *event = turbo_json_array_get(events, i);
    if (turbo_openai_event_kind_is(event, "model")) {
      if (!append_model_event || append_model_event(messages, event) != 0) {
        return -1;
      }
    } else if (turbo_openai_event_kind_is(event, "tool_results")) {
      if (!append_tool_results_event || append_tool_results_event(messages, event) != 0) {
        return -1;
      }
    }
  }

  return 0;
}

static const json_value_t *turbo_openai_responses_request_input_source(const json_value_t *state,
                                                                       json_value_t *request) {
  const json_value_t *last_event;
  const json_value_t *last_model_event;
  const json_value_t *input_source = NULL;

  if (!state || !request) {
    return NULL;
  }

  last_event = turbo_openai_state_last_event(state);
  last_model_event = turbo_openai_state_last_event_of_kind(state, "model");

  if (last_event && turbo_json_type(last_event) == TURBO_JSON_OBJECT) {
    input_source = (json_value_t *)turbo_openai_tool_results_event_outputs(last_event);
    if (input_source) {
      const char *response_id = turbo_openai_model_event_response_id(last_model_event);
      if (response_id) {
        turbo_json_object_set_string(request, "previous_response_id", response_id);
      }
    }
  }

  if (!input_source) {
    input_source = turbo_openai_state_get_array_const(state, "input");
  }

  return input_source;
}

static json_value_t *turbo_openai_build_responses_input_messages(const json_value_t *input_source) {
  turbo_runtime_data_bind_value_t *messages_bind;
  json_value_t *wire_messages;

  if (!input_source) {
    return NULL;
  }

  messages_bind = turbo_runtime_data_bind_value_from_json(input_source);
  if (!messages_bind) {
    return NULL;
  }

  wire_messages = turbo_model_provider_messages_to_wire_json(
      turbo_model_provider_openai_responses(), messages_bind, NULL);
  turbo_runtime_data_bind_value_destroy(messages_bind);
  return wire_messages;
}

static int turbo_openai_plan_normalize_steps(const json_value_t *input_steps,
                                             json_value_t **out_steps) {
  json_value_t *steps = NULL;
  size_t i;

  if (!input_steps || !out_steps || turbo_json_type(input_steps) != TURBO_JSON_ARRAY) {
    return -1;
  }

  *out_steps = NULL;
  steps = turbo_json_create_array();
  if (!steps) {
    return -1;
  }

  for (i = 0; i < turbo_json_array_size(input_steps); ++i) {
    const json_value_t *step = turbo_json_array_get(input_steps, i);
    json_value_t *normalized_step = turbo_json_create_object();
    const char *text = NULL;
    char *serialized_text = NULL;

    if (!normalized_step) {
      turbo_free_json(&steps);
      return -1;
    }

    if (step && turbo_json_type(step) == TURBO_JSON_STRING) {
      text = turbo_json_string(step);
    } else if (step && turbo_json_type(step) == TURBO_JSON_OBJECT) {
      text = turbo_json_get_string(step, "text");
      if (!text) {
        text = turbo_json_get_string(step, "title");
      }
      if (!text) {
        text = turbo_json_get_string(step, "step");
      }
      if (!text) {
        serialized_text = turbo_json_serialize(step, NULL);
        text = serialized_text;
      }
    }

    if (!text || text[0] == '\0') {
      free(serialized_text);
      turbo_free_json(&normalized_step);
      turbo_free_json(&steps);
      return -1;
    }

    turbo_json_object_set_string(normalized_step, "text", text);
    turbo_json_object_set_string(normalized_step, "status", "pending");
    if (step && turbo_json_type(step) == TURBO_JSON_OBJECT) {
      json_value_t *step_data = NULL;
      if (turbo_openai_clone_json(step, &step_data) != TURBO_GRAPH_EXEC_OK) {
        free(serialized_text);
        turbo_free_json(&normalized_step);
        turbo_free_json(&steps);
        return -1;
      }
      turbo_json_object_add(normalized_step, "data", step_data);
    }
    turbo_json_array_add(steps, normalized_step);
    free(serialized_text);
  }

  *out_steps = steps;
  return 0;
}

static int turbo_openai_state_set_plan_object(json_value_t *state, json_value_t *plan_object) {
  json_value_t *versions;
  json_value_t *progress_versions;
  json_value_t *progress;

  if (!state || !plan_object || turbo_json_type(state) != TURBO_JSON_OBJECT ||
      turbo_json_type(plan_object) != TURBO_JSON_OBJECT) {
    return -1;
  }

  versions = turbo_json_object_get(state, "plan_versions");
  if (!versions) {
    versions = turbo_json_create_array();
    if (!versions) {
      return -1;
    }
    turbo_json_object_add(state, "plan_versions", versions);
  }

  if (turbo_json_type(versions) != TURBO_JSON_ARRAY) {
    return -1;
  }

  progress_versions = turbo_json_object_get(state, "plan_progress_versions");
  if (!progress_versions) {
    progress_versions = turbo_json_create_array();
    if (!progress_versions) {
      return -1;
    }
    turbo_json_object_add(state, "plan_progress_versions", progress_versions);
  }

  if (turbo_json_type(progress_versions) != TURBO_JSON_ARRAY) {
    return -1;
  }

  progress = turbo_json_create_array();
  if (!progress) {
    return -1;
  }

  turbo_json_array_add(versions, plan_object);
  turbo_json_array_add(progress_versions, progress);
  return 0;
}

static json_value_t *turbo_openai_state_get_versions_array(json_value_t *state, const char *key) {
  json_value_t *versions;

  if (!state || !key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  versions = turbo_json_object_get(state, key);
  if (!versions) {
    versions = turbo_json_create_array();
    if (!versions) {
      return NULL;
    }
    turbo_json_object_add(state, key, versions);
  }

  return turbo_json_type(versions) == TURBO_JSON_ARRAY ? versions : NULL;
}

static json_value_t *turbo_openai_state_get_current_array_version(json_value_t *state,
                                                                  const char *key) {
  json_value_t *versions;

  versions = turbo_openai_state_get_versions_array(state, key);
  return turbo_openai_array_last(versions);
}

static json_value_t *turbo_openai_state_get_current_object_version(json_value_t *state,
                                                                   const char *key) {
  json_value_t *versions;

  versions = turbo_openai_state_get_versions_array(state, key);
  return turbo_openai_array_last(versions);
}

static const json_value_t *turbo_openai_state_get_current_array_version_const(
    const json_value_t *state, const char *key) {
  const json_value_t *versions;

  if (!state || !key) {
    return NULL;
  }

  versions = turbo_json_object_get(state, key);
  return turbo_openai_array_last_const(versions);
}

static int turbo_openai_state_append_array_version(json_value_t *state, const char *key,
                                                   json_value_t *value) {
  json_value_t *versions;

  if (!state || !key || !value || turbo_json_type(value) != TURBO_JSON_ARRAY) {
    return -1;
  }

  versions = turbo_openai_state_get_versions_array(state, key);
  if (!versions) {
    return -1;
  }

  turbo_json_array_add(versions, value);
  return 0;
}

static const json_value_t *turbo_openai_state_get_current_object_version_const(
    const json_value_t *state, const char *key) {
  const json_value_t *versions;

  if (!state || !key) {
    return NULL;
  }

  versions = turbo_json_object_get(state, key);
  return turbo_openai_array_last_const(versions);
}

static int turbo_openai_state_append_object_version(json_value_t *state, const char *key,
                                                    json_value_t *value) {
  json_value_t *versions;

  if (!state || !key || !value || turbo_json_type(value) != TURBO_JSON_OBJECT) {
    return -1;
  }

  versions = turbo_openai_state_get_versions_array(state, key);
  if (!versions) {
    return -1;
  }

  turbo_json_array_add(versions, value);
  return 0;
}

static const json_value_t *turbo_openai_agent_state_plan_steps_const(const json_value_t *state) {
  const json_value_t *plan = turbo_openai_agent_state_plan(state);
  return plan && turbo_json_type(plan) == TURBO_JSON_OBJECT ? turbo_json_object_get(plan, "steps")
                                                            : NULL;
}

static int turbo_openai_state_append_replan_version(json_value_t *state, int requested,
                                                    size_t count, size_t max_count,
                                                    const char *reason) {
  json_value_t *replan_object;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  replan_object = turbo_json_create_object();
  if (!replan_object) {
    return -1;
  }

  turbo_json_object_set_bool(replan_object, "requested", requested ? true : false);
  turbo_json_object_set_number(replan_object, "count", (double)count);
  turbo_json_object_set_number(replan_object, "max_count", (double)max_count);
  turbo_json_object_set_string(replan_object, "reason", reason && reason[0] != '\0' ? reason : "");
  return turbo_openai_state_append_object_version(state, "replan_versions", replan_object);
}

static int turbo_openai_state_append_review_version(json_value_t *state, int required,
                                                    int approved, const char *note) {
  json_value_t *review_object;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  review_object = turbo_json_create_object();
  if (!review_object) {
    return -1;
  }

  turbo_json_object_set_bool(review_object, "required", required ? true : false);
  turbo_json_object_set_bool(review_object, "approved", approved ? true : false);
  turbo_json_object_set_string(review_object, "note", note && note[0] != '\0' ? note : "");
  return turbo_openai_state_append_object_version(state, "review_versions", review_object);
}

static int turbo_openai_state_append_single_string_object_version(json_value_t *state,
                                                                  const char *version_key,
                                                                  const char *field_key,
                                                                  const char *field_value) {
  json_value_t *object;

  if (!state || !version_key || !field_key || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  object = turbo_json_create_object();
  if (!object) {
    return -1;
  }

  turbo_json_object_set_string(object, field_key, field_value ? field_value : "");
  return turbo_openai_state_append_object_version(state, version_key, object);
}

static int turbo_openai_state_append_two_string_object_version(
    json_value_t *state, const char *version_key, const char *field1_key,
    const char *field1_value, const char *field2_key, const char *field2_value) {
  json_value_t *object;

  if (!state || !version_key || !field1_key || !field2_key ||
      turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  object = turbo_json_create_object();
  if (!object) {
    return -1;
  }

  turbo_json_object_set_string(object, field1_key, field1_value ? field1_value : "");
  turbo_json_object_set_string(object, field2_key, field2_value ? field2_value : "");
  return turbo_openai_state_append_object_version(state, version_key, object);
}

static const char *turbo_openai_state_current_version_string_field(const json_value_t *state,
                                                                   const char *version_key,
                                                                   const char *field_key) {
  const json_value_t *object =
      turbo_openai_state_get_current_object_version_const(state, version_key);
  return object ? turbo_json_get_string(object, field_key) : NULL;
}

static int turbo_openai_state_current_version_bool_field(const json_value_t *state,
                                                         const char *version_key,
                                                         const char *field_key,
                                                         int default_value) {
  const json_value_t *object =
      turbo_openai_state_get_current_object_version_const(state, version_key);
  return object ? (turbo_json_get_bool(object, field_key, default_value ? true : false) ? 1 : 0)
                : default_value;
}

static size_t turbo_openai_state_current_version_size_field(const json_value_t *state,
                                                            const char *version_key,
                                                            const char *field_key) {
  const json_value_t *object =
      turbo_openai_state_get_current_object_version_const(state, version_key);
  return object ? (size_t)turbo_json_get_double(object, field_key, 0.0) : 0;
}

static const json_value_t *turbo_openai_completed_steps_const(const json_value_t *state) {
  const json_value_t *steps;

  if (!state) {
    return NULL;
  }

  steps = turbo_json_object_get(state, "completed_steps");
  return steps && turbo_json_type(steps) == TURBO_JSON_ARRAY ? steps : NULL;
}

static int turbo_openai_append_completed_step(json_value_t *state, size_t step_index,
                                              const char *step_text, const char *output_text) {
  json_value_t *completed_steps;
  json_value_t *entry;

  if (!state || !step_text || !output_text) {
    return -1;
  }

  completed_steps = turbo_openai_state_get_or_create_array(state, "completed_steps");
  if (!completed_steps) {
    return -1;
  }

  entry = turbo_json_create_object();
  if (!entry) {
    return -1;
  }

  turbo_json_object_set_number(entry, "step_index", (double)step_index);
  turbo_json_object_set_string(entry, "step", step_text);
  turbo_json_object_set_string(entry, "output", output_text);
  turbo_json_array_add(completed_steps, entry);
  return 0;
}

static char *turbo_openai_build_completed_steps_message(const json_value_t *state) {
  const json_value_t *completed_steps;
  char *buffer = NULL;
  size_t length = 0;
  size_t i;

  completed_steps = turbo_openai_completed_steps_const(state);
  if (!completed_steps || turbo_json_array_size(completed_steps) == 0) {
    return NULL;
  }

  if (turbo_openai_append_text(&buffer, &length,
                               "Completed step outputs so far:\n") != 0) {
    free(buffer);
    return NULL;
  }

  for (i = 0; i < turbo_json_array_size(completed_steps); ++i) {
    const json_value_t *entry = turbo_json_array_get(completed_steps, i);
    const char *step_text;
    const char *output_text;
    char *line;
    int needed;

    if (!entry || turbo_json_type(entry) != TURBO_JSON_OBJECT) {
      continue;
    }

    step_text = turbo_json_get_string(entry, "step");
    output_text = turbo_json_get_string(entry, "output");
    needed = snprintf(NULL, 0, "%lu. %s\nResult: %s\n",
                      (unsigned long)(i + 1), step_text ? step_text : "",
                      output_text ? output_text : "");
    if (needed < 0) {
      free(buffer);
      return NULL;
    }

    line = (char *)malloc((size_t)needed + 1);
    if (!line) {
      free(buffer);
      return NULL;
    }

    snprintf(line, (size_t)needed + 1, "%lu. %s\nResult: %s\n",
             (unsigned long)(i + 1), step_text ? step_text : "",
             output_text ? output_text : "");
    if (turbo_openai_append_text(&buffer, &length, line) != 0) {
      free(line);
      free(buffer);
      return NULL;
    }
    free(line);
  }

  return buffer;
}

typedef struct {
  char *data;
  size_t length;
} turbo_openai_stream_buffer_t;

static int turbo_openai_append_bytes(char **buffer, size_t *length, const char *data,
                                     size_t data_len) {
  char *next;

  if (!buffer || !length || (!data && data_len > 0)) {
    return -1;
  }

  next = (char *)realloc(*buffer, *length + data_len + 1);
  if (!next) {
    return -1;
  }

  if (data_len > 0) {
    memcpy(next + *length, data, data_len);
  }
  *length += data_len;
  next[*length] = '\0';
  *buffer = next;
  return 0;
}

static int turbo_openai_append_text(char **buffer, size_t *length, const char *text) {
  if (!buffer || !length || !text) {
    return -1;
  }

  return turbo_openai_append_bytes(buffer, length, text, strlen(text));
}

static int turbo_openai_replace_text(char **target, const char *text) {
  char *copy;

  if (!target || !text) {
    return -1;
  }

  copy = turbo_openai_strdup(text);
  if (!copy) {
    return -1;
  }

  free(*target);
  *target = copy;
  return 0;
}

static int turbo_openai_set_if_nonempty(char **target, const char *text) {
  if (!text || text[0] == '\0') {
    return 0;
  }

  return turbo_openai_replace_text(target, text);
}

static int turbo_openai_append_dynamic_text(char **buffer, const char *text) {
  size_t length = 0;

  if (!buffer || !text) {
    return -1;
  }

  if (*buffer) {
    length = strlen(*buffer);
  }

  return turbo_openai_append_text(buffer, &length, text);
}

static void turbo_openai_free_stream_tool_calls(turbo_openai_stream_tool_call_t *tool_calls,
                                                size_t tool_call_count) {
  size_t i;

  for (i = 0; i < tool_call_count; ++i) {
    free(tool_calls[i].id);
    free(tool_calls[i].type);
    free(tool_calls[i].name);
    free(tool_calls[i].arguments);
  }
  free(tool_calls);
}

static int turbo_openai_ensure_stream_tool_call_capacity(
    turbo_openai_stream_tool_call_t **tool_calls, size_t *tool_call_count, size_t index) {
  turbo_openai_stream_tool_call_t *next_tool_calls;
  size_t new_count;
  size_t i;

  if (!tool_calls || !tool_call_count) {
    return -1;
  }

  if (index < *tool_call_count) {
    return 0;
  }

  new_count = index + 1;
  next_tool_calls = (turbo_openai_stream_tool_call_t *)realloc(
      *tool_calls, new_count * sizeof(*next_tool_calls));
  if (!next_tool_calls) {
    return -1;
  }

  for (i = *tool_call_count; i < new_count; ++i) {
    memset(&next_tool_calls[i], 0, sizeof(next_tool_calls[i]));
  }

  *tool_calls = next_tool_calls;
  *tool_call_count = new_count;
  return 0;
}

static turbo_openai_stream_tool_call_t *turbo_openai_stream_tool_call_slot(
    turbo_openai_stream_tool_call_t **tool_calls, size_t *tool_call_count, size_t index) {
  if (turbo_openai_ensure_stream_tool_call_capacity(tool_calls, tool_call_count, index) != 0) {
    return NULL;
  }

  return &(*tool_calls)[index];
}

static void turbo_openai_free_chat_stream_state(turbo_openai_chat_stream_state_t *state) {
  if (!state) {
    return;
  }

  free(state->id);
  free(state->role);
  free(state->content);
  free(state->finish_reason);
  turbo_openai_free_stream_tool_calls(state->tool_calls, state->tool_call_count);
  memset(state, 0, sizeof(*state));
}

static void
turbo_openai_free_anthropic_stream_state(turbo_openai_anthropic_stream_state_t *state) {
  if (!state) {
    return;
  }

  free(state->id);
  free(state->role);
  free(state->content);
  free(state->stop_reason);
  turbo_openai_free_stream_tool_calls(state->tool_calls, state->tool_call_count);
  memset(state, 0, sizeof(*state));
}

static void turbo_openai_collect_stream_chunk(const char *data, size_t len, void *user_data) {
  turbo_openai_stream_buffer_t *buffer = (turbo_openai_stream_buffer_t *)user_data;

  if (!buffer || (!data && len > 0)) {
    return;
  }

  if (buffer->length == (size_t)-1) {
    return;
  }

  if (turbo_openai_append_bytes(&buffer->data, &buffer->length, data, len) != 0) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = (size_t)-1;
  }
}

static char *turbo_openai_normalize_sse_newlines(const char *data, size_t len) {
  char *normalized;
  size_t i;
  size_t out = 0;

  if (!data && len > 0) {
    return NULL;
  }

  normalized = (char *)malloc(len + 1);
  if (!normalized) {
    return NULL;
  }

  for (i = 0; i < len; ++i) {
    if (data[i] != '\r') {
      normalized[out++] = data[i];
    }
  }

  normalized[out] = '\0';
  return normalized;
}

static int turbo_openai_collect_sse_event_data(const char *begin, const char *end,
                                               char **out_data) {
  const char *cursor;
  char *data = NULL;
  size_t length = 0;

  if (!begin || !end || !out_data || end < begin) {
    return -1;
  }

  *out_data = NULL;
  cursor = begin;
  while (cursor < end) {
    const char *line_end = cursor;
    const char *value;

    while (line_end < end && *line_end != '\n') {
      ++line_end;
    }

    if ((line_end - cursor) >= 5 && strncmp(cursor, "data:", 5) == 0) {
      value = cursor + 5;
      if (value < line_end && *value == ' ') {
        ++value;
      }

      if (length > 0 && turbo_openai_append_bytes(&data, &length, "\n", 1) != 0) {
        free(data);
        return -1;
      }

      if (turbo_openai_append_bytes(&data, &length, value, (size_t)(line_end - value)) != 0) {
        free(data);
        return -1;
      }
    }

    cursor = (line_end < end) ? line_end + 1 : end;
  }

  if (!data) {
    data = turbo_openai_strdup("");
    if (!data) {
      return -1;
    }
  }

  *out_data = data;
  return 0;
}

static int turbo_openai_chat_stream_apply_chunk(turbo_openai_chat_stream_state_t *state,
                                                const json_value_t *chunk) {
  const char *id;
  const char *model;
  const json_value_t *choices;
  const json_value_t *choice;
  const json_value_t *delta;
  const json_value_t *tool_calls;
  const char *role;
  const char *content;
  const char *finish_reason;
  size_t i;

  (void)model;
  if (!state || !chunk || turbo_json_type(chunk) != TURBO_JSON_OBJECT) {
    return -1;
  }

  id = turbo_json_get_string(chunk, "id");
  if (turbo_openai_set_if_nonempty(&state->id, id) != 0) {
    return -1;
  }

  model = turbo_json_get_string(chunk, "model");
  choices = turbo_json_object_get(chunk, "choices");
  choice = choices && turbo_json_type(choices) == TURBO_JSON_ARRAY &&
                   turbo_json_array_size(choices) > 0
               ? turbo_json_array_get(choices, 0)
               : NULL;
  if (!choice || turbo_json_type(choice) != TURBO_JSON_OBJECT) {
    return -1;
  }

  delta = turbo_json_object_get(choice, "delta");
  if (delta && turbo_json_type(delta) == TURBO_JSON_OBJECT) {
    role = turbo_json_get_string(delta, "role");
    if (turbo_openai_set_if_nonempty(&state->role, role) != 0) {
      return -1;
    }

    content = turbo_json_get_string(delta, "content");
    if (content && turbo_openai_append_dynamic_text(&state->content, content) != 0) {
      return -1;
    }

    tool_calls = turbo_json_object_get(delta, "tool_calls");
    if (tool_calls && turbo_json_type(tool_calls) == TURBO_JSON_ARRAY) {
      for (i = 0; i < turbo_json_array_size(tool_calls); ++i) {
        const json_value_t *tool_call = turbo_json_array_get(tool_calls, i);
        const json_value_t *function_object;
        int index;
        const char *tool_id;
        const char *tool_type;
        const char *name_piece;
        const char *arguments_piece;
        turbo_openai_stream_tool_call_t *stream_tool_call;

        if (!tool_call || turbo_json_type(tool_call) != TURBO_JSON_OBJECT) {
          continue;
        }

        index = turbo_json_get_int(tool_call, "index", (int)i);
        if (index < 0) {
          return -1;
        }

        stream_tool_call = turbo_openai_stream_tool_call_slot(&state->tool_calls,
                                                              &state->tool_call_count,
                                                              (size_t)index);
        if (!stream_tool_call) {
          return -1;
        }
        tool_id = turbo_json_get_string(tool_call, "id");
        if (turbo_openai_set_if_nonempty(&stream_tool_call->id, tool_id) != 0) {
          return -1;
        }

        tool_type = turbo_json_get_string(tool_call, "type");
        if (turbo_openai_set_if_nonempty(&stream_tool_call->type, tool_type) != 0) {
          return -1;
        }

        function_object = turbo_json_object_get(tool_call, "function");
        if (!function_object || turbo_json_type(function_object) != TURBO_JSON_OBJECT) {
          continue;
        }

        name_piece = turbo_json_get_string(function_object, "name");
        if (name_piece && name_piece[0] != '\0' &&
            turbo_openai_append_dynamic_text(&stream_tool_call->name, name_piece) != 0) {
          return -1;
        }

        arguments_piece = turbo_json_get_string(function_object, "arguments");
        if (arguments_piece &&
            turbo_openai_append_dynamic_text(&stream_tool_call->arguments, arguments_piece) != 0) {
          return -1;
        }
      }
    }
  }

  finish_reason = turbo_json_get_string(choice, "finish_reason");
  if (turbo_openai_set_if_nonempty(&state->finish_reason, finish_reason) != 0) {
    return -1;
  }

  return 0;
}

static char *turbo_openai_build_chat_stream_response_json(
    const turbo_openai_chat_stream_state_t *state) {
  json_value_t *response;
  json_value_t *choice;
  json_value_t *message;
  json_value_t *tool_calls = NULL;

  if (!state || !state->id) {
    return NULL;
  }

  response = turbo_openai_chat_stream_response_shell_create(state, &choice, &message);
  if (!response) {
    return NULL;
  }

  tool_calls = turbo_openai_build_chat_stream_tool_calls_array(state);
  if (state->tool_call_count > 0 && !tool_calls) {
    turbo_free_json(&response);
    return NULL;
  }
  if (tool_calls) {
    turbo_json_object_add(message, "tool_calls", tool_calls);
  }

  turbo_json_object_set_string(choice, "finish_reason",
                               state->finish_reason ? state->finish_reason
                                                    : (state->tool_call_count > 0 ? "tool_calls"
                                                                                  : "stop"));
  return turbo_openai_serialize_json_and_free(response);
}

int turbo_openai_chat_sse_to_json(const char *sse_data, size_t sse_len,
                                  char **out_response_json) {
  char *normalized;
  char *cursor;
  turbo_openai_chat_stream_state_t state = {0};
  int saw_chunk = 0;

  if (!sse_data || !out_response_json) {
    return -1;
  }

  *out_response_json = NULL;
  normalized = turbo_openai_normalize_sse_newlines(sse_data, sse_len);
  if (!normalized) {
    return -1;
  }

  cursor = normalized;
  while (*cursor != '\0') {
    char *event_end = strstr(cursor, "\n\n");
    char *data = NULL;
    json_value_t *chunk = NULL;
    int is_done = 0;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }

    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_openai_collect_sse_event_data(cursor, event_end, &data) != 0) {
      free(normalized);
      turbo_openai_free_chat_stream_state(&state);
      return -1;
    }

    if (strcmp(data, "[DONE]") == 0) {
      is_done = 1;
    } else if (data[0] != '\0') {
      if (turbo_parse_json((const uint8_t *)data, strlen(data), &chunk) != 0 ||
          turbo_openai_chat_stream_apply_chunk(&state, chunk) != 0) {
        free(data);
        turbo_free_json(&chunk);
        free(normalized);
        turbo_openai_free_chat_stream_state(&state);
        return -1;
      }
      saw_chunk = 1;
    }

    free(data);
    turbo_free_json(&chunk);

    if (is_done) {
      break;
    }

    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  free(normalized);
  if (!saw_chunk) {
    turbo_openai_free_chat_stream_state(&state);
    return -1;
  }

  *out_response_json = turbo_openai_build_chat_stream_response_json(&state);
  turbo_openai_free_chat_stream_state(&state);
  return *out_response_json ? 0 : -1;
}

int turbo_openai_responses_sse_to_json(const char *sse_data, size_t sse_len,
                                       char **out_response_json) {
  char *normalized;
  char *cursor;
  json_value_t *completed_response = NULL;
  json_value_t *output_items = NULL;

  if (!sse_data || !out_response_json) {
    return -1;
  }

  *out_response_json = NULL;
  normalized = turbo_openai_normalize_sse_newlines(sse_data, sse_len);
  if (!normalized) {
    return -1;
  }

  output_items = turbo_json_create_array();
  if (!output_items) {
    free(normalized);
    return -1;
  }

  cursor = normalized;
  while (*cursor != '\0') {
    char *event_end = strstr(cursor, "\n\n");
    char *data = NULL;
    json_value_t *event = NULL;
    const char *type;
    json_value_t *response;
    const json_value_t *item;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }

    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_openai_collect_sse_event_data(cursor, event_end, &data) != 0) {
      free(normalized);
      return -1;
    }

    if (strcmp(data, "[DONE]") == 0) {
      free(data);
      break;
    }

    if (data[0] != '\0' &&
        turbo_parse_json((const uint8_t *)data, strlen(data), &event) == 0) {
      type = turbo_json_get_string(event, "type");
      response = turbo_json_object_get(event, "response");
      item = turbo_json_object_get(event, "item");
      if (type && item && turbo_json_type(item) == TURBO_JSON_OBJECT &&
          strcmp(type, "response.output_item.done") == 0) {
        if (turbo_openai_array_add_clone(output_items, item) != 0) {
          turbo_free_json(&event);
          free(data);
          free(normalized);
          turbo_free_json(&output_items);
          turbo_free_json(&completed_response);
          return -1;
        }
      } else if (type && response && turbo_json_type(response) == TURBO_JSON_OBJECT &&
                 strcmp(type, "response.completed") == 0) {
        turbo_free_json(&completed_response);
        if (turbo_openai_clone_json(response, &completed_response) != TURBO_GRAPH_EXEC_OK) {
          turbo_free_json(&event);
          free(data);
          free(normalized);
          turbo_free_json(&output_items);
          return -1;
        }
      }
    }

    turbo_free_json(&event);
    free(data);
    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  free(normalized);
  if (completed_response && turbo_json_type(completed_response) == TURBO_JSON_OBJECT) {
    const json_value_t *completed_output = turbo_json_object_get(completed_response, "output");
    if (output_items && turbo_json_array_size(output_items) > 0 &&
        (!completed_output || turbo_json_type(completed_output) != TURBO_JSON_ARRAY ||
         turbo_json_array_size(completed_output) == 0)) {
      json_value_t *response = turbo_json_create_object();
      json_value_t *rebuilt_output = turbo_json_create_array();
      const char *response_id = turbo_json_get_string(completed_response, "id");
      size_t i;

      if (!response || !rebuilt_output) {
        turbo_free_json(&completed_response);
        turbo_free_json(&response);
        turbo_free_json(&rebuilt_output);
        turbo_free_json(&output_items);
        return -1;
      }

      if (response_id) {
        turbo_json_object_set_string(response, "id", response_id);
      }

      for (i = 0; i < turbo_json_array_size(output_items); ++i) {
        const json_value_t *output_item = turbo_json_array_get(output_items, i);
        if (turbo_openai_array_add_clone(rebuilt_output, output_item) != 0) {
          turbo_free_json(&response);
          turbo_free_json(&rebuilt_output);
          turbo_free_json(&completed_response);
          turbo_free_json(&output_items);
          return -1;
        }
      }

      turbo_json_object_add(response, "output", rebuilt_output);
      *out_response_json = turbo_json_serialize(response, NULL);
      turbo_free_json(&response);
      turbo_free_json(&completed_response);
      turbo_free_json(&output_items);
      return *out_response_json ? 0 : -1;
    }

    *out_response_json = turbo_json_serialize(completed_response, NULL);
    turbo_free_json(&completed_response);
    turbo_free_json(&output_items);
    return *out_response_json ? 0 : -1;
  }

  turbo_free_json(&output_items);
  return -1;
}

static int turbo_openai_anthropic_stream_apply_event(
    turbo_openai_anthropic_stream_state_t *state, const json_value_t *event) {
  const char *type;

  if (!state || !event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return -1;
  }

  type = turbo_json_get_string(event, "type");
  if (!type) {
    return -1;
  }

  if (strcmp(type, "message_start") == 0) {
    const json_value_t *message = turbo_json_object_get(event, "message");
    if (!message || turbo_json_type(message) != TURBO_JSON_OBJECT) {
      return -1;
    }
    if (turbo_openai_set_if_nonempty(&state->id, turbo_json_get_string(message, "id")) != 0 ||
        turbo_openai_set_if_nonempty(&state->role, turbo_json_get_string(message, "role")) != 0) {
      return -1;
    }
  } else if (strcmp(type, "content_block_start") == 0) {
    const json_value_t *content_block = turbo_json_object_get(event, "content_block");
    const char *block_type;
    int index;

    if (!content_block || turbo_json_type(content_block) != TURBO_JSON_OBJECT) {
      return -1;
    }

    block_type = turbo_json_get_string(content_block, "type");
    index = turbo_json_get_int(event, "index", 0);
    if (index < 0) {
      return -1;
    }

    if (block_type && strcmp(block_type, "text") == 0) {
      const char *text = turbo_json_get_string(content_block, "text");
      if (text && turbo_openai_append_dynamic_text(&state->content, text) != 0) {
        return -1;
      }
    } else if (block_type && strcmp(block_type, "tool_use") == 0) {
      turbo_openai_stream_tool_call_t *tool_call;
      json_value_t *input = turbo_json_object_get(content_block, "input");
      char *serialized_input = NULL;

      tool_call = turbo_openai_stream_tool_call_slot(&state->tool_calls, &state->tool_call_count,
                                                     (size_t)index);
      if (!tool_call) {
        return -1;
      }

      if (turbo_openai_set_if_nonempty(&tool_call->id,
                                       turbo_json_get_string(content_block, "id")) != 0 ||
          turbo_openai_set_if_nonempty(&tool_call->type, "tool_use") != 0 ||
          turbo_openai_set_if_nonempty(&tool_call->name,
                                       turbo_json_get_string(content_block, "name")) != 0) {
        return -1;
      }

      if (input && turbo_json_type(input) == TURBO_JSON_OBJECT &&
          turbo_json_object_size(input) > 0) {
        serialized_input = turbo_json_serialize(input, NULL);
      } else if (input && turbo_json_type(input) != TURBO_JSON_OBJECT) {
        serialized_input = turbo_openai_strdup("{}");
      }

      if (serialized_input) {
        if (turbo_openai_replace_text(&tool_call->arguments, serialized_input) != 0) {
          free(serialized_input);
          return -1;
        }
        free(serialized_input);
      }
    }
  } else if (strcmp(type, "content_block_delta") == 0) {
    const json_value_t *delta = turbo_json_object_get(event, "delta");
    const char *delta_type;
    int index;

    if (!delta || turbo_json_type(delta) != TURBO_JSON_OBJECT) {
      return -1;
    }

    delta_type = turbo_json_get_string(delta, "type");
    index = turbo_json_get_int(event, "index", 0);
    if (index < 0) {
      return -1;
    }

    if (delta_type && strcmp(delta_type, "text_delta") == 0) {
      const char *text = turbo_json_get_string(delta, "text");
      if (text && turbo_openai_append_dynamic_text(&state->content, text) != 0) {
        return -1;
      }
    } else if (delta_type && strcmp(delta_type, "input_json_delta") == 0) {
      const char *partial_json = turbo_json_get_string(delta, "partial_json");
      turbo_openai_stream_tool_call_t *tool_call;

      tool_call = turbo_openai_stream_tool_call_slot(&state->tool_calls, &state->tool_call_count,
                                                     (size_t)index);
      if (!tool_call) {
        return -1;
      }

      if (turbo_openai_set_if_nonempty(&tool_call->type, "tool_use") != 0) {
        return -1;
      }

      if (partial_json &&
          turbo_openai_append_dynamic_text(&tool_call->arguments, partial_json) != 0) {
        return -1;
      }
    }
  } else if (strcmp(type, "message_delta") == 0) {
    const json_value_t *delta = turbo_json_object_get(event, "delta");
    if (!delta || turbo_json_type(delta) != TURBO_JSON_OBJECT) {
      return -1;
    }
    if (turbo_openai_set_if_nonempty(&state->stop_reason,
                                     turbo_json_get_string(delta, "stop_reason")) != 0) {
      return -1;
    }
  }

  return 0;
}

static char *turbo_openai_build_anthropic_stream_response_json(
    const turbo_openai_anthropic_stream_state_t *state) {
  json_value_t *response;
  json_value_t *content;
  size_t i;

  if (!state || !state->id) {
    return NULL;
  }

  response = turbo_openai_anthropic_stream_response_shell_create(state, &content);
  if (!response) {
    return NULL;
  }

  if (state->content && state->content[0] != '\0') {
    json_value_t *text_block = turbo_prompt_content_text_part_create(state->content);
    if (!text_block) {
      turbo_free_json(&response);
      return NULL;
    }
    turbo_json_array_add(content, text_block);
  }

  for (i = 0; i < state->tool_call_count; ++i) {
    const turbo_openai_stream_tool_call_t *tool_call = &state->tool_calls[i];
    json_value_t *tool_use;
    json_value_t *input;

    if (!tool_call->id || !tool_call->name) {
      continue;
    }

    input = turbo_openai_stream_tool_call_input_object(tool_call);
    if (!input) {
      turbo_free_json(&response);
      return NULL;
    }

    tool_use = turbo_prompt_tool_use_part_create(tool_call->id, tool_call->name, input);
    if (!tool_use || !input) {
      turbo_free_json(&tool_use);
      turbo_free_json(&input);
      turbo_free_json(&response);
      return NULL;
    }

    turbo_json_array_add(content, tool_use);
  }

  turbo_json_object_set_string(response, "stop_reason",
                               state->stop_reason ? state->stop_reason : "end_turn");
  return turbo_openai_serialize_json_and_free(response);
}

int turbo_openai_anthropic_messages_sse_to_json(const char *sse_data, size_t sse_len,
                                                char **out_response_json) {
  char *normalized;
  char *cursor;
  turbo_openai_anthropic_stream_state_t state = {0};
  int saw_event = 0;

  if (!sse_data || !out_response_json) {
    return -1;
  }

  *out_response_json = NULL;
  normalized = turbo_openai_normalize_sse_newlines(sse_data, sse_len);
  if (!normalized) {
    return -1;
  }

  cursor = normalized;
  while (*cursor != '\0') {
    char *event_end = strstr(cursor, "\n\n");
    char *data = NULL;
    json_value_t *event = NULL;
    const char *type;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }

    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_openai_collect_sse_event_data(cursor, event_end, &data) != 0) {
      free(normalized);
      turbo_openai_free_anthropic_stream_state(&state);
      return -1;
    }

    if (strcmp(data, "[DONE]") == 0) {
      free(data);
      break;
    }

    if (data[0] != '\0' &&
        turbo_parse_json((const uint8_t *)data, strlen(data), &event) == 0) {
      type = turbo_json_get_string(event, "type");
      if (type && strcmp(type, "ping") != 0 &&
          turbo_openai_anthropic_stream_apply_event(&state, event) != 0) {
        turbo_free_json(&event);
        free(data);
        free(normalized);
        turbo_openai_free_anthropic_stream_state(&state);
        return -1;
      }
      if (type && strcmp(type, "ping") != 0) {
        saw_event = 1;
      }
      if (type && strcmp(type, "message_stop") == 0) {
        turbo_free_json(&event);
        free(data);
        break;
      }
    }

    turbo_free_json(&event);
    free(data);
    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  free(normalized);
  if (!saw_event) {
    turbo_openai_free_anthropic_stream_state(&state);
    return -1;
  }

  *out_response_json = turbo_openai_build_anthropic_stream_response_json(&state);
  turbo_openai_free_anthropic_stream_state(&state);
  return *out_response_json ? 0 : -1;
}

static int turbo_openai_configure_http_client_openai(const turbo_openai_agent_t *agent,
                                                     http_client_t *http_client) {
  if (!http_client) {
    return -1;
  }

  if (agent && agent->api_key && agent->api_key[0] != '\0') {
    http_client_set_bearer_token(http_client, agent->api_key);
  }

  http_client_set_user_agent(http_client, "TurboNet-Agent/0.1");
  return 0;
}

static int turbo_openai_configure_http_client_anthropic(const turbo_openai_agent_t *agent,
                                                        http_client_t *http_client) {
  if (!http_client) {
    return -1;
  }

  if (agent && agent->api_key && agent->api_key[0] != '\0') {
    http_client_set_default_header(http_client, "x-api-key", agent->api_key);
  }

  http_client_set_default_header(http_client, "anthropic-version", "2023-06-01");
  http_client_set_user_agent(http_client, "TurboNet-Agent/0.1");
  return 0;
}

static char *turbo_openai_format_tool_error(const char *name, const char *message) {
  int needed;
  char *buffer;

  if (!name) {
    name = "unknown";
  }
  if (!message) {
    message = "tool execution failed";
  }

  needed = snprintf(NULL, 0, "{\"ok\":false,\"tool\":%s,\"error\":%s}", "\"\"", "\"\"");
  (void)needed;

  needed = snprintf(NULL, 0, "{\"ok\":false,\"tool\":\"%s\",\"error\":\"%s\"}", name,
                    message);
  if (needed < 0) {
    return NULL;
  }

  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }

  snprintf(buffer, (size_t)needed + 1, "{\"ok\":false,\"tool\":\"%s\",\"error\":\"%s\"}",
           name, message);
  return buffer;
}

static int turbo_openai_http_transport(const char *request_json, char **out_response_json,
                                       void *user_data) {
  turbo_openai_agent_t *agent = (turbo_openai_agent_t *)user_data;
  http_response_t *response;
  char *copy;
  turbo_openai_stream_buffer_t stream_buffer = {0};

  if (!agent || !agent->http_client || !request_json || !out_response_json) {
    return -1;
  }

  turbo_openai_agent_clear_last_stream_sse(agent);
  *out_response_json = NULL;
  if (agent->stream_response) {
    response = http_sse_post_json(agent->http_client, agent->endpoint_path, request_json,
                                  turbo_openai_collect_stream_chunk, &stream_buffer);
  } else {
    response = http_post_json(agent->http_client, agent->endpoint_path, request_json);
  }
  if (!response) {
    *out_response_json =
        turbo_openai_http_transport_error_detail(NULL, stream_buffer.data, stream_buffer.length);
    free(stream_buffer.data);
    return -1;
  }

  if (stream_buffer.length == (size_t)-1) {
    *out_response_json =
        turbo_openai_strdup("http streaming callback failed while collecting model response");
    http_response_free(response);
    return -1;
  }

  if (response->error_code != HTTP_ERROR_NONE || response->status_code < 200 ||
      response->status_code >= 300 || !response->body) {
    if (agent->stream_response && response->error_code == HTTP_ERROR_NONE &&
        response->status_code >= 200 && response->status_code < 300 && stream_buffer.data) {
      http_response_free(response);
      if (turbo_openai_agent_capture_last_stream_sse(agent, stream_buffer.data,
                                                     stream_buffer.length) != 0) {
        free(stream_buffer.data);
        *out_response_json = turbo_openai_strdup("failed to capture SSE stream");
        return -1;
      }
      if (agent->provider &&
          turbo_openai_provider_sse_to_response_json(agent->provider, stream_buffer.data,
                                                     stream_buffer.length,
                                                     out_response_json) == 0) {
        free(stream_buffer.data);
        return 0;
      }
      *out_response_json =
          turbo_openai_strdup("failed to aggregate SSE stream into a JSON model response");
      free(stream_buffer.data);
      return -1;
    }

    *out_response_json =
        turbo_openai_http_transport_error_detail(response, stream_buffer.data, stream_buffer.length);
    free(stream_buffer.data);
    http_response_free(response);
    return -1;
  }

  copy = turbo_openai_strdup(response->body);
  http_response_free(response);
  free(stream_buffer.data);
  if (!copy) {
    return -1;
  }

  *out_response_json = copy;
  return 0;
}

static char *turbo_openai_http_transport_error_detail(const http_response_t *response,
                                                      const char *stream_data,
                                                      size_t stream_len) {
  const char *error_text = response && response->error ? response->error : "";
  const char *body_text = NULL;
  size_t preview_len = 0;
  int needed;
  char *buffer;

  if (response && response->body && response->body[0] != '\0') {
    body_text = response->body;
    preview_len = strlen(body_text);
  } else if (stream_data && stream_len > 0 && stream_len != (size_t)-1) {
    body_text = stream_data;
    preview_len = stream_len;
  }

  if (preview_len > 240) {
    preview_len = 240;
  }

  if (!response) {
    return turbo_openai_strdup("http transport failed before a response was created");
  }

  needed = snprintf(NULL, 0,
                    "http transport failed: status=%d error_code=%d error=%s%s%s%.*s",
                    response->status_code, (int)response->error_code,
                    error_text[0] != '\0' ? error_text : "(none)",
                    preview_len > 0 ? " body_preview=" : "", preview_len > 0 ? "\"" : "",
                    (int)preview_len, body_text ? body_text : "");
  if (needed < 0) {
    return NULL;
  }

  buffer = (char *)malloc((size_t)needed + 1 + (preview_len > 0 ? 1 : 0));
  if (!buffer) {
    return NULL;
  }

  snprintf(buffer, (size_t)needed + 1 + (preview_len > 0 ? 1 : 0),
           "http transport failed: status=%d error_code=%d error=%s%s%s%.*s%s",
           response->status_code, (int)response->error_code,
           error_text[0] != '\0' ? error_text : "(none)",
           preview_len > 0 ? " body_preview=" : "", preview_len > 0 ? "\"" : "",
           (int)preview_len, body_text ? body_text : "", preview_len > 0 ? "\"" : "");
  return buffer;
}

static int turbo_openai_build_responses_turn_request(const turbo_openai_agent_t *agent,
                                                     json_value_t *state,
                                                     char **out_request_json) {
  json_value_t *request;
  json_value_t *input_clone = NULL;
  json_value_t *tools = NULL;
  const json_value_t *input_source = NULL;
  char *effective_instructions = NULL;

  if (!agent || !state || !out_request_json) {
    return -1;
  }

  *out_request_json = NULL;
  request = turbo_openai_request_create(agent);
  if (!request) {
    return -1;
  }

  input_source = turbo_openai_responses_request_input_source(state, request);

  if (!input_source) {
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_json_object_get(request, "previous_response_id")) {
    if (turbo_openai_clone_json(input_source, &input_clone) != TURBO_GRAPH_EXEC_OK) {
      turbo_free_json(&request);
      return -1;
    }
  } else {
    input_clone = turbo_openai_build_responses_input_messages(input_source);
    if (!input_clone) {
      turbo_free_json(&request);
      return -1;
    }
  }

  effective_instructions = turbo_openai_agent_build_effective_instructions(agent, state);
  if ((agent->instructions || turbo_openai_agent_state_memory_layer_count(state) > 0) &&
      !effective_instructions) {
    turbo_free_json(&request);
    return -1;
  }

  turbo_json_object_add(request, "input", input_clone);
  if (effective_instructions && effective_instructions[0] != '\0') {
    turbo_json_object_set_string(request, "instructions", effective_instructions);
  }
  if (turbo_openai_agent_parallel_tool_calls_enabled(agent)) {
    turbo_json_object_set_bool(request, "parallel_tool_calls", true);
  }

  tools = turbo_tool_schema_build_openai_tools(agent->tool_registry);
  if (!tools) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_openai_request_add_tools_if_any(request, tools) != 0) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_openai_request_add_structured_output(agent, request, 0, 0, 0) != 0) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  {
    int rc = turbo_openai_request_serialize_into_output(request, out_request_json);
    free(effective_instructions);
    return rc;
  }
}

static json_value_t *turbo_openai_build_canonical_messages(const turbo_openai_agent_t *agent,
                                                           const json_value_t *state) {
  const json_value_t *input;
  const json_value_t *events;
  json_value_t *messages;
  char *effective_instructions;
  size_t i;

  if (!agent || !state) {
    return NULL;
  }

  messages = turbo_prompt_messages_create();
  if (!messages) {
    return NULL;
  }

  effective_instructions = turbo_openai_agent_build_effective_instructions(agent, state);
  if ((agent->instructions || turbo_openai_agent_state_memory_layer_count(state) > 0) &&
      !effective_instructions) {
    turbo_free_json(&messages);
    return NULL;
  }

  if (effective_instructions && effective_instructions[0] != '\0') {
    json_value_t *system_message = turbo_prompt_message_create("system", effective_instructions);
    if (!system_message) {
      free(effective_instructions);
      turbo_free_json(&messages);
      return NULL;
    }
    turbo_json_array_add(messages, system_message);
  }
  free(effective_instructions);

  input = turbo_openai_state_get_array_const(state, "input");
  if (!input) {
    turbo_free_json(&messages);
    return NULL;
  }

  for (i = 0; i < turbo_json_array_size(input); ++i) {
    const json_value_t *message = turbo_json_array_get(input, i);
    if (turbo_openai_array_add_clone(messages, message) != 0) {
      turbo_free_json(&messages);
      return NULL;
    }
  }

  events = turbo_openai_state_get_array_const(state, "events");
  if (turbo_openai_replay_message_events(events, messages,
                                         turbo_openai_append_chat_model_event_message,
                                         turbo_openai_append_chat_tool_results_messages) != 0) {
    turbo_free_json(&messages);
    return NULL;
  }

  return messages;
}

static json_value_t *turbo_openai_build_chat_messages(const turbo_openai_agent_t *agent,
                                                      const json_value_t *state) {
  json_value_t *canonical_messages;
  turbo_runtime_data_bind_value_t *messages_bind;
  json_value_t *wire_messages;

  canonical_messages = turbo_openai_build_canonical_messages(agent, state);
  if (!canonical_messages) {
    return NULL;
  }

  messages_bind = turbo_runtime_data_bind_value_from_json(canonical_messages);
  turbo_free_json(&canonical_messages);
  if (!messages_bind) {
    return NULL;
  }

  wire_messages = turbo_model_provider_messages_to_wire_json(
      turbo_model_provider_openai_chat_completions(), messages_bind, NULL);
  turbo_runtime_data_bind_value_destroy(messages_bind);
  return wire_messages;
}

static int turbo_openai_build_anthropic_wire_messages(const turbo_openai_agent_t *agent,
                                                      const json_value_t *state,
                                                      json_value_t **out_messages,
                                                      char **out_system) {
  json_value_t *canonical_messages;
  turbo_runtime_data_bind_value_t *messages_bind;

  if (!out_messages) {
    return -1;
  }

  *out_messages = NULL;
  if (out_system) {
    *out_system = NULL;
  }

  canonical_messages = turbo_openai_build_canonical_messages(agent, state);
  if (!canonical_messages) {
    return -1;
  }

  messages_bind = turbo_runtime_data_bind_value_from_json(canonical_messages);
  turbo_free_json(&canonical_messages);
  if (!messages_bind) {
    return -1;
  }

  *out_messages = turbo_model_provider_messages_to_wire_json(
      turbo_model_provider_anthropic_messages(), messages_bind, out_system);
  turbo_runtime_data_bind_value_destroy(messages_bind);
  return *out_messages ? 0 : -1;
}

static int turbo_openai_build_chat_turn_request_with_options(const turbo_openai_agent_t *agent,
                                                             json_value_t *state,
                                                             int compatible_mode,
                                                             char **out_request_json) {
  json_value_t *request;
  json_value_t *messages;
  json_value_t *tools = NULL;

  if (!agent || !state || !out_request_json) {
    return -1;
  }

  *out_request_json = NULL;
  request = turbo_openai_request_create(agent);
  if (!request) {
    return -1;
  }

  messages = turbo_openai_build_chat_messages(agent, state);
  if (!messages) {
    turbo_free_json(&request);
    return -1;
  }

  turbo_json_object_add(request, "messages", messages);
  if (!compatible_mode && turbo_openai_agent_parallel_tool_calls_enabled(agent)) {
    turbo_json_object_set_bool(request, "parallel_tool_calls", true);
  }

  tools = compatible_mode
              ? turbo_tool_schema_build_openai_compatible_chat_tools(agent->tool_registry)
              : turbo_tool_schema_build_openai_chat_tools(agent->tool_registry);
  if (!tools) {
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_openai_request_add_tools_if_any(request, tools) != 0) {
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_json_object_get(request, "tools")) {
    if (!compatible_mode) {
      turbo_json_object_set_string(request, "tool_choice", "auto");
    }
  }

  if (turbo_openai_request_add_structured_output(agent, request, 1, compatible_mode, 0) != 0) {
    turbo_free_json(&request);
    return -1;
  }

  return turbo_openai_request_serialize_into_output(request, out_request_json);
}

static int turbo_openai_build_chat_turn_request(const turbo_openai_agent_t *agent,
                                                json_value_t *state,
                                                char **out_request_json) {
  return turbo_openai_build_chat_turn_request_with_options(agent, state, 0, out_request_json);
}

static int turbo_openai_build_compatible_chat_turn_request(const turbo_openai_agent_t *agent,
                                                           json_value_t *state,
                                                           char **out_request_json) {
  return turbo_openai_build_chat_turn_request_with_options(agent, state, 1, out_request_json);
}

static int turbo_openai_build_anthropic_messages_turn_request(const turbo_openai_agent_t *agent,
                                                              json_value_t *state,
                                                              char **out_request_json) {
  json_value_t *request;
  json_value_t *messages;
  json_value_t *tools = NULL;
  char *effective_instructions = NULL;
  char *system_from_messages = NULL;

  if (!agent || !state || !out_request_json) {
    return -1;
  }

  *out_request_json = NULL;
  request = turbo_openai_request_create(agent);
  if (!request) {
    return -1;
  }

  if (turbo_openai_build_anthropic_wire_messages(agent, state, &messages, &system_from_messages) !=
      0) {
    turbo_free_json(&request);
    return -1;
  }

  effective_instructions = turbo_openai_agent_build_effective_instructions(agent, state);
  if ((agent->instructions || turbo_openai_agent_state_memory_layer_count(state) > 0) &&
      !effective_instructions) {
    turbo_free_json(&request);
    return -1;
  }

  turbo_json_object_add(request, "messages", messages);
  if (effective_instructions && effective_instructions[0] != '\0' && system_from_messages &&
      system_from_messages[0] != '\0') {
    char *joined = turbo_openai_join_text_blocks(effective_instructions, "\n\n",
                                                 system_from_messages);
    if (!joined) {
      free(system_from_messages);
      free(effective_instructions);
      turbo_free_json(&request);
      return -1;
    }
    free(effective_instructions);
    effective_instructions = joined;
  } else if ((!effective_instructions || effective_instructions[0] == '\0') &&
             system_from_messages && system_from_messages[0] != '\0') {
    free(effective_instructions);
    effective_instructions = system_from_messages;
    system_from_messages = NULL;
  }

  if (effective_instructions && effective_instructions[0] != '\0') {
    turbo_json_object_set_string(request, "system", effective_instructions);
  }
  free(system_from_messages);

  tools = turbo_tool_schema_build_anthropic_tools(agent->tool_registry);
  if (!tools) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_openai_request_add_tools_if_any(request, tools) != 0) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_openai_request_add_structured_output(agent, request, 0, 0, 1) != 0) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  {
    int rc = turbo_openai_request_serialize_into_output(request, out_request_json);
    free(effective_instructions);
    return rc;
  }
}

static int turbo_openai_build_turn_request(turbo_openai_agent_t *agent, json_value_t *state,
                                           char **out_request_json) {
  if (!agent || !agent->provider || !agent->provider->build_request) {
    return -1;
  }

  return agent->provider->build_request(agent, state, out_request_json);
}

static int turbo_openai_append_model_event(turbo_openai_agent_t *agent, json_value_t *state,
                                           const json_value_t *response) {
  json_value_t *event = NULL;

  if (!agent || !agent->provider || !state) {
    return -1;
  }

  if (agent->last_stream_sse && agent->last_stream_sse_len > 0) {
    event = turbo_model_provider_sse_to_event_json(agent->provider, agent->last_stream_sse,
                                                   agent->last_stream_sse_len);
  }
  if (!event && response) {
    event = turbo_model_provider_response_to_event_json(agent->provider, response);
  }
  turbo_openai_agent_clear_last_stream_sse(agent);
  if (!event) {
    return -1;
  }

  return turbo_openai_append_event(state, event);
}

static const json_value_t *turbo_openai_last_model_tool_calls(const json_value_t *state) {
  const json_value_t *event = turbo_openai_state_last_event_of_kind(state, "model");
  return turbo_openai_model_event_tool_calls(event);
}

static json_value_t *turbo_openai_planner_loop_build_step_input(const json_value_t *state) {
  const json_value_t *input;
  json_value_t *step_input;
  json_value_t *context_message = NULL;
  json_value_t *message;
  const char *step_text;
  char *completed_steps_message = NULL;
  size_t step_index;
  size_t step_count;
  int needed;
  char *prompt;

  if (!state) {
    return NULL;
  }

  input = turbo_openai_state_get_array_const(state, "input");
  step_text = turbo_openai_agent_state_current_plan_step_text(state);
  step_index = turbo_openai_agent_state_plan_step_index(state);
  step_count = turbo_openai_agent_state_plan_step_count(state);
  if (!input || !step_text || step_text[0] == '\0') {
    return NULL;
  }

  step_input = NULL;
  if (turbo_openai_clone_json(input, &step_input) != TURBO_GRAPH_EXEC_OK) {
    return NULL;
  }

  completed_steps_message = turbo_openai_build_completed_steps_message(state);
  if (completed_steps_message) {
    context_message = turbo_prompt_message_create("user", completed_steps_message);
    if (!context_message) {
      free(completed_steps_message);
      turbo_free_json(&step_input);
      return NULL;
    }
    turbo_json_array_add(step_input, context_message);
    free(completed_steps_message);
  }

  needed = snprintf(NULL, 0,
                    "Execute plan step %lu of %lu: %s\nReturn only the result for this step.",
                    (unsigned long)(step_index + 1), (unsigned long)step_count, step_text);
  if (needed < 0) {
    turbo_free_json(&step_input);
    return NULL;
  }

  prompt = (char *)malloc((size_t)needed + 1);
  if (!prompt) {
    turbo_free_json(&step_input);
    return NULL;
  }

  snprintf(prompt, (size_t)needed + 1,
           "Execute plan step %lu of %lu: %s\nReturn only the result for this step.",
           (unsigned long)(step_index + 1), (unsigned long)step_count, step_text);
  message = turbo_prompt_message_create("user", prompt);
  free(prompt);
  if (!message) {
    turbo_free_json(&step_input);
    return NULL;
  }
  turbo_json_array_add(step_input, message);
  return step_input;
}

static json_value_t *turbo_openai_substate_create(json_value_t *input, json_value_t *events) {
  json_value_t *substate;

  if (!input || !events || turbo_json_type(input) != TURBO_JSON_ARRAY ||
      turbo_json_type(events) != TURBO_JSON_ARRAY) {
    return NULL;
  }

  substate = turbo_json_create_object();
  if (!substate) {
    return NULL;
  }

  turbo_json_object_add(substate, "input", input);
  turbo_json_object_add(substate, "events", events);
  return substate;
}

static json_value_t *turbo_openai_state_ensure_versioned_substate(
    json_value_t *root_state, const char *state_key, const char *legacy_input_key,
    const char *event_versions_key) {
  json_value_t *substate;
  json_value_t *input;
  json_value_t *events;
  json_value_t *input_copy = NULL;
  json_value_t *events_copy = NULL;

  if (!root_state || !state_key || !event_versions_key) {
    return NULL;
  }

  substate = turbo_openai_state_get_current_object_version(root_state, state_key);
  if (substate && turbo_json_type(substate) == TURBO_JSON_OBJECT) {
    input = turbo_openai_state_get_array(substate, "input");
    events = turbo_openai_state_get_array(substate, "events");
    if (input && events && turbo_json_type(input) == TURBO_JSON_ARRAY &&
        turbo_json_type(events) == TURBO_JSON_ARRAY) {
      return substate;
    }
  }

  if (!legacy_input_key) {
    return NULL;
  }

  input = turbo_openai_state_get_current_array_version(root_state, legacy_input_key);
  events = turbo_openai_state_get_current_array_version(root_state, event_versions_key);
  if (!input || turbo_json_type(input) != TURBO_JSON_ARRAY) {
    return NULL;
  }
  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY) {
    events = turbo_json_create_array();
    if (!events) {
      return NULL;
    }
  }

  if (turbo_openai_clone_json(input, &input_copy) != TURBO_GRAPH_EXEC_OK ||
      turbo_openai_clone_json(events, &events_copy) != TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&input_copy);
    turbo_free_json(&events_copy);
    if (events != turbo_openai_state_get_current_array_version(root_state, event_versions_key)) {
      turbo_free_json(&events);
    }
    return NULL;
  }

  substate = turbo_openai_substate_create(input_copy, events_copy);
  if (!substate || turbo_openai_state_append_object_version(root_state, state_key, substate) != 0) {
    turbo_free_json(&input_copy);
    turbo_free_json(&events_copy);
    return NULL;
  }

  return turbo_openai_state_get_current_object_version(root_state, state_key);
}

static int turbo_openai_append_last_substate_event(json_value_t *root_state,
                                                   const char *event_versions_key,
                                                   const json_value_t *substate) {
  json_value_t *version_events;
  const json_value_t *last_event;

  if (!root_state || !event_versions_key || !substate) {
    return -1;
  }

  last_event = turbo_openai_state_last_event(substate);
  if (!last_event) {
    return -1;
  }

  version_events = turbo_openai_state_get_current_array_version(root_state, event_versions_key);
  if (!version_events) {
    version_events = turbo_json_create_array();
    if (!version_events ||
        turbo_openai_state_append_array_version(root_state, event_versions_key, version_events) != 0) {
      turbo_free_json(&version_events);
      return -1;
    }
    version_events = turbo_openai_state_get_current_array_version(root_state, event_versions_key);
  }

  if (turbo_openai_array_add_clone(version_events, last_event) != 0) {
    return -1;
  }
  return turbo_openai_append_event_clone(root_state, last_event);
}

int turbo_openai_agent_plan_step_prepare_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  json_value_t *step_input;
  json_value_t *step_events;
  json_value_t *step_history;
  json_value_t *executor_state;

  (void)user_data;
  if (!ctx || !ctx->state || turbo_openai_agent_state_plan_complete(ctx->state)) {
    return -1;
  }

  step_input = turbo_openai_planner_loop_build_step_input(ctx->state);
  step_events = turbo_json_create_array();
  step_history = turbo_json_create_array();
  if (!step_input || !step_events || !step_history) {
    turbo_free_json(&step_input);
    turbo_free_json(&step_events);
    turbo_free_json(&step_history);
    return -1;
  }

  if (turbo_openai_state_append_array_version(ctx->state, "executor_event_versions", step_history) !=
      0) {
    turbo_free_json(&step_input);
    turbo_free_json(&step_events);
    turbo_free_json(&step_history);
    return -1;
  }

  executor_state = turbo_openai_substate_create(step_input, step_events);
  if (!executor_state ||
      turbo_openai_state_append_object_version(ctx->state, "executor_state_versions", executor_state) !=
          0) {
    turbo_free_json(&step_input);
    turbo_free_json(&step_events);
    return -1;
  }

  return 0;
}

int turbo_openai_agent_executor_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_openai_agent_t *agent = (turbo_openai_agent_t *)user_data;
  json_value_t *executor_state;
  turbo_graph_exec_ctx_t temp_ctx = {0};
  int rc;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  executor_state = turbo_openai_state_ensure_versioned_substate(
      ctx->state, "executor_state_versions", "executor_input_versions", "executor_event_versions");
  if (!executor_state) {
    return -1;
  }

  temp_ctx.graph = ctx->graph;
  temp_ctx.state = executor_state;
  rc = turbo_openai_agent_model_node(&temp_ctx, agent);
  if (rc == 0) {
    rc = turbo_openai_append_last_substate_event(ctx->state, "executor_event_versions",
                                                 executor_state);
  } else {
    turbo_openai_agent_copy_model_error(ctx->state, executor_state);
  }

  return rc;
}

int turbo_openai_agent_executor_tool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_openai_agent_t *agent = (turbo_openai_agent_t *)user_data;
  json_value_t *executor_state;
  turbo_graph_exec_ctx_t temp_ctx = {0};
  int rc;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  executor_state = turbo_openai_state_ensure_versioned_substate(
      ctx->state, "executor_state_versions", "executor_input_versions", "executor_event_versions");
  if (!executor_state) {
    return -1;
  }

  temp_ctx.graph = ctx->graph;
  temp_ctx.state = executor_state;
  rc = turbo_openai_agent_tool_node(&temp_ctx, agent);
  if (rc == 0) {
    rc = turbo_openai_append_last_substate_event(ctx->state, "executor_event_versions",
                                                 executor_state);
  } else {
    turbo_openai_agent_copy_model_error(ctx->state, executor_state);
    turbo_openai_agent_copy_guardrail_rejection(ctx->state, executor_state);
  }

  return rc;
}

int turbo_openai_agent_plan_advance_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *step_text;
  const char *output_text;
  size_t step_index;

  (void)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  step_text = turbo_openai_agent_state_current_plan_step_text(ctx->state);
  output_text = turbo_openai_agent_state_latest_executor_output_text(ctx->state);
  step_index = turbo_openai_agent_state_plan_step_index(ctx->state);
  if (step_text && output_text && output_text[0] != '\0' &&
      turbo_openai_append_completed_step(ctx->state, step_index, step_text, output_text) != 0) {
    return -1;
  }

  return turbo_openai_agent_state_advance_plan(ctx->state);
}

static int turbo_openai_agent_plan_complete_predicate(const turbo_graph_exec_ctx_t *ctx,
                                                      void *user_data) {
  (void)user_data;
  return ctx && ctx->state ? turbo_openai_agent_state_plan_complete(ctx->state) : 0;
}

static int turbo_openai_end_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  turbo_graph_ctx_stop(ctx);
  return 0;
}

turbo_openai_agent_t *
turbo_openai_agent_create(const turbo_openai_agent_config_t *config) {
  turbo_openai_agent_t *agent;
  const turbo_model_provider_t *provider;

  if (!config || !config->model) {
    return NULL;
  }

  if (!config->transport_fn && !config->http_client && !config->api_key) {
    return NULL;
  }

  agent = (turbo_openai_agent_t *)calloc(1, sizeof(*agent));
  if (!agent) {
    return NULL;
  }

  provider = turbo_openai_resolve_provider(config->provider, config->api_mode, &agent->api_mode);
  if (turbo_openai_agent_apply_core_config(agent, config, provider) != 0) {
    turbo_openai_agent_destroy(agent);
    return NULL;
  }

  if (turbo_openai_agent_attach_http_client(agent, config, provider) != 0) {
    turbo_openai_agent_destroy(agent);
    return NULL;
  }

  turbo_openai_agent_finalize_transport(agent);
  return agent;
}

turbo_openai_agent_t *turbo_openai_agent_create_with_action_tools(
    const turbo_openai_agent_config_t *config,
    const turbo_action_tool_registry_t *action_tool_registry) {
  turbo_tool_registry_t *tool_registry;

  if (!config || !action_tool_registry) {
    return NULL;
  }

  tool_registry = turbo_action_tool_registry_build_tool_registry_bridge(action_tool_registry);
  if (!tool_registry) {
    return NULL;
  }

  return turbo_openai_agent_create_with_owned_tool_registry(config, tool_registry);
}

void turbo_openai_agent_destroy(turbo_openai_agent_t *agent) {
  if (!agent) {
    return;
  }

  turbo_openai_agent_release_owned_resources(agent);
  turbo_openai_agent_free_strings(agent);
  free(agent);
}

json_value_t *turbo_openai_agent_state_create(void) {
  json_value_t *state = turbo_json_create_object();
  json_value_t *input;
  json_value_t *events;

  if (!state) {
    return NULL;
  }

  input = turbo_json_create_array();
  events = turbo_json_create_array();
  if (!input || !events) {
    turbo_free_json(&input);
    turbo_free_json(&events);
    turbo_free_json(&state);
    return NULL;
  }

  turbo_json_object_set_number(state, "state_version",
                               (double)TURBO_OPENAI_AGENT_STATE_SCHEMA_VERSION);
  turbo_json_object_add(state, "input", input);
  turbo_json_object_add(state, "events", events);
  return state;
}

turbo_runtime_data_bind_value_t *turbo_openai_agent_state_create_bind(void) {
  json_value_t *state = turbo_openai_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  if (!state) {
    return NULL;
  }

  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

size_t turbo_openai_agent_state_schema_version(void) {
  return TURBO_OPENAI_AGENT_STATE_SCHEMA_VERSION;
}

size_t turbo_openai_agent_state_version(const json_value_t *state) {
  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return 0;
  }

  return (size_t)turbo_json_get_double(state, "state_version", 0);
}

int turbo_openai_agent_state_version_supported(const json_value_t *state) {
  size_t version = turbo_openai_agent_state_version(state);
  return version == 0 || version == TURBO_OPENAI_AGENT_STATE_SCHEMA_VERSION ? 1 : 0;
}

int turbo_openai_agent_state_add_user_message(json_value_t *state, const char *text) {
  json_value_t *input;

  if (!state || !text) {
    return -1;
  }

  input = turbo_openai_state_get_array(state, "input");
  if (!input) {
    return -1;
  }

  return turbo_prompt_messages_append(input, "user", text) == TURBO_PROMPT_OK ? 0 : -1;
}

const json_value_t *turbo_openai_agent_state_events(const json_value_t *state) {
  return turbo_openai_state_get_array_const(state, "events");
}

size_t turbo_openai_agent_state_event_count(const json_value_t *state) {
  const json_value_t *events = turbo_openai_agent_state_events(state);
  return events && turbo_json_type(events) == TURBO_JSON_ARRAY ? turbo_json_array_size(events) : 0;
}

const json_value_t *turbo_openai_agent_state_event_at(const json_value_t *state, size_t index) {
  const json_value_t *events = turbo_openai_agent_state_events(state);

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(events)) {
    return NULL;
  }

  return turbo_json_array_get(events, index);
}

const json_value_t *turbo_openai_agent_state_trace_events(const json_value_t *state) {
  return turbo_openai_state_get_array_const(state, "trace_events");
}

size_t turbo_openai_agent_state_trace_event_count(const json_value_t *state) {
  const json_value_t *events = turbo_openai_agent_state_trace_events(state);
  return events && turbo_json_type(events) == TURBO_JSON_ARRAY ? turbo_json_array_size(events) : 0;
}

const json_value_t *turbo_openai_agent_state_trace_event_at(const json_value_t *state,
                                                            size_t index) {
  const json_value_t *events = turbo_openai_agent_state_trace_events(state);

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(events)) {
    return NULL;
  }

  return turbo_json_array_get(events, index);
}

turbo_runtime_data_bind_value_t *
turbo_openai_agent_state_trace_events_bind(const turbo_runtime_data_bind_value_t *state) {
  json_value_t *json_state = turbo_openai_agent_state_bind_to_json_object(state);
  const json_value_t *events;
  turbo_runtime_data_bind_value_t *bound = NULL;

  if (!json_state) {
    return NULL;
  }

  events = turbo_openai_agent_state_trace_events(json_state);
  if (events) {
    bound = turbo_runtime_data_bind_value_from_json(events);
  }

  turbo_free_json(&json_state);
  return bound;
}

static turbo_runtime_data_bind_value_t *turbo_openai_agent_state_array_version_bind(
    const turbo_runtime_data_bind_value_t *state, const char *field_name, size_t index) {
  json_value_t *json_state = turbo_openai_agent_state_bind_to_json_object(state);
  const json_value_t *versions;
  const json_value_t *version;
  turbo_runtime_data_bind_value_t *bound = NULL;

  if (!json_state || !field_name) {
    turbo_free_json(&json_state);
    return NULL;
  }

  versions = turbo_json_object_get(json_state, field_name);
  if (versions && turbo_json_type(versions) == TURBO_JSON_ARRAY &&
      index < turbo_json_array_size(versions)) {
    version = turbo_json_array_get(versions, index);
    if (version && turbo_json_type(version) == TURBO_JSON_ARRAY) {
      bound = turbo_runtime_data_bind_value_from_json(version);
    }
  }

  turbo_free_json(&json_state);
  return bound;
}

static turbo_runtime_data_bind_value_t *turbo_openai_agent_state_latest_array_version_bind(
    const turbo_runtime_data_bind_value_t *state, const char *field_name) {
  json_value_t *json_state = turbo_openai_agent_state_bind_to_json_object(state);
  const json_value_t *versions;
  size_t count = 0;
  turbo_runtime_data_bind_value_t *bound = NULL;

  if (!json_state || !field_name) {
    turbo_free_json(&json_state);
    return NULL;
  }

  versions = turbo_json_object_get(json_state, field_name);
  if (versions && turbo_json_type(versions) == TURBO_JSON_ARRAY) {
    count = turbo_json_array_size(versions);
  }
  turbo_free_json(&json_state);

  if (count == 0) {
    return NULL;
  }

  return turbo_openai_agent_state_array_version_bind(state, field_name, count - 1);
}

int turbo_openai_agent_state_add_trace_event_bind(
    turbo_runtime_data_bind_value_t *state, const turbo_runtime_data_bind_value_t *event) {
  const turbo_runtime_data_bind_value_t *trace_events_const;
  turbo_runtime_data_bind_value_t *trace_events;
  turbo_runtime_data_bind_value_t *event_copy;

  if (!state || !event ||
      turbo_runtime_data_bind_value_kind(state) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT ||
      turbo_event_trace_validate_bind(event) != 0) {
    return -1;
  }

  trace_events_const = turbo_runtime_data_bind_object_get(state, "trace_events");
  if (!trace_events_const) {
    trace_events = turbo_runtime_data_bind_value_create_array();
    if (!trace_events ||
        turbo_runtime_data_bind_object_set(state, "trace_events", trace_events) !=
            TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(trace_events);
      return -1;
    }
    trace_events_const = turbo_runtime_data_bind_object_get(state, "trace_events");
  }

  if (!trace_events_const ||
      turbo_runtime_data_bind_value_kind(trace_events_const) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return -1;
  }

  event_copy = turbo_runtime_data_bind_value_clone(event);
  if (!event_copy) {
    return -1;
  }

  if (turbo_runtime_data_bind_array_append((turbo_runtime_data_bind_value_t *)trace_events_const,
                                           event_copy) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(event_copy);
    return -1;
  }

  return 0;
}

void turbo_openai_agent_state_capture_trace_event_bind(const turbo_runtime_data_bind_value_t *event,
                                                       void *user_data) {
  turbo_runtime_data_bind_value_t *state = (turbo_runtime_data_bind_value_t *)user_data;

  if (!state) {
    return;
  }

  turbo_openai_agent_state_add_trace_event_bind(state, event);
}

int turbo_openai_agent_state_set_memory_json(json_value_t *state, const char *key,
                                             const char *value_json) {
  json_value_t *memory;
  json_value_t *value = NULL;

  if (!state || !key || !value_json || key[0] == '\0' || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  memory = turbo_openai_state_get_or_create_object(state, "memory");
  if (!memory) {
    return -1;
  }

  if (turbo_parse_json((const uint8_t *)value_json, strlen(value_json), &value) != 0) {
    return -1;
  }

  turbo_json_object_add(memory, key, value);
  return 0;
}

const json_value_t *turbo_openai_agent_state_memory_json(const json_value_t *state, const char *key) {
  const json_value_t *memory;
  size_t i;

  if (!state || !key || key[0] == '\0' || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  memory = turbo_openai_state_get_object_const(state, "memory");
  if (!memory || turbo_json_type(memory) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  for (i = turbo_json_object_size(memory); i > 0; --i) {
    const char *memory_key = turbo_json_object_key(memory, i - 1);
    if (memory_key && strcmp(memory_key, key) == 0) {
      return turbo_json_object_value(memory, i - 1);
    }
  }

  return NULL;
}

int turbo_openai_agent_state_load_memory(turbo_openai_agent_t *agent, json_value_t *state,
                                         const char *key) {
  char *value_json = NULL;
  int rc;

  if (!agent || !state || !key || !agent->has_store || !agent->store.get) {
    return -1;
  }

  rc = agent->store.get(agent->store.user_data, key, &value_json);
  if (rc != 0 || !value_json) {
    free(value_json);
    return -1;
  }

  rc = turbo_openai_agent_state_set_memory_json(state, key, value_json);
  if (rc == 0) {
    turbo_openai_agent_emit_trace(agent, state, TURBO_OPENAI_AGENT_TRACE_MEMORY_LOAD, key, NULL,
                                  value_json, 0);
  }
  free(value_json);
  return rc;
}

int turbo_openai_agent_state_save_memory(turbo_openai_agent_t *agent, json_value_t *state,
                                         const char *key) {
  const json_value_t *value;
  char *serialized;
  int rc;

  if (!agent || !state || !key || !agent->has_store || !agent->store.put) {
    return -1;
  }

  value = turbo_openai_agent_state_memory_json(state, key);
  if (!value) {
    return agent->store.remove ? agent->store.remove(agent->store.user_data, key) : -1;
  }

  serialized = turbo_json_serialize(value, NULL);
  if (!serialized) {
    return -1;
  }

  rc = agent->store.put(agent->store.user_data, key, serialized);
  if (rc == 0) {
    turbo_openai_agent_emit_trace(agent, state, TURBO_OPENAI_AGENT_TRACE_MEMORY_SAVE, key, NULL,
                                  serialized, 0);
  }
  turbo_json_serialize_free(serialized);
  return rc;
}

int turbo_openai_agent_state_add_memory_context_layer(json_value_t *state, const char *scope,
                                                      const char *path, const char *text) {
  json_value_t *memory_context;
  json_value_t *layers;
  json_value_t *layer;

  if (!state || !scope || scope[0] == '\0' || !text || text[0] == '\0' ||
      turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }

  memory_context = turbo_openai_state_get_or_create_object(state, "memory_context");
  if (!memory_context) {
    return -1;
  }

  layers = turbo_json_object_get(memory_context, "layers");
  if (!layers) {
    layers = turbo_json_create_array();
    if (!layers) {
      return -1;
    }
    turbo_json_object_add(memory_context, "layers", layers);
  }
  if (turbo_json_type(layers) != TURBO_JSON_ARRAY) {
    return -1;
  }

  layer = turbo_json_create_object();
  if (!layer) {
    return -1;
  }

  turbo_json_object_set_string(layer, "scope", scope);
  if (path && path[0] != '\0') {
    turbo_json_object_set_string(layer, "path", path);
  } else {
    turbo_json_object_set_string(layer, "path", "");
  }
  turbo_json_object_set_string(layer, "text", text);
  turbo_json_array_add(layers, layer);
  return 0;
}

const json_value_t *turbo_openai_agent_state_memory_context(const json_value_t *state) {
  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  return turbo_openai_state_get_object_const(state, "memory_context");
}

const json_value_t *turbo_openai_agent_state_memory_layers(const json_value_t *state) {
  const json_value_t *memory_context = turbo_openai_agent_state_memory_context(state);
  const json_value_t *layers;

  if (!memory_context || turbo_json_type(memory_context) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  layers = turbo_json_object_get(memory_context, "layers");
  return layers && turbo_json_type(layers) == TURBO_JSON_ARRAY ? layers : NULL;
}

size_t turbo_openai_agent_state_memory_layer_count(const json_value_t *state) {
  const json_value_t *layers = turbo_openai_agent_state_memory_layers(state);
  return layers ? turbo_json_array_size(layers) : 0;
}

const json_value_t *turbo_openai_agent_state_memory_layer_at(const json_value_t *state,
                                                             size_t index) {
  const json_value_t *layers = turbo_openai_agent_state_memory_layers(state);

  if (!layers || index >= turbo_json_array_size(layers)) {
    return NULL;
  }

  return turbo_json_array_get(layers, index);
}

char *turbo_openai_agent_state_memory_context_text(const json_value_t *state) {
  return turbo_openai_agent_memory_context_text_internal(state);
}

int turbo_openai_agent_state_set_plan_from_json(json_value_t *state, const char *plan_json) {
  json_value_t *root = NULL;
  json_value_t *plan_object = NULL;
  json_value_t *steps = NULL;
  const json_value_t *input_steps = NULL;
  char *extracted_json = NULL;
  int rc = -1;

  if (!state || !plan_json) {
    return -1;
  }

  if (turbo_parse_json((const uint8_t *)plan_json, strlen(plan_json), &root) != 0) {
    extracted_json = turbo_openai_extract_first_json_value(plan_json);
    if (!extracted_json ||
        turbo_parse_json((const uint8_t *)extracted_json, strlen(extracted_json), &root) != 0) {
      free(extracted_json);
      return -1;
    }
  }

  if (turbo_json_type(root) == TURBO_JSON_ARRAY) {
    input_steps = root;
  } else if (turbo_json_type(root) == TURBO_JSON_OBJECT) {
    input_steps = turbo_json_object_get(root, "steps");
  }

  if (!input_steps || turbo_openai_plan_normalize_steps(input_steps, &steps) != 0) {
    turbo_free_json(&root);
    return -1;
  }

  plan_object = turbo_json_create_object();
  if (!plan_object) {
    turbo_free_json(&steps);
    turbo_free_json(&root);
    return -1;
  }

  turbo_json_object_add(plan_object, "steps", steps);
  rc = turbo_openai_state_set_plan_object(state, plan_object);
  turbo_free_json(&root);
  free(extracted_json);
  return rc;
}

const json_value_t *turbo_openai_agent_state_plan(const json_value_t *state) {
  return turbo_openai_state_get_current_object_version_const(state, "plan_versions");
}

size_t turbo_openai_agent_state_plan_step_count(const json_value_t *state) {
  const json_value_t *steps = turbo_openai_agent_state_plan_steps_const(state);

  if (!steps || turbo_json_type(steps) != TURBO_JSON_ARRAY) {
    return 0;
  }

  return turbo_json_array_size(steps);
}

size_t turbo_openai_agent_state_plan_step_index(const json_value_t *state) {
  const json_value_t *progress =
      turbo_openai_state_get_current_array_version_const(state, "plan_progress_versions");

  return progress && turbo_json_type(progress) == TURBO_JSON_ARRAY ? turbo_json_array_size(progress)
                                                                   : 0;
}

const char *turbo_openai_agent_state_current_plan_step_text(const json_value_t *state) {
  const json_value_t *steps;
  const json_value_t *step;
  size_t index;
 
  steps = turbo_openai_agent_state_plan_steps_const(state);
  if (!steps || turbo_json_type(steps) != TURBO_JSON_ARRAY) {
    return NULL;
  }

  index = turbo_openai_agent_state_plan_step_index(state);
  if (index >= turbo_json_array_size(steps)) {
    return NULL;
  }

  step = turbo_json_array_get(steps, index);
  if (!step || turbo_json_type(step) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  return turbo_json_get_string(step, "text");
}

int turbo_openai_agent_state_advance_plan(json_value_t *state) {
  json_value_t *progress;
  size_t current_index;
  size_t total;

  if (!state) {
    return -1;
  }

  progress = turbo_openai_state_get_current_array_version(state, "plan_progress_versions");
  if (!progress || turbo_json_type(progress) != TURBO_JSON_ARRAY) {
    return -1;
  }

  {
    const json_value_t *steps = turbo_openai_agent_state_plan_steps_const(state);
    if (!steps || turbo_json_type(steps) != TURBO_JSON_ARRAY) {
      return -1;
    }

    total = turbo_json_array_size(steps);
  }
  current_index = turbo_json_array_size(progress);
  if (current_index >= total) {
    return 0;
  }

  turbo_json_array_add(progress, turbo_json_create_bool(true));
  return 0;
}

int turbo_openai_agent_state_plan_complete(const json_value_t *state) {
  return turbo_openai_agent_state_plan_step_index(state) >=
                 turbo_openai_agent_state_plan_step_count(state)
             ? 1
             : 0;
}

int turbo_openai_agent_state_request_replan(json_value_t *state, const char *reason) {
  size_t current_count = 0;
  size_t max_count = 0;

  current_count = turbo_openai_agent_state_replan_count(state);
  max_count = turbo_openai_agent_state_replan_limit(state);
  return turbo_openai_state_append_replan_version(state, 1, current_count + 1, max_count, reason);
}

int turbo_openai_agent_state_replan_requested(const json_value_t *state) {
  return turbo_openai_state_current_version_bool_field(state, "replan_versions", "requested", 0);
}

int turbo_openai_agent_state_set_replan_limit(json_value_t *state, size_t max_replans) {
  const char *reason;
  size_t count;
  int requested;

  reason = turbo_openai_agent_state_replan_reason(state);
  count = turbo_openai_agent_state_replan_count(state);
  requested = turbo_openai_agent_state_replan_requested(state);
  return turbo_openai_state_append_replan_version(state, requested, count, max_replans, reason);
}

size_t turbo_openai_agent_state_replan_count(const json_value_t *state) {
  return turbo_openai_state_current_version_size_field(state, "replan_versions", "count");
}

size_t turbo_openai_agent_state_replan_limit(const json_value_t *state) {
  return turbo_openai_state_current_version_size_field(state, "replan_versions", "max_count");
}

const char *turbo_openai_agent_state_replan_reason(const json_value_t *state) {
  return turbo_openai_state_current_version_string_field(state, "replan_versions", "reason");
}

int turbo_openai_agent_state_set_guardrail_rejection(json_value_t *state, const char *phase,
                                                     const char *reason) {
  return turbo_openai_state_append_two_string_object_version(
      state, "guardrail_versions", "phase", phase, "reason", reason);
}

const char *turbo_openai_agent_state_guardrail_rejection_phase(const json_value_t *state) {
  const char *phase =
      turbo_openai_state_current_version_string_field(state, "guardrail_versions", "phase");
  return phase && phase[0] != '\0' ? phase : NULL;
}

const char *turbo_openai_agent_state_guardrail_rejection_reason(const json_value_t *state) {
  const char *reason =
      turbo_openai_state_current_version_string_field(state, "guardrail_versions", "reason");
  return reason && reason[0] != '\0' ? reason : NULL;
}

int turbo_openai_agent_state_set_model_error(json_value_t *state, const char *phase,
                                             const char *detail) {
  return turbo_openai_state_append_two_string_object_version(
      state, "model_error_versions", "phase", phase, "detail", detail);
}

const char *turbo_openai_agent_state_model_error_phase(const json_value_t *state) {
  const char *phase =
      turbo_openai_state_current_version_string_field(state, "model_error_versions", "phase");
  return phase && phase[0] != '\0' ? phase : NULL;
}

const char *turbo_openai_agent_state_model_error_detail(const json_value_t *state) {
  const char *detail =
      turbo_openai_state_current_version_string_field(state, "model_error_versions", "detail");
  return detail && detail[0] != '\0' ? detail : NULL;
}

int turbo_openai_agent_state_set_failure(json_value_t *state, const char *kind, const char *reason) {
  return turbo_openai_state_append_two_string_object_version(
      state, "failure_versions", "kind", kind, "reason", reason);
}

const char *turbo_openai_agent_state_failure_kind(const json_value_t *state) {
  return turbo_openai_state_current_version_string_field(state, "failure_versions", "kind");
}

const char *turbo_openai_agent_state_failure_reason(const json_value_t *state) {
  return turbo_openai_state_current_version_string_field(state, "failure_versions", "reason");
}

json_value_t *turbo_openai_agent_state_control_snapshot(const json_value_t *state) {
  json_value_t *snapshot;
  json_value_t *replan;
  json_value_t *review;
  json_value_t *failure;
  json_value_t *model_error;
  json_value_t *guardrail;

  if (!state) {
    return NULL;
  }

  snapshot = turbo_json_create_object();
  replan = turbo_json_create_object();
  review = turbo_json_create_object();
  failure = turbo_json_create_object();
  model_error = turbo_json_create_object();
  guardrail = turbo_json_create_object();
  if (!snapshot || !replan || !review || !failure || !model_error || !guardrail) {
    turbo_free_json(&snapshot);
    turbo_free_json(&replan);
    turbo_free_json(&review);
    turbo_free_json(&failure);
    turbo_free_json(&model_error);
    turbo_free_json(&guardrail);
    return NULL;
  }

  turbo_json_object_set_number(snapshot, "state_version",
                               (double)turbo_openai_agent_state_version(state));

  turbo_json_object_set_bool(replan, "requested",
                             turbo_openai_agent_state_replan_requested(state) ? true : false);
  turbo_json_object_set_number(replan, "count",
                               (double)turbo_openai_agent_state_replan_count(state));
  turbo_json_object_set_number(replan, "limit",
                               (double)turbo_openai_agent_state_replan_limit(state));
  turbo_json_object_set_string(replan, "reason",
                               turbo_openai_agent_state_replan_reason(state)
                                   ? turbo_openai_agent_state_replan_reason(state)
                                   : "");

  turbo_json_object_set_bool(review, "required",
                             turbo_openai_agent_state_review_required(state) ? true : false);
  turbo_json_object_set_bool(review, "approved",
                             turbo_openai_agent_state_review_approved(state) ? true : false);
  turbo_json_object_set_string(review, "note",
                               turbo_openai_agent_state_review_note(state)
                                   ? turbo_openai_agent_state_review_note(state)
                                   : "");

  turbo_json_object_set_string(failure, "kind",
                               turbo_openai_agent_state_failure_kind(state)
                                   ? turbo_openai_agent_state_failure_kind(state)
                                   : "");
  turbo_json_object_set_string(failure, "reason",
                               turbo_openai_agent_state_failure_reason(state)
                                   ? turbo_openai_agent_state_failure_reason(state)
                                   : "");

  turbo_json_object_set_string(model_error, "phase",
                               turbo_openai_agent_state_model_error_phase(state)
                                   ? turbo_openai_agent_state_model_error_phase(state)
                                   : "");
  turbo_json_object_set_string(model_error, "detail",
                               turbo_openai_agent_state_model_error_detail(state)
                                   ? turbo_openai_agent_state_model_error_detail(state)
                                   : "");

  turbo_json_object_set_string(guardrail, "phase",
                               turbo_openai_agent_state_guardrail_rejection_phase(state)
                                   ? turbo_openai_agent_state_guardrail_rejection_phase(state)
                                   : "");
  turbo_json_object_set_string(guardrail, "reason",
                               turbo_openai_agent_state_guardrail_rejection_reason(state)
                                   ? turbo_openai_agent_state_guardrail_rejection_reason(state)
                                   : "");

  turbo_json_object_add(snapshot, "replan", replan);
  turbo_json_object_add(snapshot, "review", review);
  turbo_json_object_add(snapshot, "failure", failure);
  turbo_json_object_add(snapshot, "model_error", model_error);
  turbo_json_object_add(snapshot, "guardrail", guardrail);
  return snapshot;
}

turbo_runtime_data_bind_value_t *
turbo_openai_agent_state_control_snapshot_bind(const turbo_runtime_data_bind_value_t *state) {
  json_value_t *json_state = turbo_openai_agent_state_bind_to_json_object(state);
  json_value_t *snapshot;
  turbo_runtime_data_bind_value_t *bound;

  if (!json_state) {
    return NULL;
  }

  snapshot = turbo_openai_agent_state_control_snapshot(json_state);
  turbo_free_json(&json_state);
  if (!snapshot) {
    return NULL;
  }

  bound = turbo_runtime_data_bind_value_from_json(snapshot);
  turbo_free_json(&snapshot);
  return bound;
}

json_value_t *turbo_openai_agent_state_workflow_snapshot(const json_value_t *state) {
  json_value_t *snapshot;
  json_value_t *control = NULL;
  json_value_t *plan_clone = NULL;
  json_value_t *completed_steps_clone = NULL;
  json_value_t *events_clone = NULL;
  json_value_t *trace_events_clone = NULL;
  json_value_t *planner_versions_clone = NULL;
  json_value_t *executor_versions_clone = NULL;
  json_value_t *memory_context_clone = NULL;
  char *memory_context_text = NULL;

  if (!state) {
    return NULL;
  }

  snapshot = turbo_json_create_object();
  if (!snapshot) {
    return NULL;
  }

  control = turbo_openai_agent_state_control_snapshot(state);
  if (!control) {
    turbo_free_json(&snapshot);
    return NULL;
  }

  if (turbo_openai_agent_state_plan(state) &&
      turbo_openai_clone_json(turbo_openai_agent_state_plan(state), &plan_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&control);
    turbo_free_json(&snapshot);
    return NULL;
  }
  if (turbo_openai_agent_state_completed_steps(state) &&
      turbo_openai_clone_json(turbo_openai_agent_state_completed_steps(state), &completed_steps_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&plan_clone);
    turbo_free_json(&control);
    turbo_free_json(&snapshot);
    return NULL;
  }
  if (turbo_openai_agent_state_events(state) &&
      turbo_openai_clone_json(turbo_openai_agent_state_events(state), &events_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&completed_steps_clone);
    turbo_free_json(&plan_clone);
    turbo_free_json(&control);
    turbo_free_json(&snapshot);
    return NULL;
  }
  if (turbo_openai_agent_state_trace_events(state) &&
      turbo_openai_clone_json(turbo_openai_agent_state_trace_events(state), &trace_events_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&events_clone);
    turbo_free_json(&completed_steps_clone);
    turbo_free_json(&plan_clone);
    turbo_free_json(&control);
    turbo_free_json(&snapshot);
    return NULL;
  }
  if (turbo_openai_agent_state_planner_event_versions(state) &&
      turbo_openai_clone_json(turbo_openai_agent_state_planner_event_versions(state),
                              &planner_versions_clone) != TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&trace_events_clone);
    turbo_free_json(&events_clone);
    turbo_free_json(&completed_steps_clone);
    turbo_free_json(&plan_clone);
    turbo_free_json(&control);
    turbo_free_json(&snapshot);
    return NULL;
  }
  if (turbo_openai_agent_state_executor_event_versions(state) &&
      turbo_openai_clone_json(turbo_openai_agent_state_executor_event_versions(state),
                              &executor_versions_clone) != TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&planner_versions_clone);
    turbo_free_json(&trace_events_clone);
    turbo_free_json(&events_clone);
    turbo_free_json(&completed_steps_clone);
    turbo_free_json(&plan_clone);
    turbo_free_json(&control);
    turbo_free_json(&snapshot);
    return NULL;
  }
  if (turbo_openai_agent_state_memory_context(state) &&
      turbo_openai_clone_json(turbo_openai_agent_state_memory_context(state), &memory_context_clone) !=
          TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&executor_versions_clone);
    turbo_free_json(&planner_versions_clone);
    turbo_free_json(&trace_events_clone);
    turbo_free_json(&events_clone);
    turbo_free_json(&completed_steps_clone);
    turbo_free_json(&plan_clone);
    turbo_free_json(&control);
    turbo_free_json(&snapshot);
    return NULL;
  }

  memory_context_text = turbo_openai_agent_memory_context_text_internal(state);
  if (turbo_openai_agent_state_memory_layer_count(state) > 0 && !memory_context_text) {
    turbo_free_json(&memory_context_clone);
    turbo_free_json(&executor_versions_clone);
    turbo_free_json(&planner_versions_clone);
    turbo_free_json(&trace_events_clone);
    turbo_free_json(&events_clone);
    turbo_free_json(&completed_steps_clone);
    turbo_free_json(&plan_clone);
    turbo_free_json(&control);
    turbo_free_json(&snapshot);
    return NULL;
  }
  if (memory_context_clone && memory_context_text) {
    turbo_json_object_set_string(memory_context_clone, "merged_text", memory_context_text);
  }

  turbo_json_object_set_number(snapshot, "state_version",
                               (double)turbo_openai_agent_state_version(state));
  turbo_json_object_set_bool(snapshot, "state_version_supported",
                             turbo_openai_agent_state_version_supported(state) ? true : false);
  turbo_json_object_add(snapshot, "control", control);
  if (plan_clone) {
    turbo_json_object_add(snapshot, "plan", plan_clone);
  }
  if (completed_steps_clone) {
    turbo_json_object_add(snapshot, "completed_steps", completed_steps_clone);
  }
  if (events_clone) {
    turbo_json_object_add(snapshot, "events", events_clone);
  }
  if (trace_events_clone) {
    turbo_json_object_add(snapshot, "trace_events", trace_events_clone);
  }
  if (planner_versions_clone) {
    turbo_json_object_add(snapshot, "planner_event_versions", planner_versions_clone);
  }
  if (executor_versions_clone) {
    turbo_json_object_add(snapshot, "executor_event_versions", executor_versions_clone);
  }
  if (memory_context_clone) {
    turbo_json_object_add(snapshot, "memory_context", memory_context_clone);
  }
  free(memory_context_text);
  return snapshot;
}

turbo_runtime_data_bind_value_t *
turbo_openai_agent_state_workflow_snapshot_bind(const turbo_runtime_data_bind_value_t *state) {
  json_value_t *json_state = turbo_openai_agent_state_bind_to_json_object(state);
  json_value_t *snapshot;
  turbo_runtime_data_bind_value_t *bound;

  if (!json_state) {
    return NULL;
  }

  snapshot = turbo_openai_agent_state_workflow_snapshot(json_state);
  turbo_free_json(&json_state);
  if (!snapshot) {
    return NULL;
  }

  bound = turbo_runtime_data_bind_value_from_json(snapshot);
  turbo_free_json(&snapshot);
  return bound;
}

int turbo_openai_agent_state_set_final_answer(json_value_t *state, const char *text) {
  return turbo_openai_state_append_single_string_object_version(state, "final_answer_versions",
                                                                "text", text);
}

int turbo_openai_agent_state_request_review(json_value_t *state, const char *note) {
  return turbo_openai_state_append_review_version(state, 1, 0, note);
}

int turbo_openai_agent_state_set_review_approved(json_value_t *state, int approved) {
  const char *note;

  note = turbo_openai_agent_state_review_note(state);
  return turbo_openai_state_append_review_version(state, approved ? 0 : 1, approved ? 1 : 0,
                                                  note);
}

int turbo_openai_agent_state_review_required(const json_value_t *state) {
  return turbo_openai_state_current_version_bool_field(state, "review_versions", "required", 0);
}

int turbo_openai_agent_state_review_approved(const json_value_t *state) {
  return turbo_openai_state_current_version_bool_field(state, "review_versions", "approved", 0);
}

const char *turbo_openai_agent_state_review_note(const json_value_t *state) {
  return turbo_openai_state_current_version_string_field(state, "review_versions", "note");
}

const char *turbo_openai_agent_state_last_output_text(const json_value_t *state) {
  const json_value_t *event = turbo_openai_state_last_event_of_kind(state, "model");
  return turbo_openai_event_output_text(event);
}

static int turbo_openai_agent_copy_model_error(json_value_t *dst_state,
                                               const json_value_t *src_state) {
  if (!turbo_openai_agent_state_model_error_phase(src_state) &&
      !turbo_openai_agent_state_model_error_detail(src_state)) {
    return 0;
  }

  return turbo_openai_agent_state_set_model_error(
      dst_state, turbo_openai_agent_state_model_error_phase(src_state),
      turbo_openai_agent_state_model_error_detail(src_state));
}

static int turbo_openai_agent_copy_guardrail_rejection(json_value_t *dst_state,
                                                       const json_value_t *src_state) {
  if (!turbo_openai_agent_state_guardrail_rejection_phase(src_state) &&
      !turbo_openai_agent_state_guardrail_rejection_reason(src_state)) {
    return 0;
  }

  return turbo_openai_agent_state_set_guardrail_rejection(
      dst_state, turbo_openai_agent_state_guardrail_rejection_phase(src_state),
      turbo_openai_agent_state_guardrail_rejection_reason(src_state));
}

size_t turbo_openai_agent_state_pending_tool_calls(const json_value_t *state) {
  const json_value_t *tool_calls = turbo_openai_last_model_tool_calls(state);
  if (!tool_calls || turbo_json_type(tool_calls) != TURBO_JSON_ARRAY) {
    return 0;
  }

  return turbo_json_array_size(tool_calls);
}

int turbo_openai_agent_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_openai_agent_t *agent = (turbo_openai_agent_t *)user_data;
  json_value_t *attempt_state = NULL;
  char *request_json = NULL;
  char *response_json = NULL;
  json_value_t *response = NULL;
  char *detail = NULL;
  char *structured_reason = NULL;
  char *guardrail_reason = NULL;
  int rc;
  size_t attempt = 0;

  if (!ctx || !ctx->state || !agent || !agent->transport_fn) {
    return -1;
  }

  attempt_state = ctx->state;
  for (attempt = 0;; ++attempt) {
    turbo_openai_agent_clear_last_stream_sse(agent);
    if (turbo_openai_build_turn_request(agent, attempt_state, &request_json) != 0) {
      turbo_openai_agent_state_set_model_error(ctx->state, "build_request",
                                               "failed to build model request");
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    if (turbo_openai_agent_invoke_before_model_middlewares(agent, attempt_state, &request_json) !=
        0) {
      turbo_openai_agent_state_set_model_error(ctx->state, "middleware",
                                               "before_model middleware failed");
      turbo_json_serialize_free(request_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }
    if (turbo_openai_agent_invoke_before_model_guardrails(agent, attempt_state, request_json,
                                                          &guardrail_reason) != 0) {
      turbo_openai_agent_state_set_guardrail_rejection(
          ctx->state, "before_model",
          guardrail_reason ? guardrail_reason : "before_model guardrail rejected request");
      turbo_openai_agent_emit_trace(agent, attempt_state,
                                    TURBO_OPENAI_AGENT_TRACE_GUARDRAIL_REJECTED, "before_model",
                                    guardrail_reason ? guardrail_reason : "guardrail rejected request",
                                    request_json, -1);
      turbo_openai_agent_state_set_model_error(ctx->state, "guardrail",
                                               guardrail_reason ? guardrail_reason
                                                                : "before_model guardrail rejected request");
      free(guardrail_reason);
      turbo_json_serialize_free(request_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    turbo_openai_agent_emit_trace(agent, attempt_state, TURBO_OPENAI_AGENT_TRACE_MODEL_REQUEST,
                                  agent->model, agent->provider ? agent->provider->name : NULL,
                                  request_json, (int)attempt);
    rc = agent->transport_fn(request_json, &response_json, agent->transport_user_data);
    if (turbo_openai_agent_invoke_after_model_middlewares(agent, attempt_state, request_json,
                                                          &response_json, rc) != 0) {
      turbo_openai_agent_state_set_model_error(ctx->state, "middleware",
                                               "after_model middleware failed");
      turbo_json_serialize_free(request_json);
      free(response_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }
    turbo_openai_agent_emit_trace(agent, attempt_state, TURBO_OPENAI_AGENT_TRACE_MODEL_RESPONSE,
                                  agent->model, agent->provider ? agent->provider->name : NULL,
                                  response_json, rc);
    turbo_json_serialize_free(request_json);
    request_json = NULL;
    if (rc != 0 || !response_json) {
      detail = response_json && response_json[0] != '\0'
                   ? turbo_openai_strdup(response_json)
                   : turbo_openai_strdup("model request transport failed");
      turbo_openai_agent_state_set_model_error(
          ctx->state, "transport", detail ? detail : "model request transport failed");
      free(detail);
      free(response_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    if (turbo_parse_json((const uint8_t *)response_json, strlen(response_json), &response) != 0) {
      detail = turbo_openai_strdup(response_json);
      turbo_openai_agent_state_set_model_error(
          ctx->state, "parse_response",
          detail ? detail : "failed to parse model response JSON");
      free(detail);
      free(response_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    rc = turbo_openai_append_model_event(agent, attempt_state, response);
    if (rc == 0 &&
        turbo_openai_agent_invoke_after_model_guardrails(agent, attempt_state, response_json, response,
                                                         &guardrail_reason) != 0) {
      turbo_openai_agent_state_set_guardrail_rejection(
          ctx->state, "after_model",
          guardrail_reason ? guardrail_reason : "after_model guardrail rejected response");
      turbo_openai_agent_emit_trace(agent, attempt_state,
                                    TURBO_OPENAI_AGENT_TRACE_GUARDRAIL_REJECTED, "after_model",
                                    guardrail_reason ? guardrail_reason
                                                     : "guardrail rejected response",
                                    response_json, -1);
      rc = -1;
    }
    free(response_json);
    response_json = NULL;
    turbo_free_json(&response);
    response = NULL;
    if (rc != 0) {
      if (guardrail_reason) {
        turbo_openai_agent_state_set_model_error(ctx->state, "guardrail", guardrail_reason);
        free(guardrail_reason);
      } else {
        turbo_openai_agent_state_set_model_error(ctx->state, "append_model_event",
                                                 "failed to append model event to state");
      }
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    free(structured_reason);
    structured_reason = NULL;
    if (turbo_openai_agent_structured_output_valid_for_state(agent, attempt_state,
                                                             &structured_reason)) {
      if (attempt_state != ctx->state) {
        const json_value_t *event = turbo_openai_state_last_event_of_kind(attempt_state, "model");
        if (!event || turbo_openai_append_event_clone(ctx->state, event) != 0) {
          turbo_free_json(&attempt_state);
          free(structured_reason);
          turbo_openai_agent_state_set_model_error(ctx->state, "append_model_event",
                                                   "failed to append validated retry event");
          return -1;
        }
        turbo_free_json(&attempt_state);
      }
      free(structured_reason);
      turbo_openai_agent_state_set_model_error(ctx->state, "", "");
      turbo_openai_agent_state_set_guardrail_rejection(ctx->state, "", "");
      return 0;
    }

    if (!agent->structured_output_schema_json || agent->structured_output_max_retries == 0 ||
        attempt >= agent->structured_output_max_retries) {
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      turbo_openai_agent_state_set_model_error(ctx->state, "structured_output",
                                               structured_reason && structured_reason[0] != '\0'
                                                   ? structured_reason
                                                   : "model output did not satisfy structured output schema");
      free(structured_reason);
      return -1;
    }

    turbo_openai_agent_emit_trace(agent, ctx->state, TURBO_OPENAI_AGENT_TRACE_STRUCTURED_RETRY,
                                  agent->structured_output_name ? agent->structured_output_name
                                                                : "structured_output",
                                  structured_reason ? structured_reason : "schema_validation_failed",
                                  NULL, (int)(attempt + 1));
    if (attempt_state != ctx->state) {
      turbo_free_json(&attempt_state);
    }
    attempt_state =
        turbo_openai_agent_build_structured_retry_state(ctx->state, attempt, structured_reason);
    free(structured_reason);
    structured_reason = NULL;
    if (!attempt_state) {
      turbo_openai_agent_state_set_model_error(ctx->state, "structured_output",
                                               "failed to prepare structured output retry");
      return -1;
    }
  }
}

int turbo_openai_agent_planner_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_openai_agent_t *agent = (turbo_openai_agent_t *)user_data;
  json_value_t *planner_state;
  turbo_graph_exec_ctx_t temp_ctx = {0};
  int rc;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  planner_state = turbo_openai_state_ensure_versioned_substate(
      ctx->state, "planner_state_versions", "planner_input_versions", "planner_event_versions");
  if (!planner_state) {
    return turbo_openai_agent_model_node(ctx, user_data);
  }

  temp_ctx.graph = ctx->graph;
  temp_ctx.state = planner_state;
  rc = turbo_openai_agent_model_node(&temp_ctx, agent);
  if (rc == 0) {
    rc = turbo_openai_append_last_substate_event(ctx->state, "planner_event_versions",
                                                 planner_state);
  } else {
    turbo_openai_agent_copy_model_error(ctx->state, planner_state);
  }

  return rc;
}

int turbo_openai_agent_tool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_openai_agent_t *agent = (turbo_openai_agent_t *)user_data;
  const json_value_t *tool_calls;
  json_value_t *event;
  json_value_t *outputs;
  size_t i;
  char *guardrail_reason = NULL;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  tool_calls = turbo_openai_last_model_tool_calls(ctx->state);
  if (!tool_calls || turbo_json_type(tool_calls) != TURBO_JSON_ARRAY) {
    turbo_openai_agent_state_set_model_error(ctx->state, "tool", "no pending tool calls");
    return -1;
  }

  event = turbo_openai_event_create("tool_results");
  outputs = turbo_json_create_array();
  if (!event || !outputs) {
    turbo_openai_agent_state_set_model_error(ctx->state, "tool",
                                             "failed to allocate tool result containers");
    turbo_free_json(&event);
    turbo_free_json(&outputs);
    return -1;
  }

  for (i = 0; i < turbo_json_array_size(tool_calls); ++i) {
    const json_value_t *call = turbo_json_array_get(tool_calls, i);
    const char *call_id;
    const char *name;
    const char *arguments_json;
    char *mutable_arguments_json = NULL;
    char *output = NULL;
    turbo_tool_status_t status;
    json_value_t *output_item;

    if (!turbo_openai_tool_call_record_fields(call, &call_id, &name, &arguments_json)) {
      continue;
    }

    mutable_arguments_json = turbo_openai_strdup(arguments_json ? arguments_json : "{}");
    if (!mutable_arguments_json) {
      turbo_openai_agent_state_set_model_error(ctx->state, "tool",
                                               "failed to allocate tool arguments buffer");
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    if (turbo_openai_agent_invoke_before_tool_middlewares(agent, ctx->state, call_id, name,
                                                          &mutable_arguments_json) != 0) {
      turbo_openai_agent_state_set_model_error(ctx->state, "middleware",
                                               "before_tool middleware failed");
      free(mutable_arguments_json);
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    if (turbo_openai_agent_invoke_before_tool_guardrails(agent, ctx->state, call_id, name,
                                                         mutable_arguments_json,
                                                         &guardrail_reason) != 0) {
      turbo_openai_agent_state_set_guardrail_rejection(
          ctx->state, "before_tool",
          guardrail_reason ? guardrail_reason : "before_tool guardrail rejected tool dispatch");
      turbo_openai_agent_emit_trace(agent, ctx->state,
                                    TURBO_OPENAI_AGENT_TRACE_GUARDRAIL_REJECTED, "before_tool",
                                    guardrail_reason ? guardrail_reason
                                                     : "guardrail rejected tool dispatch",
                                    mutable_arguments_json, -1);
      output = turbo_openai_format_tool_error(name, guardrail_reason ? guardrail_reason
                                                                     : "tool_guardrail_rejected");
      free(guardrail_reason);
      guardrail_reason = NULL;
      status = TURBO_TOOL_ERROR;
    } else {
      turbo_openai_agent_emit_trace(agent, ctx->state, TURBO_OPENAI_AGENT_TRACE_TOOL_DISPATCH,
                                    name, call_id, mutable_arguments_json, 0);
      status = turbo_tool_registry_execute(agent->tool_registry, name, mutable_arguments_json,
                                           &output);
      if (status != TURBO_TOOL_OK || !output) {
        output = turbo_openai_format_tool_error(
            name, status == TURBO_TOOL_NOT_FOUND ? "tool_not_found" : "tool_execution_failed");
        if (!output) {
          turbo_openai_agent_state_set_model_error(ctx->state, "tool",
                                                   "failed to format tool error payload");
          free(mutable_arguments_json);
          turbo_free_json(&event);
          turbo_free_json(&outputs);
          return -1;
        }
      }
    }
    if (turbo_openai_agent_invoke_after_tool_middlewares(agent, ctx->state, call_id, name,
                                                         mutable_arguments_json, &output,
                                                         status) != 0) {
      turbo_openai_agent_state_set_model_error(ctx->state, "middleware",
                                               "after_tool middleware failed");
      free(mutable_arguments_json);
      free(output);
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    if (turbo_openai_agent_invoke_after_tool_guardrails(agent, ctx->state, call_id, name,
                                                        mutable_arguments_json, output, status,
                                                        &guardrail_reason) != 0) {
      turbo_openai_agent_state_set_guardrail_rejection(
          ctx->state, "after_tool",
          guardrail_reason ? guardrail_reason : "after_tool guardrail rejected tool output");
      turbo_openai_agent_emit_trace(agent, ctx->state,
                                    TURBO_OPENAI_AGENT_TRACE_GUARDRAIL_REJECTED, "after_tool",
                                    guardrail_reason ? guardrail_reason
                                                     : "guardrail rejected tool output",
                                    output, -1);
      char *guardrail_output =
          turbo_openai_format_tool_error(name, guardrail_reason ? guardrail_reason
                                                                : "tool_output_guardrail_rejected");
      free(guardrail_reason);
      guardrail_reason = NULL;
      if (!guardrail_output) {
        turbo_openai_agent_state_set_model_error(ctx->state, "guardrail",
                                                 "failed to format guardrail error payload");
        free(mutable_arguments_json);
        free(output);
        turbo_free_json(&event);
        turbo_free_json(&outputs);
        return -1;
      }
      free(output);
      output = guardrail_output;
      status = TURBO_TOOL_ERROR;
    }
    turbo_openai_agent_emit_trace(agent, ctx->state, TURBO_OPENAI_AGENT_TRACE_TOOL_RESULT, name,
                                  call_id, output, (int)status);

    output_item = turbo_openai_tool_result_output_item_create(call_id, output);
    if (!output_item) {
      turbo_openai_agent_state_set_model_error(ctx->state, "tool",
                                               "failed to record tool output");
      free(mutable_arguments_json);
      free(output);
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    turbo_json_array_add(outputs, output_item);
    free(mutable_arguments_json);
    free(output);
  }

  turbo_json_object_add(event, "outputs", outputs);
  turbo_openai_agent_state_set_guardrail_rejection(ctx->state, "", "");
  return turbo_openai_append_event(ctx->state, event);
}

int turbo_openai_agent_plan_commit_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *output_text;

  (void)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  output_text = turbo_openai_agent_state_last_output_text(ctx->state);
  if (!output_text || output_text[0] == '\0') {
    return -1;
  }

  return turbo_openai_agent_state_set_plan_from_json(ctx->state, output_text);
}

int turbo_openai_agent_replan_prepare_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_openai_agent_t *agent = (turbo_openai_agent_t *)user_data;
  const char *step_text;
  const char *reason;
  const char *latest_executor_text;
  char *completed_steps_message = NULL;
  size_t replan_count;
  size_t replan_limit;
  int needed;
  char *prompt;
  json_value_t *planner_input;
  json_value_t *planner_events;
  json_value_t *planner_history;
  json_value_t *planner_state;
  const json_value_t *base_input;
  json_value_t *message;

  if (!ctx || !ctx->state || !turbo_openai_agent_state_replan_requested(ctx->state)) {
    return -1;
  }

  step_text = turbo_openai_agent_state_current_plan_step_text(ctx->state);
  reason = turbo_openai_agent_state_replan_reason(ctx->state);
  latest_executor_text = turbo_openai_agent_state_latest_executor_output_text(ctx->state);
  completed_steps_message = turbo_openai_build_completed_steps_message(ctx->state);
  replan_count = turbo_openai_agent_state_replan_count(ctx->state);
  replan_limit = turbo_openai_agent_state_replan_limit(ctx->state);
  turbo_openai_agent_emit_trace(agent, ctx->state, TURBO_OPENAI_AGENT_TRACE_REPLAN_REQUESTED,
                                "replan", reason, latest_executor_text, (int)replan_count);
  needed = snprintf(
      NULL, 0,
      "Replan from the current failure. Failed step: %s. Failure reason: %s. "
      "Latest executor output: %s. %sReturn only JSON with a steps array. "
      "Avoid repeating the same failed attempt.",
      step_text && step_text[0] != '\0' ? step_text : "unknown",
      reason && reason[0] != '\0' ? reason : "unspecified",
      latest_executor_text && latest_executor_text[0] != '\0' ? latest_executor_text : "none",
      completed_steps_message ? completed_steps_message : "");
  if (needed < 0) {
    free(completed_steps_message);
    return -1;
  }

  prompt = (char *)malloc((size_t)needed + 1);
  if (!prompt) {
    free(completed_steps_message);
    return -1;
  }

  snprintf(prompt, (size_t)needed + 1,
           "Replan from the current failure. Failed step: %s. Failure reason: %s. "
           "Latest executor output: %s. %sReturn only JSON with a steps array. "
           "Avoid repeating the same failed attempt.",
           step_text && step_text[0] != '\0' ? step_text : "unknown",
           reason && reason[0] != '\0' ? reason : "unspecified",
           latest_executor_text && latest_executor_text[0] != '\0' ? latest_executor_text : "none",
           completed_steps_message ? completed_steps_message : "");
  free(completed_steps_message);
  base_input = turbo_openai_state_get_array_const(ctx->state, "input");
  if (!base_input) {
    free(prompt);
    return -1;
  }

  planner_input = NULL;
  if (turbo_openai_clone_json(base_input, &planner_input) != TURBO_GRAPH_EXEC_OK) {
    free(prompt);
    return -1;
  }

  planner_events = turbo_json_create_array();
  planner_history = turbo_json_create_array();
  planner_state = NULL;
  message = turbo_prompt_message_create("user", prompt);
  if (!message || !planner_events || !planner_history) {
    free(prompt);
    turbo_free_json(&planner_input);
    turbo_free_json(&message);
    turbo_free_json(&planner_events);
    turbo_free_json(&planner_history);
    return -1;
  }

  free(prompt);
  turbo_json_array_add(planner_input, message);
  if (turbo_openai_state_append_array_version(ctx->state, "planner_event_versions", planner_history) !=
      0) {
    turbo_free_json(&planner_input);
    turbo_free_json(&planner_events);
    turbo_free_json(&planner_history);
    return -1;
  }

  planner_state = turbo_openai_substate_create(planner_input, planner_events);
  if (!planner_state ||
      turbo_openai_state_append_object_version(ctx->state, "planner_state_versions", planner_state) !=
          0) {
    turbo_free_json(&planner_input);
    turbo_free_json(&planner_events);
    return -1;
  }

  return turbo_openai_state_append_replan_version(ctx->state, 0, replan_count, replan_limit, "");
}

int turbo_openai_agent_has_pending_tool_calls(const turbo_graph_exec_ctx_t *ctx,
                                              void *user_data) {
  (void)user_data;
  return ctx && ctx->state && turbo_openai_agent_state_pending_tool_calls(ctx->state) > 0;
}

int turbo_openai_agent_should_replan(const turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  return ctx && ctx->state ? turbo_openai_agent_state_replan_requested(ctx->state) : 0;
}

int turbo_openai_agent_replan_route_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *replan_node_name = (const char *)user_data;

  if (!ctx || !ctx->state || !replan_node_name) {
    return -1;
  }

  if (!turbo_openai_agent_state_replan_requested(ctx->state)) {
    return 0;
  }

  return turbo_graph_ctx_set_next(ctx, replan_node_name);
}

int turbo_openai_agent_review_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_openai_agent_t *agent = (turbo_openai_agent_t *)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  if (turbo_openai_agent_state_review_required(ctx->state) &&
      !turbo_openai_agent_state_review_approved(ctx->state)) {
    turbo_openai_agent_emit_trace(agent, ctx->state, TURBO_OPENAI_AGENT_TRACE_REVIEW_REQUIRED,
                                  "review", turbo_openai_agent_state_review_note(ctx->state), NULL,
                                  0);
    turbo_graph_ctx_stop(ctx);
  } else if (turbo_openai_agent_state_review_approved(ctx->state)) {
    turbo_openai_agent_emit_trace(agent, ctx->state, TURBO_OPENAI_AGENT_TRACE_REVIEW_APPROVED,
                                  "review", turbo_openai_agent_state_review_note(ctx->state), NULL,
                                  1);
  }
  return 0;
}

int turbo_openai_agent_review_reset_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *note;

  (void)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  note = turbo_openai_agent_state_review_note(ctx->state);
  if (!note) {
    return 0;
  }

  if (turbo_openai_agent_state_review_required(ctx->state) &&
      !turbo_openai_agent_state_review_approved(ctx->state)) {
    return 0;
  }

  return turbo_openai_agent_state_request_review(ctx->state, note);
}

int turbo_openai_agent_review_approved_predicate(const turbo_graph_exec_ctx_t *ctx,
                                                 void *user_data) {
  (void)user_data;
  return ctx && ctx->state ? turbo_openai_agent_state_review_approved(ctx->state) : 0;
}

static const json_value_t *
turbo_openai_agent_latest_executor_events(const json_value_t *state) {
  const json_value_t *versions;

  if (!state) {
    return NULL;
  }

  versions = turbo_json_object_get(state, "executor_event_versions");
  return turbo_openai_array_last_const(versions);
}

static const json_value_t *turbo_openai_tool_results_event_outputs(const json_value_t *event) {
  const char *kind;
  const json_value_t *outputs;

  kind = event ? turbo_json_get_string(event, "kind") : NULL;
  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT || !kind ||
      strcmp(kind, "tool_results") != 0) {
    return NULL;
  }

  outputs = turbo_json_object_get(event, "outputs");
  return outputs && turbo_json_type(outputs) == TURBO_JSON_ARRAY ? outputs : NULL;
}

static json_value_t *turbo_openai_tool_result_output_parse_json(const json_value_t *output_item) {
  const char *output_text;
  json_value_t *output_json = NULL;

  if (!turbo_openai_tool_result_output_fields(output_item, NULL, &output_text) ||
      output_text[0] == '\0') {
    return NULL;
  }

  if (turbo_parse_json((const uint8_t *)output_text, strlen(output_text), &output_json) != 0) {
    return NULL;
  }

  return output_json;
}

static int turbo_openai_tool_result_json_failed(const json_value_t *tool_json) {
  return tool_json && turbo_json_type(tool_json) == TURBO_JSON_OBJECT &&
                 !turbo_json_get_bool(tool_json, "ok", true)
             ? 1
             : 0;
}

static int turbo_openai_tool_result_json_like(const json_value_t *value) {
  return value && turbo_json_type(value) == TURBO_JSON_OBJECT &&
                 (turbo_json_object_get(value, "ok") || turbo_json_object_get(value, "summary")) &&
                 (turbo_json_object_get(value, "matches") ||
                  turbo_json_object_get(value, "content") ||
                  turbo_json_object_get(value, "stdout") ||
                  turbo_json_object_get(value, "stderr") ||
                  turbo_json_object_get(value, "changed_files") ||
                  turbo_json_object_get(value, "artifacts"))
             ? 1
             : 0;
}

static char *turbo_openai_tool_result_failure_reason(const json_value_t *tool_json) {
  const char *summary;
  const char *stderr_text;

  if (!turbo_openai_tool_result_json_failed(tool_json)) {
    return NULL;
  }

  summary = turbo_json_get_string(tool_json, "summary");
  if (summary && summary[0] != '\0') {
    return turbo_openai_strdup(summary);
  }

  stderr_text = turbo_json_get_string(tool_json, "stderr");
  if (stderr_text && stderr_text[0] != '\0') {
    return turbo_openai_strdup(stderr_text);
  }

  return turbo_openai_strdup("tool execution failed");
}

static int turbo_openai_tool_results_event_failed(const json_value_t *event) {
  const json_value_t *outputs;
  size_t j;

  outputs = turbo_openai_tool_results_event_outputs(event);
  if (!outputs) {
    return 0;
  }

  for (j = 0; j < turbo_json_array_size(outputs); ++j) {
    const json_value_t *output_item = turbo_json_array_get(outputs, j);
    json_value_t *output_json = turbo_openai_tool_result_output_parse_json(output_item);

    if (!output_json) {
      continue;
    }

    if (turbo_openai_tool_result_json_failed(output_json)) {
      turbo_free_json(&output_json);
      return 1;
    }

    turbo_free_json(&output_json);
  }

  return 0;
}

const char *turbo_openai_agent_state_latest_executor_output_text(const json_value_t *state) {
  const json_value_t *events = turbo_openai_agent_latest_executor_events(state);
  const json_value_t *event;

  event = turbo_openai_events_last_of_kind(events, "model");
  if (!event) {
    return NULL;
  }

  return turbo_openai_event_output_text(event);
}

const json_value_t *turbo_openai_agent_state_planner_event_versions(const json_value_t *state) {
  return state && turbo_json_type(state) == TURBO_JSON_OBJECT
             ? turbo_json_object_get(state, "planner_event_versions")
             : NULL;
}

size_t turbo_openai_agent_state_planner_event_version_count(const json_value_t *state) {
  const json_value_t *versions = turbo_openai_agent_state_planner_event_versions(state);
  return versions && turbo_json_type(versions) == TURBO_JSON_ARRAY ? turbo_json_array_size(versions)
                                                                   : 0;
}

const json_value_t *turbo_openai_agent_state_planner_event_version_at(const json_value_t *state,
                                                                      size_t index) {
  const json_value_t *versions = turbo_openai_agent_state_planner_event_versions(state);
  if (!versions || turbo_json_type(versions) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(versions)) {
    return NULL;
  }
  return turbo_json_array_get(versions, index);
}

turbo_runtime_data_bind_value_t *
turbo_openai_agent_state_planner_event_version_bind(const turbo_runtime_data_bind_value_t *state,
                                                    size_t index) {
  return turbo_openai_agent_state_array_version_bind(state, "planner_event_versions", index);
}

turbo_runtime_data_bind_value_t *
turbo_openai_agent_state_latest_planner_event_version_bind(
    const turbo_runtime_data_bind_value_t *state) {
  return turbo_openai_agent_state_latest_array_version_bind(state, "planner_event_versions");
}

const json_value_t *turbo_openai_agent_state_executor_event_versions(const json_value_t *state) {
  return state && turbo_json_type(state) == TURBO_JSON_OBJECT
             ? turbo_json_object_get(state, "executor_event_versions")
             : NULL;
}

size_t turbo_openai_agent_state_executor_event_version_count(const json_value_t *state) {
  const json_value_t *versions = turbo_openai_agent_state_executor_event_versions(state);
  return versions && turbo_json_type(versions) == TURBO_JSON_ARRAY ? turbo_json_array_size(versions)
                                                                   : 0;
}

const json_value_t *turbo_openai_agent_state_executor_event_version_at(const json_value_t *state,
                                                                       size_t index) {
  const json_value_t *versions = turbo_openai_agent_state_executor_event_versions(state);
  if (!versions || turbo_json_type(versions) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(versions)) {
    return NULL;
  }
  return turbo_json_array_get(versions, index);
}

turbo_runtime_data_bind_value_t *
turbo_openai_agent_state_executor_event_version_bind(const turbo_runtime_data_bind_value_t *state,
                                                     size_t index) {
  return turbo_openai_agent_state_array_version_bind(state, "executor_event_versions", index);
}

turbo_runtime_data_bind_value_t *
turbo_openai_agent_state_latest_executor_event_version_bind(
    const turbo_runtime_data_bind_value_t *state) {
  return turbo_openai_agent_state_latest_array_version_bind(state, "executor_event_versions");
}

const json_value_t *turbo_openai_agent_state_completed_steps(const json_value_t *state) {
  return turbo_openai_completed_steps_const(state);
}

size_t turbo_openai_agent_state_completed_step_count(const json_value_t *state) {
  const json_value_t *steps = turbo_openai_agent_state_completed_steps(state);
  return steps && turbo_json_type(steps) == TURBO_JSON_ARRAY ? turbo_json_array_size(steps) : 0;
}

const json_value_t *turbo_openai_agent_state_completed_step_at(const json_value_t *state,
                                                               size_t index) {
  const json_value_t *steps = turbo_openai_agent_state_completed_steps(state);
  if (!steps || turbo_json_type(steps) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(steps)) {
    return NULL;
  }
  return turbo_json_array_get(steps, index);
}

int turbo_openai_agent_state_executor_tool_results_failed(const json_value_t *state) {
  const json_value_t *events = turbo_openai_agent_latest_executor_events(state);
  const json_value_t *event;
  size_t i;

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY) {
    return 0;
  }

  event = turbo_openai_array_last_const(events);
  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return 0;
  }

  if (turbo_openai_event_kind_is(event, "tool_results")) {
    return turbo_openai_tool_results_event_failed(event);
  }

  if (!turbo_openai_event_kind_is(event, "model") ||
      !turbo_openai_agent_contains_failure_marker(turbo_openai_event_output_text(event))) {
    return 0;
  }

  for (i = turbo_json_array_size(events) - 1; i > 0; --i) {
    const json_value_t *previous = turbo_json_array_get(events, i - 1);
    if (!previous || turbo_json_type(previous) != TURBO_JSON_OBJECT) {
      continue;
    }

    if (turbo_openai_event_kind_is(previous, "tool_results")) {
      return turbo_openai_tool_results_event_failed(previous);
    }

    if (turbo_openai_event_kind_is(previous, "model")) {
      break;
    }
  }

  return 0;
}

static int turbo_openai_agent_output_looks_like_tool_result_json(const char *text) {
  json_value_t *value = NULL;
  int is_tool_result = 0;

  if (!text || text[0] != '{') {
    return 0;
  }

  if (turbo_parse_json((const uint8_t *)text, strlen(text), &value) != 0) {
    return 0;
  }

  if (turbo_openai_tool_result_json_like(value)) {
    is_tool_result = 1;
  }

  turbo_free_json(&value);
  return is_tool_result;
}

const char *turbo_openai_agent_state_final_answer_text(const json_value_t *state) {
  const char *executor_text = turbo_openai_agent_state_latest_executor_output_text(state);
  const char *last_text = turbo_openai_agent_state_last_output_text(state);
  const char *stored_text =
      turbo_openai_state_current_version_string_field(state, "final_answer_versions", "text");

  if (stored_text && stored_text[0] != '\0') {
    return stored_text;
  }

  if (executor_text && executor_text[0] != '\0' &&
      !turbo_openai_agent_output_looks_like_tool_result_json(executor_text)) {
    return executor_text;
  }

  if (last_text && last_text[0] != '\0' &&
      !turbo_openai_agent_output_looks_like_tool_result_json(last_text)) {
    return last_text;
  }

  return NULL;
}

int turbo_openai_agent_state_parse_final_output_json(const json_value_t *state,
                                                     json_value_t **out_json) {
  const char *text;

  if (!out_json) {
    return -1;
  }

  *out_json = NULL;
  text = turbo_openai_agent_state_final_answer_text(state);
  if (!text || text[0] == '\0') {
    return -1;
  }

  return turbo_parse_json((const uint8_t *)text, strlen(text), out_json) == 0 ? 0 : -1;
}

static char *turbo_openai_schema_reason(const char *message, const char *field_name) {
  int needed;
  char *buffer;

  needed = snprintf(NULL, 0, field_name && field_name[0] != '\0' ? "%s: %s" : "%s",
                    message ? message : "schema validation failed",
                    field_name && field_name[0] != '\0' ? field_name : "");
  if (needed < 0) {
    return NULL;
  }

  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }

  snprintf(buffer, (size_t)needed + 1, field_name && field_name[0] != '\0' ? "%s: %s" : "%s",
           message ? message : "schema validation failed",
           field_name && field_name[0] != '\0' ? field_name : "");
  return buffer;
}

static int turbo_openai_json_schema_type_matches(const json_value_t *value, const char *type_name) {
  if (!type_name || type_name[0] == '\0') {
    return 1;
  }

  if (strcmp(type_name, "object") == 0) {
    return turbo_json_type(value) == TURBO_JSON_OBJECT;
  }
  if (strcmp(type_name, "array") == 0) {
    return turbo_json_type(value) == TURBO_JSON_ARRAY;
  }
  if (strcmp(type_name, "string") == 0) {
    return turbo_json_type(value) == TURBO_JSON_STRING;
  }
  if (strcmp(type_name, "boolean") == 0) {
    return turbo_json_type(value) == TURBO_JSON_BOOL;
  }
  if (strcmp(type_name, "number") == 0) {
    return turbo_json_type(value) == TURBO_JSON_NUMBER;
  }
  if (strcmp(type_name, "integer") == 0) {
    double num;
    return turbo_json_type(value) == TURBO_JSON_NUMBER &&
                   ((num = turbo_json_number(value)), num == (double)((long long)num))
               ? 1
               : 0;
  }
  if (strcmp(type_name, "null") == 0) {
    return turbo_json_type(value) == TURBO_JSON_NULL;
  }

  return 1;
}

static int turbo_openai_json_schema_validate_type(const json_value_t *value,
                                                  const json_value_t *type_schema,
                                                  char **out_reason) {
  size_t i;

  if (!type_schema) {
    return 1;
  }

  if (turbo_json_type(type_schema) == TURBO_JSON_STRING) {
    if (turbo_openai_json_schema_type_matches(value, turbo_json_string(type_schema))) {
      return 1;
    }
    if (out_reason) {
      *out_reason = turbo_openai_schema_reason("type mismatch", turbo_json_string(type_schema));
    }
    return 0;
  }

  if (turbo_json_type(type_schema) != TURBO_JSON_ARRAY) {
    return 1;
  }

  for (i = 0; i < turbo_json_array_size(type_schema); ++i) {
    const json_value_t *entry = turbo_json_array_get(type_schema, i);
    if (entry && turbo_json_type(entry) == TURBO_JSON_STRING &&
        turbo_openai_json_schema_type_matches(value, turbo_json_string(entry))) {
      return 1;
    }
  }

  if (out_reason) {
    *out_reason = turbo_openai_schema_reason("type mismatch", "allowed types");
  }
  return 0;
}

static int turbo_openai_json_schema_validate(const json_value_t *value, const json_value_t *schema,
                                             char **out_reason) {
  const json_value_t *type_schema;
  const json_value_t *required;
  const json_value_t *properties;
  const json_value_t *items;
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }

  if (!schema || turbo_json_type(schema) != TURBO_JSON_OBJECT) {
    return 1;
  }

  type_schema = turbo_json_object_get(schema, "type");
  if (!turbo_openai_json_schema_validate_type(value, type_schema, out_reason)) {
    return 0;
  }

  required = turbo_json_object_get(schema, "required");
  if (required && turbo_json_type(required) == TURBO_JSON_ARRAY &&
      turbo_json_type(value) == TURBO_JSON_OBJECT) {
    for (i = 0; i < turbo_json_array_size(required); ++i) {
      const json_value_t *required_entry = turbo_json_array_get(required, i);
      const char *required_name;
      if (!required_entry || turbo_json_type(required_entry) != TURBO_JSON_STRING) {
        continue;
      }
      required_name = turbo_json_string(required_entry);
      if (!turbo_json_object_get(value, required_name)) {
        if (out_reason) {
          *out_reason = turbo_openai_schema_reason("missing required property", required_name);
        }
        return 0;
      }
    }
  }

  properties = turbo_json_object_get(schema, "properties");
  if (properties && turbo_json_type(properties) == TURBO_JSON_OBJECT &&
      turbo_json_type(value) == TURBO_JSON_OBJECT) {
    for (i = 0; i < turbo_json_object_size(properties); ++i) {
      const char *key = turbo_json_object_key(properties, i);
      const json_value_t *property_schema = turbo_json_object_value(properties, i);
      const json_value_t *property_value = key ? turbo_json_object_get(value, key) : NULL;
      char *child_reason = NULL;

      if (!key || !property_value) {
        continue;
      }
      if (!turbo_openai_json_schema_validate(property_value, property_schema, &child_reason)) {
        if (out_reason) {
          *out_reason = child_reason ? child_reason : turbo_openai_schema_reason("invalid property", key);
        } else {
          free(child_reason);
        }
        return 0;
      }
    }
  }

  if (turbo_json_type(value) == TURBO_JSON_OBJECT &&
      !turbo_json_get_bool(schema, "additionalProperties", true)) {
    for (i = 0; i < turbo_json_object_size(value); ++i) {
      const char *key = turbo_json_object_key(value, i);
      if (key && (!properties || turbo_json_type(properties) != TURBO_JSON_OBJECT ||
                  !turbo_json_object_get(properties, key))) {
        if (out_reason) {
          *out_reason = turbo_openai_schema_reason("unexpected property", key);
        }
        return 0;
      }
    }
  }

  items = turbo_json_object_get(schema, "items");
  if (items && turbo_json_type(value) == TURBO_JSON_ARRAY) {
    for (i = 0; i < turbo_json_array_size(value); ++i) {
      const json_value_t *item = turbo_json_array_get(value, i);
      char *child_reason = NULL;
      if (!turbo_openai_json_schema_validate(item, items, &child_reason)) {
        if (out_reason) {
          *out_reason = child_reason ? child_reason : turbo_openai_schema_reason("invalid array item", NULL);
        } else {
          free(child_reason);
        }
        return 0;
      }
    }
  }

  return 1;
}

static int turbo_openai_agent_structured_output_valid_for_state(
    const turbo_openai_agent_t *agent, const json_value_t *state, char **out_reason) {
  json_value_t *parsed = NULL;
  json_value_t *schema = NULL;
  int valid = 0;

  if (!agent) {
    return 0;
  }

  if (!agent->structured_output_schema_json || agent->structured_output_schema_json[0] == '\0') {
    if (out_reason) {
      *out_reason = NULL;
    }
    return 1;
  }

  if (turbo_openai_agent_state_parse_final_output_json(state, &parsed) != 0) {
    if (out_reason) {
      *out_reason = turbo_openai_strdup("response was not valid JSON");
    }
    return 0;
  }

  if (turbo_parse_json((const uint8_t *)agent->structured_output_schema_json,
                       strlen(agent->structured_output_schema_json), &schema) != 0) {
    turbo_free_json(&parsed);
    if (out_reason) {
      *out_reason = turbo_openai_strdup("failed to parse structured output schema");
    }
    return 0;
  }

  valid = turbo_openai_json_schema_validate(parsed, schema, out_reason);
  turbo_free_json(&schema);
  turbo_free_json(&parsed);
  return valid;
}

static json_value_t *turbo_openai_agent_build_structured_retry_state(const json_value_t *state,
                                                                     size_t attempt_index,
                                                                     const char *reason) {
  static const char *retry_prefix =
      "Your previous response did not satisfy the requested JSON schema. Return only valid JSON "
      "that matches the requested schema. Do not include markdown fences or extra prose.";
  json_value_t *retry_state = NULL;
  json_value_t *input = NULL;
  json_value_t *message = NULL;
  char *prompt = NULL;
  int needed;

  if (!state) {
    return NULL;
  }

  if (turbo_openai_clone_json(state, &retry_state) != TURBO_GRAPH_EXEC_OK) {
    return NULL;
  }

  input = turbo_openai_state_get_array(retry_state, "input");
  if (!input) {
    turbo_free_json(&retry_state);
    return NULL;
  }

  needed = snprintf(NULL, 0, reason && reason[0] != '\0' ? "%s Reason: %s. Retry attempt %lu."
                                                         : "%s Retry attempt %lu.",
                    retry_prefix, reason && reason[0] != '\0' ? reason : "",
                    (unsigned long)(attempt_index + 1));
  if (needed < 0) {
    turbo_free_json(&retry_state);
    return NULL;
  }

  prompt = (char *)malloc((size_t)needed + 1);
  if (!prompt) {
    turbo_free_json(&retry_state);
    return NULL;
  }

  snprintf(prompt, (size_t)needed + 1,
           reason && reason[0] != '\0' ? "%s Reason: %s. Retry attempt %lu."
                                       : "%s Retry attempt %lu.",
           retry_prefix, reason && reason[0] != '\0' ? reason : "",
           (unsigned long)(attempt_index + 1));
  message = turbo_prompt_message_create("user", prompt);
  free(prompt);
  if (!message) {
    turbo_free_json(&retry_state);
    return NULL;
  }

  turbo_json_array_add(input, message);
  return retry_state;
}

static char *turbo_openai_agent_executor_failure_reason(const json_value_t *state) {
  const json_value_t *events = turbo_openai_agent_latest_executor_events(state);
  const char *output_text = turbo_openai_agent_state_latest_executor_output_text(state);
  size_t i;

  if (events && turbo_json_type(events) == TURBO_JSON_ARRAY) {
    for (i = 0; i < turbo_json_array_size(events); ++i) {
      const json_value_t *event = turbo_json_array_get(events, i);
      const json_value_t *outputs;
      size_t j;

      outputs = turbo_openai_tool_results_event_outputs(event);
      if (!outputs) {
        continue;
      }

      for (j = 0; j < turbo_json_array_size(outputs); ++j) {
        const json_value_t *output_item = turbo_json_array_get(outputs, j);
        json_value_t *tool_json = turbo_openai_tool_result_output_parse_json(output_item);
        char *reason;

        if (!tool_json) {
          continue;
        }

        reason = turbo_openai_tool_result_failure_reason(tool_json);
        if (!reason) {
          turbo_free_json(&tool_json);
          continue;
        }

        turbo_free_json(&tool_json);
        return reason;
      }
    }
  }

  if (output_text && output_text[0] != '\0') {
    return turbo_openai_strdup(output_text);
  }

  return turbo_openai_strdup("step failed");
}

static int turbo_openai_starts_with_nocase(const char *text, const char *prefix) {
  size_t i;

  if (!text || !prefix) {
    return 0;
  }

  for (i = 0; prefix[i] != '\0'; ++i) {
    if (text[i] == '\0' ||
        tolower((unsigned char)text[i]) != tolower((unsigned char)prefix[i])) {
      return 0;
    }
  }

  return 1;
}

static int turbo_openai_agent_contains_failure_marker(const char *text) {
  const char *line_start;

  if (!text || text[0] == '\0') {
    return 0;
  }

  line_start = text;
  while (line_start && *line_start) {
    if (turbo_openai_starts_with_nocase(line_start, "FAILED:") ||
        turbo_openai_starts_with_nocase(line_start, "ERROR:") ||
        (turbo_openai_starts_with_nocase(line_start, "failed") &&
         (line_start[6] == '\0' || line_start[6] == '\n' || isspace((unsigned char)line_start[6])))) {
      return 1;
    }
    line_start = strchr(line_start, '\n');
    if (line_start) {
      ++line_start;
    }
  }

  return 0;
}

int turbo_openai_agent_detect_failed_step_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *output_text;
  char *failure_reason = NULL;
  const char *failure_kind = NULL;
  size_t replan_limit = 0;
  size_t replan_count = 0;
  int tool_failed = 0;
  int text_failed = 0;

  (void)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  output_text = turbo_openai_agent_state_latest_executor_output_text(ctx->state);
  tool_failed = turbo_openai_agent_state_executor_tool_results_failed(ctx->state);
  text_failed = turbo_openai_agent_contains_failure_marker(output_text);
  if (tool_failed || text_failed) {
    int rc;

    failure_reason = turbo_openai_agent_executor_failure_reason(ctx->state);
    failure_kind = tool_failed ? "tool_result" : "executor_text";
    rc = turbo_openai_agent_state_set_failure(ctx->state, failure_kind,
                                              failure_reason && failure_reason[0] != '\0'
                                                  ? failure_reason
                                                  : "step failed");
    if (rc != 0) {
      free(failure_reason);
      return rc;
    }

    replan_limit = turbo_openai_agent_state_replan_limit(ctx->state);
    replan_count = turbo_openai_agent_state_replan_count(ctx->state);
    if (replan_limit > 0 && replan_count >= replan_limit) {
      char *final_answer = NULL;
      int needed = snprintf(NULL, 0,
                            "FAILED: maximum replans exceeded after failure: %s",
                            failure_reason && failure_reason[0] != '\0'
                                ? failure_reason
                                : "step failed");
      turbo_openai_agent_state_set_failure(ctx->state, "limit",
                                           "maximum replans exceeded");
      if (needed > 0) {
        final_answer = (char *)malloc((size_t)needed + 1);
        if (final_answer) {
          snprintf(final_answer, (size_t)needed + 1,
                   "FAILED: maximum replans exceeded after failure: %s",
                   failure_reason && failure_reason[0] != '\0' ? failure_reason : "step failed");
          turbo_openai_agent_state_set_final_answer(ctx->state, final_answer);
          free(final_answer);
        }
      }
      turbo_graph_ctx_stop(ctx);
      free(failure_reason);
      return 0;
    }

    rc = turbo_openai_agent_state_request_replan(ctx->state,
                                                 failure_reason && failure_reason[0] != '\0'
                                                     ? failure_reason
                                                     : "step failed");
    free(failure_reason);
    return rc;
  }

  return 0;
}

turbo_graph_exec_status_t
turbo_openai_agent_install_planner_loop(
    turbo_graph_t *graph, turbo_openai_agent_t *planner_agent,
    turbo_openai_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !plan_step_node_name || !executor_node_name || !tool_node_name ||
      !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_add_node(graph, planner_node_name,
                                turbo_openai_agent_planner_model_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_commit_node_name,
                                turbo_openai_agent_plan_commit_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status =
      turbo_graph_add_node(graph, plan_step_node_name, turbo_openai_agent_plan_step_prepare_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, executor_node_name,
                                turbo_openai_agent_executor_model_node, executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, tool_node_name, turbo_openai_agent_executor_tool_node,
                                executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status =
      turbo_graph_add_node(graph, plan_advance_node_name, turbo_openai_agent_plan_advance_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, end_node_name, turbo_openai_end_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, planner_node_name, plan_commit_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_commit_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_step_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, tool_node_name,
                                turbo_openai_agent_has_pending_tool_calls, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, plan_advance_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, tool_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, end_node_name,
                                turbo_openai_agent_plan_complete_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  if (set_entry) {
    status = turbo_graph_set_entry(graph, planner_node_name);
    if (status != TURBO_GRAPH_EXEC_OK) {
      return status;
    }
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_openai_agent_install_review_loop(
    turbo_graph_t *graph, turbo_openai_agent_t *planner_agent,
    turbo_openai_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !plan_step_node_name || !review_node_name ||
      !executor_node_name || !tool_node_name || !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_add_node(graph, planner_node_name,
                                turbo_openai_agent_planner_model_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_commit_node_name,
                                turbo_openai_agent_plan_commit_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_step_node_name,
                                turbo_openai_agent_plan_step_prepare_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, review_node_name, turbo_openai_agent_review_node,
                                executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, executor_node_name,
                                turbo_openai_agent_executor_model_node, executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, tool_node_name, turbo_openai_agent_executor_tool_node,
                                executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_advance_node_name,
                                turbo_openai_agent_plan_advance_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, end_node_name, turbo_openai_end_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, planner_node_name, plan_commit_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_commit_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_step_node_name, review_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, review_node_name, executor_node_name,
                                turbo_openai_agent_review_approved_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, review_node_name, end_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, tool_node_name,
                                turbo_openai_agent_has_pending_tool_calls, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, plan_advance_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, tool_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, end_node_name,
                                turbo_openai_agent_plan_complete_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  if (set_entry) {
    status = turbo_graph_set_entry(graph, planner_node_name);
    if (status != TURBO_GRAPH_EXEC_OK) {
      return status;
    }
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_openai_agent_install_engineering_loop(
    turbo_graph_t *graph, turbo_openai_agent_t *planner_agent,
    turbo_openai_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_failure_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry) {
  return turbo_openai_agent_install_review_replan_loop(
      graph, planner_agent, executor_agent, planner_node_name, plan_commit_node_name,
      plan_step_node_name, review_node_name, executor_node_name, tool_node_name,
      detect_failure_node_name, turbo_openai_agent_detect_failed_step_node, NULL,
      replan_route_node_name, replan_prepare_node_name, plan_advance_node_name, end_node_name,
      set_entry);
}

turbo_graph_exec_status_t
turbo_openai_agent_install_engineering_loop_each_step_review(
    turbo_graph_t *graph, turbo_openai_agent_t *planner_agent,
    turbo_openai_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *review_reset_node_name,
    const char *plan_step_node_name, const char *review_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *detect_failure_node_name, const char *replan_route_node_name,
    const char *replan_prepare_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !review_reset_node_name || !plan_step_node_name ||
      !review_node_name || !executor_node_name || !tool_node_name ||
      !detect_failure_node_name || !replan_route_node_name || !replan_prepare_node_name ||
      !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_add_node(graph, planner_node_name,
                                turbo_openai_agent_planner_model_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_commit_node_name,
                                turbo_openai_agent_plan_commit_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, review_reset_node_name,
                                turbo_openai_agent_review_reset_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_step_node_name,
                                turbo_openai_agent_plan_step_prepare_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, review_node_name, turbo_openai_agent_review_node,
                                executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, executor_node_name,
                                turbo_openai_agent_executor_model_node, executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, tool_node_name, turbo_openai_agent_executor_tool_node,
                                executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, detect_failure_node_name,
                                turbo_openai_agent_detect_failed_step_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, replan_route_node_name,
                                turbo_openai_agent_replan_route_node,
                                (void *)replan_prepare_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, replan_prepare_node_name,
                                turbo_openai_agent_replan_prepare_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_advance_node_name,
                                turbo_openai_agent_plan_advance_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, end_node_name, turbo_openai_end_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, planner_node_name, plan_commit_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_commit_node_name, review_reset_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, review_reset_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_step_node_name, review_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, review_node_name, executor_node_name,
                                turbo_openai_agent_review_approved_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, review_node_name, end_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, tool_node_name,
                                turbo_openai_agent_has_pending_tool_calls, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, detect_failure_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, tool_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, detect_failure_node_name, replan_route_node_name, NULL,
                                NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, replan_route_node_name, plan_advance_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, replan_prepare_node_name, planner_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, end_node_name,
                                turbo_openai_agent_plan_complete_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, review_reset_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  if (set_entry) {
    status = turbo_graph_set_entry(graph, planner_node_name);
    if (status != TURBO_GRAPH_EXEC_OK) {
      return status;
    }
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_openai_agent_install_replan_loop(
    turbo_graph_t *graph, turbo_openai_agent_t *planner_agent,
    turbo_openai_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *detect_replan_node_name, turbo_graph_node_fn detect_replan_node_fn,
    void *detect_replan_user_data, const char *replan_route_node_name,
    const char *replan_prepare_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !plan_step_node_name || !executor_node_name || !tool_node_name ||
      !detect_replan_node_name || !detect_replan_node_fn || !replan_route_node_name ||
      !replan_prepare_node_name || !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_add_node(graph, planner_node_name,
                                turbo_openai_agent_planner_model_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_commit_node_name,
                                turbo_openai_agent_plan_commit_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_step_node_name,
                                turbo_openai_agent_plan_step_prepare_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, executor_node_name,
                                turbo_openai_agent_executor_model_node, executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, tool_node_name, turbo_openai_agent_executor_tool_node,
                                executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, detect_replan_node_name, detect_replan_node_fn,
                                detect_replan_user_data);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, replan_route_node_name,
                                turbo_openai_agent_replan_route_node,
                                (void *)replan_prepare_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, replan_prepare_node_name,
                                turbo_openai_agent_replan_prepare_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_advance_node_name,
                                turbo_openai_agent_plan_advance_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, end_node_name, turbo_openai_end_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, planner_node_name, plan_commit_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_commit_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_step_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, tool_node_name,
                                turbo_openai_agent_has_pending_tool_calls, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, detect_replan_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, tool_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, detect_replan_node_name, replan_route_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, replan_route_node_name, plan_advance_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, replan_prepare_node_name, planner_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, end_node_name,
                                turbo_openai_agent_plan_complete_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  if (set_entry) {
    status = turbo_graph_set_entry(graph, planner_node_name);
    if (status != TURBO_GRAPH_EXEC_OK) {
      return status;
    }
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_openai_agent_install_review_replan_loop(
    turbo_graph_t *graph, turbo_openai_agent_t *planner_agent,
    turbo_openai_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_replan_node_name,
    turbo_graph_node_fn detect_replan_node_fn, void *detect_replan_user_data,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !plan_step_node_name || !review_node_name ||
      !executor_node_name || !tool_node_name || !detect_replan_node_name ||
      !detect_replan_node_fn || !replan_route_node_name || !replan_prepare_node_name ||
      !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_add_node(graph, planner_node_name,
                                turbo_openai_agent_planner_model_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_commit_node_name,
                                turbo_openai_agent_plan_commit_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_step_node_name,
                                turbo_openai_agent_plan_step_prepare_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, review_node_name, turbo_openai_agent_review_node,
                                executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, executor_node_name,
                                turbo_openai_agent_executor_model_node, executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, tool_node_name, turbo_openai_agent_executor_tool_node,
                                executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, detect_replan_node_name, detect_replan_node_fn,
                                detect_replan_user_data);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, replan_route_node_name,
                                turbo_openai_agent_replan_route_node,
                                (void *)replan_prepare_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, replan_prepare_node_name,
                                turbo_openai_agent_replan_prepare_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, plan_advance_node_name,
                                turbo_openai_agent_plan_advance_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, end_node_name, turbo_openai_end_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, planner_node_name, plan_commit_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_commit_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_step_node_name, review_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, review_node_name, executor_node_name,
                                turbo_openai_agent_review_approved_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, review_node_name, end_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, tool_node_name,
                                turbo_openai_agent_has_pending_tool_calls, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, executor_node_name, detect_replan_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, tool_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, detect_replan_node_name, replan_route_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, replan_route_node_name, plan_advance_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, replan_prepare_node_name, planner_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, end_node_name,
                                turbo_openai_agent_plan_complete_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, plan_advance_node_name, plan_step_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  if (set_entry) {
    status = turbo_graph_set_entry(graph, planner_node_name);
    if (status != TURBO_GRAPH_EXEC_OK) {
      return status;
    }
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_openai_agent_install_loop(turbo_graph_t *graph, turbo_openai_agent_t *agent,
                                const char *model_node_name, const char *tool_node_name,
                                const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !agent || !model_node_name || !tool_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_add_node(graph, model_node_name, turbo_openai_agent_model_node, agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, tool_node_name, turbo_openai_agent_tool_node, agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_node(graph, end_node_name, turbo_openai_end_node, agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, model_node_name, tool_node_name,
                                turbo_openai_agent_has_pending_tool_calls, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, model_node_name, end_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, tool_node_name, model_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  if (set_entry) {
    status = turbo_graph_set_entry(graph, model_node_name);
    if (status != TURBO_GRAPH_EXEC_OK) {
      return status;
    }
  }

  return TURBO_GRAPH_EXEC_OK;
}
