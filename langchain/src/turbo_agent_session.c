#include "turbo_agent_session.h"

#include "turbo_prompt.h"
#include "turbo_agent_state.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_util_internal.h"
#include "turbo_agent_workflow.h"
#include "turbo_model_provider.h"

#include <stdlib.h>
#include <string.h>

struct turbo_agent_session_s {
  turbo_agent_runtime_t *runtime;
  turbo_agent_t *agent;
  turbo_agent_memory_store_t memory_store;
  turbo_agent_session_workflow_kind_t workflow_kind;
  char *thread_id;
  char *last_run_id;
  char *last_checkpoint_id;
  char *memory_namespace;
  char *parent_agent_run_id;
  char *parent_tool_call_id;
  char *parent_tool_name;
  char *model;
  char *base_url;
  char *provider_name;
  int has_api_key;
};

CXX_C_API int turbo_agent_runtime_apply_state_patch_bind(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_runtime_apply_checkpoint_state_patch_bind(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_runtime_apply_thread_state_patch_bind(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_runtime_resume_state_patch_bind_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_runtime_resume_checkpoint_state_patch_bind_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_runtime_resume_thread_state_patch_bind_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *thread_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_runtime_fork_state_patch_bind_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_runtime_fork_checkpoint_state_patch_bind_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_runtime_fork_thread_state_patch_bind_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *thread_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_session_apply_checkpoint_state_patch_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_session_apply_thread_state_patch_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_session_resume_checkpoint_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_session_resume_thread_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_session_fork_checkpoint_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_session_fork_thread_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

typedef struct turbo_agent_session_observer_bridge_s {
  turbo_agent_observer_bind_sink_t sink;
} turbo_agent_session_observer_bridge_t;

static int turbo_agent_session_should_load_env(const turbo_agent_session_config_t *config) {
  return config && config->load_env ? 1 : 0;
}

static int turbo_agent_session_should_create_agent(const turbo_agent_config_t *config) {
  return config &&
         (config->model || config->api_key || config->base_url || config->endpoint_path ||
          config->instructions || config->structured_output_name ||
          config->structured_output_schema_json || config->http_client || config->transport_fn ||
          config->provider || config->tool_registry);
}

static char *turbo_agent_session_strdup_or_null(const char *text) {
  return text ? turbo_agent_util_strdup(text) : NULL;
}

static int turbo_agent_session_bind_object_set_string(
    turbo_runtime_data_bind_value_t *object, const char *key, const char *value) {
  turbo_runtime_data_bind_value_t *field;

  if (!object || !key || !value) {
    return -1;
  }
  field = turbo_runtime_data_bind_value_create_string(value);
  if (!field) {
    return -1;
  }
  if (turbo_runtime_data_bind_object_set(object, key, field) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(field);
    return -1;
  }
  return 0;
}

static int turbo_agent_session_bind_object_set_int64(
    turbo_runtime_data_bind_value_t *object, const char *key, int64_t value) {
  turbo_runtime_data_bind_value_t *field;

  if (!object || !key) {
    return -1;
  }
  field = turbo_runtime_data_bind_value_create_int64(value);
  if (!field) {
    return -1;
  }
  if (turbo_runtime_data_bind_object_set(object, key, field) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(field);
    return -1;
  }
  return 0;
}

static int turbo_agent_session_bind_object_set_clone(
    turbo_runtime_data_bind_value_t *object, const char *key,
    const turbo_runtime_data_bind_value_t *value) {
  turbo_runtime_data_bind_value_t *copy;

  if (!object || !key || !value) {
    return -1;
  }
  copy = turbo_runtime_data_bind_value_clone(value);
  if (!copy) {
    return -1;
  }
  if (turbo_runtime_data_bind_object_set(object, key, copy) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(copy);
    return -1;
  }
  return 0;
}

static int turbo_agent_session_get_supervisor_array_json_local(
    turbo_agent_session_t *session,
    const json_value_t *(*selector)(const json_value_t *state),
    json_value_t **out_array_json) {
  turbo_runtime_data_bind_value_t *state_bind = NULL;
  json_value_t *state_json = NULL;
  json_value_t *array_json = NULL;
  const json_value_t *selected = NULL;
  int rc = -1;

  if (!session || !selector || !out_array_json) {
    return -1;
  }
  *out_array_json = NULL;

  if (turbo_agent_session_get_thread_state_bind(session, &state_bind) != 0 || !state_bind) {
    goto cleanup;
  }
  state_json = turbo_runtime_data_bind_value_to_json(state_bind);
  if (!state_json) {
    goto cleanup;
  }

  selected = selector(state_json);
  if (selected && turbo_json_type(selected) == TURBO_JSON_ARRAY) {
    array_json = turbo_json_clone(selected);
  } else {
    array_json = turbo_json_create_array();
  }
  if (!array_json) {
    goto cleanup;
  }

  *out_array_json = array_json;
  array_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&array_json);
  turbo_free_json(&state_json);
  turbo_runtime_data_bind_value_destroy(state_bind);
  return rc;
}

static int turbo_agent_session_build_supervisor_inbox_override_local(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    turbo_runtime_data_bind_value_t **out_state_override) {
  turbo_runtime_data_bind_value_t *state_bind = NULL;
  turbo_runtime_data_bind_value_t *override_bind = NULL;
  json_value_t *state_json = NULL;
  int rc = -1;

  if (!session || !text || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;

  if (turbo_agent_session_get_thread_state_bind(session, &state_bind) != 0 || !state_bind) {
    goto cleanup;
  }
  state_json = turbo_runtime_data_bind_value_to_json(state_bind);
  if (!state_json) {
    goto cleanup;
  }
  if (turbo_agent_state_append_supervisor_inbox_message(state_json, source_agent, text) != 0) {
    goto cleanup;
  }
  override_bind = turbo_runtime_data_bind_value_from_json(state_json);
  if (!override_bind) {
    goto cleanup;
  }

  *out_state_override = override_bind;
  override_bind = NULL;
  rc = 0;

cleanup:
  turbo_runtime_data_bind_value_destroy(override_bind);
  turbo_free_json(&state_json);
  turbo_runtime_data_bind_value_destroy(state_bind);
  return rc;
}

static int turbo_agent_session_build_supervisor_inspect_local(
    turbo_agent_session_t *session, json_value_t **out_inspect_json) {
  turbo_runtime_data_bind_value_t *state_bind = NULL;
  turbo_runtime_data_bind_value_t *control_bind = NULL;
  turbo_runtime_data_bind_value_t *workflow_bind = NULL;
  json_value_t *state_json = NULL;
  json_value_t *control_json = NULL;
  json_value_t *workflow_json = NULL;
  json_value_t *inspect_json = NULL;
  json_value_t *inbox_json = NULL;
  json_value_t *history_json = NULL;
  json_value_t *supervisor_json = NULL;
  const json_value_t *state_inbox = NULL;
  const json_value_t *state_history = NULL;
  const json_value_t *control_supervisor = NULL;
  int rc = -1;

  if (!session || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_thread_state_bind(session, &state_bind) != 0 || !state_bind) {
    goto cleanup;
  }
  state_json = turbo_runtime_data_bind_value_to_json(state_bind);
  if (!state_json) {
    goto cleanup;
  }
  control_bind = turbo_agent_state_control_snapshot_bind(state_bind);
  workflow_bind = turbo_agent_state_workflow_snapshot_bind(state_bind);
  if (!control_bind || !workflow_bind) {
    goto cleanup;
  }
  control_json = turbo_runtime_data_bind_value_to_json(control_bind);
  workflow_json = turbo_runtime_data_bind_value_to_json(workflow_bind);
  if (!control_json || !workflow_json) {
    goto cleanup;
  }

  state_inbox = turbo_agent_state_supervisor_inbox(state_json);
  state_history = turbo_agent_state_supervisor_handoff_history(state_json);
  control_supervisor = turbo_json_object_get(control_json, "supervisor");

  inbox_json = state_inbox && turbo_json_type(state_inbox) == TURBO_JSON_ARRAY
                   ? turbo_json_clone(state_inbox)
                   : turbo_json_create_array();
  history_json = state_history && turbo_json_type(state_history) == TURBO_JSON_ARRAY
                     ? turbo_json_clone(state_history)
                     : turbo_json_create_array();
  supervisor_json = control_supervisor && turbo_json_type(control_supervisor) == TURBO_JSON_OBJECT
                        ? turbo_json_clone(control_supervisor)
                        : turbo_json_create_object();
  inspect_json = turbo_json_create_object();
  if (!inbox_json || !history_json || !supervisor_json || !inspect_json) {
    goto cleanup;
  }

  turbo_json_object_add(inspect_json, "supervisor", supervisor_json);
  supervisor_json = NULL;
  turbo_json_object_add(inspect_json, "inbox", inbox_json);
  inbox_json = NULL;
  turbo_json_object_add(inspect_json, "handoff_history", history_json);
  history_json = NULL;
  turbo_json_object_add(inspect_json, "control_snapshot", control_json);
  control_json = NULL;
  turbo_json_object_add(inspect_json, "workflow_snapshot", workflow_json);
  workflow_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&supervisor_json);
  turbo_free_json(&history_json);
  turbo_free_json(&inbox_json);
  turbo_free_json(&workflow_json);
  turbo_free_json(&control_json);
  turbo_free_json(&state_json);
  turbo_runtime_data_bind_value_destroy(workflow_bind);
  turbo_runtime_data_bind_value_destroy(control_bind);
  turbo_runtime_data_bind_value_destroy(state_bind);
  return rc;
}

static int turbo_agent_session_build_orchestration_inspect_local(
    turbo_agent_session_t *session, json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *thread_lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *child_runs_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  turbo_runtime_data_bind_value_t *thread_timeline_bind = NULL;
  int rc = -1;

  if (!session || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_supervisor_inspect(session, &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_thread_timeline_bind(session, &thread_timeline_bind) == 0 &&
      thread_timeline_bind) {
    thread_timeline_json = turbo_runtime_data_bind_value_to_json(thread_timeline_bind);
    if (!thread_timeline_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_session_list_thread_lineage(session, &thread_lineage_json) != 0 ||
      !thread_lineage_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_branch_tree(session, &branch_tree_json) != 0 || !branch_tree_json) {
    goto cleanup;
  }
  if (session->runtime && session->last_run_id && session->last_run_id[0] != '\0') {
    if (turbo_agent_runtime_list_child_runs(session->runtime, session->last_run_id,
                                            &child_runs_json) != 0) {
      goto cleanup;
    }
  } else {
    child_runs_json = turbo_json_create_array();
  }
  if (!child_runs_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "supervisor_inspect", supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  if (thread_timeline_json) {
    turbo_json_object_add(inspect_json, "thread_timeline", thread_timeline_json);
    thread_timeline_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "thread_timeline");
  }
  turbo_json_object_add(inspect_json, "thread_lineage", thread_lineage_json);
  thread_lineage_json = NULL;
  turbo_json_object_add(inspect_json, "branch_tree", branch_tree_json);
  branch_tree_json = NULL;
  turbo_json_object_add(inspect_json, "child_runs", child_runs_json);
  child_runs_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_runs_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_lineage_json);
  turbo_free_json(&thread_timeline_json);
  turbo_runtime_data_bind_value_destroy(thread_timeline_bind);
  turbo_free_json(&supervisor_inspect_json);
  return rc;
}

static const char *turbo_agent_session_observer_type_for_trace_name(const char *name) {
  if (!name || name[0] == '\0') {
    return NULL;
  }
  if (strcmp(name, "model_request") == 0 || strcmp(name, "model_response") == 0) {
    return "model_delta";
  }
  if (strcmp(name, "tool_dispatch") == 0) {
    return "tool_call_started";
  }
  if (strcmp(name, "tool_result") == 0) {
    return "tool_result";
  }
  if (strcmp(name, "structured_retry") == 0 || strcmp(name, "replan_requested") == 0 ||
      strcmp(name, "review_required") == 0 || strcmp(name, "review_approved") == 0 ||
      strcmp(name, "guardrail_rejected") == 0 || strcmp(name, "memory_load") == 0 ||
      strcmp(name, "memory_save") == 0) {
    return "state_updated";
  }
  return NULL;
}

static int turbo_agent_session_observer_event_from_trace(
    const turbo_runtime_data_bind_value_t *trace_event,
    turbo_runtime_data_bind_value_t **out_observer_event) {
  const char *observer_type;
  const char *name;
  turbo_runtime_data_bind_value_t *observer_event = NULL;

  if (!trace_event || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;

  name = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(trace_event, "name"));
  observer_type = turbo_agent_session_observer_type_for_trace_name(name);
  if (!observer_type) {
    return 0;
  }

  observer_event = turbo_runtime_data_bind_value_create_object();
  if (!observer_event ||
      turbo_agent_session_bind_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_session_bind_object_set_string(observer_event, "type", observer_type) != 0 ||
      turbo_agent_session_bind_object_set_clone(observer_event, "event", trace_event) != 0 ||
      turbo_agent_session_bind_object_set_string(observer_event, "name", name) != 0 ||
      turbo_agent_session_bind_object_set_string(
          observer_event, "detail",
          turbo_runtime_data_bind_value_as_string(
              turbo_runtime_data_bind_object_get(trace_event, "detail"))) != 0 ||
      turbo_agent_session_bind_object_set_string(
          observer_event, "payload",
          turbo_runtime_data_bind_value_as_string(
              turbo_runtime_data_bind_object_get(trace_event, "payload"))) != 0 ||
      turbo_agent_session_bind_object_set_int64(
          observer_event, "status",
          turbo_runtime_data_bind_value_as_int64(
              turbo_runtime_data_bind_object_get(trace_event, "status"), 0)) != 0) {
    turbo_runtime_data_bind_value_destroy(observer_event);
    return -1;
  }

  *out_observer_event = observer_event;
  return 1;
}

static void turbo_agent_session_observer_bridge_free(void *user_data) {
  turbo_agent_session_observer_bridge_t *bridge =
      (turbo_agent_session_observer_bridge_t *)user_data;

  if (!bridge) {
    return;
  }
  if (bridge->sink.user_data_free) {
    bridge->sink.user_data_free(bridge->sink.user_data);
  }
  free(bridge);
}

static void turbo_agent_session_capture_observer_event(turbo_agent_t *agent_unused,
                                                       const turbo_runtime_data_bind_value_t *event,
                                                       void *user_data) {
  turbo_agent_session_observer_bridge_t *bridge =
      (turbo_agent_session_observer_bridge_t *)user_data;
  turbo_runtime_data_bind_value_t *observer_event = NULL;
  int rc;

  (void)agent_unused;
  if (!bridge || !bridge->sink.callback || !event) {
    return;
  }
  rc = turbo_agent_session_observer_event_from_trace(event, &observer_event);
  if (rc <= 0 || !observer_event) {
    turbo_runtime_data_bind_value_destroy(observer_event);
    return;
  }
  bridge->sink.callback(observer_event, bridge->sink.user_data);
  turbo_runtime_data_bind_value_destroy(observer_event);
}

static void turbo_agent_session_replace_string(char **slot, const char *text) {
  char *copy = turbo_agent_session_strdup_or_null(text);

  if (!slot) {
    free(copy);
    return;
  }
  free(*slot);
  *slot = copy;
}

static int turbo_agent_session_load_memory_context_record(json_value_t *state,
                                                          const json_value_t *record) {
  const char *value_json;
  json_value_t *payload = NULL;
  const char *scope;
  const char *path;
  const char *text;
  int rc;

  if (!state || !record || turbo_json_type(record) != TURBO_JSON_OBJECT) {
    return -1;
  }
  value_json = turbo_json_get_string(record, "value_json");
  if (!value_json || value_json[0] == '\0' ||
      turbo_parse_json((const uint8_t *)value_json, strlen(value_json), &payload) != 0 || !payload ||
      turbo_json_type(payload) != TURBO_JSON_OBJECT) {
    turbo_free_json(&payload);
    return -1;
  }
  scope = turbo_json_get_string(payload, "scope");
  path = turbo_json_get_string(payload, "path");
  text = turbo_json_get_string(payload, "text");
  if (!scope || scope[0] == '\0' || !text || text[0] == '\0') {
    turbo_free_json(&payload);
    return -1;
  }
  rc = turbo_agent_state_add_memory_context_layer(state, scope, path, text);
  turbo_free_json(&payload);
  return rc;
}

static int turbo_agent_session_capture_summary(turbo_agent_session_t *session,
                                               const json_value_t *summary) {
  const char *thread_id;
  const char *run_id;
  json_value_t *checkpoint_value;
  const char *checkpoint_id = NULL;

  if (!session || !summary) {
    return -1;
  }
  thread_id = turbo_json_get_string(summary, "thread_id");
  run_id = turbo_json_get_string(summary, "run_id");
  checkpoint_value = turbo_json_object_get(summary, "checkpoint_id");
  if (!thread_id || !run_id) {
    return -1;
  }
  if (checkpoint_value && turbo_json_type(checkpoint_value) == TURBO_JSON_STRING) {
    checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
  }

  turbo_agent_session_replace_string(&session->thread_id, thread_id);
  turbo_agent_session_replace_string(&session->last_run_id, run_id);
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    turbo_agent_session_replace_string(&session->last_checkpoint_id, checkpoint_id);
  }
  return 0;
}

static const char *turbo_agent_session_resolve_checkpoint_id(
    const turbo_agent_session_t *session, const char *checkpoint_id) {
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    return checkpoint_id;
  }
  return session ? session->last_checkpoint_id : NULL;
}

static const char *turbo_agent_session_resolve_run_id(const turbo_agent_session_t *session,
                                                      const char *run_id) {
  if (run_id && run_id[0] != '\0') {
    return run_id;
  }
  return session ? session->last_run_id : NULL;
}

static const char *turbo_agent_session_resolve_parent_agent_run_id(
    const turbo_agent_session_t *session, const char *parent_agent_run_id) {
  turbo_agent_execution_context_t current_context = {0};

  if (parent_agent_run_id && parent_agent_run_id[0] != '\0') {
    return parent_agent_run_id;
  }
  if (session && session->parent_agent_run_id && session->parent_agent_run_id[0] != '\0') {
    return session->parent_agent_run_id;
  }
  turbo_agent_execution_context_get(&current_context);
  return current_context.run_id;
}

static int turbo_agent_session_has_parent_link(const turbo_agent_session_t *session,
                                               turbo_agent_runtime_parent_link_t *out_link) {
  turbo_agent_execution_context_t current_context = {0};
  const char *resolved_parent_agent_run_id;
  const char *resolved_parent_tool_call_id;
  const char *resolved_parent_tool_name;
  int has_link = 0;

  if (out_link) {
    memset(out_link, 0, sizeof(*out_link));
  }
  if (!session) {
    return 0;
  }
  turbo_agent_execution_context_get(&current_context);
  resolved_parent_agent_run_id =
      (session->parent_agent_run_id && session->parent_agent_run_id[0] != '\0')
          ? session->parent_agent_run_id
          : current_context.run_id;
  resolved_parent_tool_call_id =
      (session->parent_tool_call_id && session->parent_tool_call_id[0] != '\0')
          ? session->parent_tool_call_id
          : current_context.tool_call_id;
  resolved_parent_tool_name =
      (session->parent_tool_name && session->parent_tool_name[0] != '\0')
          ? session->parent_tool_name
          : current_context.tool_name;
  if (out_link) {
    out_link->parent_agent_run_id = resolved_parent_agent_run_id;
    out_link->parent_tool_call_id = resolved_parent_tool_call_id;
    out_link->parent_tool_name = resolved_parent_tool_name;
  }
  has_link = (resolved_parent_agent_run_id && resolved_parent_agent_run_id[0] != '\0') ||
             (resolved_parent_tool_call_id && resolved_parent_tool_call_id[0] != '\0') ||
             (resolved_parent_tool_name && resolved_parent_tool_name[0] != '\0');
  return has_link;
}

static const char *turbo_agent_session_output_item_child_run_id(
    const json_value_t *output_item) {
  return turbo_agent_state_tool_result_child_run_id(output_item);
}

static const char *turbo_agent_session_output_item_child_checkpoint_id(
    const json_value_t *output_item) {
  return turbo_agent_state_tool_result_child_checkpoint_id(output_item);
}

static const char *turbo_agent_session_output_item_child_thread_id(
    const json_value_t *output_item) {
  return turbo_agent_state_tool_result_child_thread_id(output_item);
}

static int turbo_agent_session_apply_effective_defaults(turbo_agent_config_t *config) {
  const turbo_model_provider_t *provider;

  if (!config) {
    return -1;
  }
  if (!config->model &&
      (config->api_key || config->transport_fn || config->http_client || config->base_url ||
       config->provider)) {
    config->model = "gpt-5.4";
  }
  provider = config->provider ? config->provider : turbo_model_provider_openai_responses();
  config->provider = provider;
  if (!config->base_url && (config->api_key || config->http_client)) {
    config->base_url = turbo_model_provider_select_base_url(provider, NULL, NULL,
                                                            "https://api.openai.com/v1");
  }
  return 0;
}

CXX_C_API turbo_agent_session_t *
turbo_agent_session_create(const turbo_agent_session_config_t *config) {
  turbo_agent_session_t *session;
  turbo_agent_runtime_store_t runtime_store;
  turbo_agent_config_t effective_config = {0};
  const turbo_model_provider_t *provider;
  int create_agent;

  if (!config) {
    return NULL;
  }
  runtime_store = config->runtime_store;
  if (!runtime_store.put || !runtime_store.get || !runtime_store.list) {
    runtime_store = turbo_agent_runtime_store_memory_create();
  }
  if (!runtime_store.put || !runtime_store.get || !runtime_store.list) {
    return NULL;
  }

  session = (turbo_agent_session_t *)calloc(1, sizeof(*session));
  if (!session) {
    if (runtime_store.user_data_free) {
      runtime_store.user_data_free(runtime_store.user_data);
    }
    return NULL;
  }

  session->runtime = turbo_agent_runtime_create(&runtime_store);
  if (!session->runtime) {
    if (runtime_store.user_data_free) {
      runtime_store.user_data_free(runtime_store.user_data);
    }
    turbo_agent_session_destroy(session);
    return NULL;
  }

  effective_config = config->agent_config;
  if (turbo_agent_session_should_load_env(config)) {
    turbo_agent_config_apply_env(&effective_config, config->env_path, config->overwrite_env);
  }
  create_agent = turbo_agent_session_should_create_agent(&effective_config);

  if (create_agent) {
    turbo_agent_session_apply_effective_defaults(&effective_config);
    session->agent = turbo_agent_create(&effective_config);
    if (!session->agent) {
      turbo_agent_session_destroy(session);
      return NULL;
    }
  }

  provider = effective_config.provider;
  session->model = turbo_agent_session_strdup_or_null(create_agent ? effective_config.model : NULL);
  session->base_url =
      turbo_agent_session_strdup_or_null(create_agent ? effective_config.base_url : NULL);
  session->provider_name = turbo_agent_session_strdup_or_null(
      (create_agent && provider) ? provider->name : NULL);
  session->has_api_key =
      (create_agent && effective_config.api_key && effective_config.api_key[0] != '\0') ? 1 : 0;
  session->memory_store = config->memory_store;
  session->workflow_kind = config->workflow_kind;
  session->thread_id = turbo_agent_session_strdup_or_null(
      (config->thread_id && config->thread_id[0] != '\0') ? config->thread_id : NULL);
  session->memory_namespace = turbo_agent_session_strdup_or_null(
      (config->memory_namespace && config->memory_namespace[0] != '\0') ? config->memory_namespace
                                                                        : NULL);
  session->parent_agent_run_id = turbo_agent_session_strdup_or_null(
      (config->parent_agent_run_id && config->parent_agent_run_id[0] != '\0')
          ? config->parent_agent_run_id
          : NULL);
  session->parent_tool_call_id = turbo_agent_session_strdup_or_null(
      (config->parent_tool_call_id && config->parent_tool_call_id[0] != '\0')
          ? config->parent_tool_call_id
          : NULL);
  session->parent_tool_name = turbo_agent_session_strdup_or_null(
      (config->parent_tool_name && config->parent_tool_name[0] != '\0')
          ? config->parent_tool_name
          : NULL);
  return session;
}

CXX_C_API void turbo_agent_session_destroy(turbo_agent_session_t *session) {
  if (!session) {
    return;
  }
  turbo_agent_destroy(session->agent);
  turbo_agent_runtime_destroy(session->runtime);
  turbo_agent_memory_store_destroy(&session->memory_store);
  free(session->thread_id);
  free(session->last_run_id);
  free(session->last_checkpoint_id);
  free(session->memory_namespace);
  free(session->parent_agent_run_id);
  free(session->parent_tool_call_id);
  free(session->parent_tool_name);
  free(session->model);
  free(session->base_url);
  free(session->provider_name);
  free(session);
}

CXX_C_API turbo_agent_t *turbo_agent_session_agent(const turbo_agent_session_t *session) {
  return session ? session->agent : NULL;
}

CXX_C_API turbo_agent_runtime_t *
turbo_agent_session_runtime(const turbo_agent_session_t *session) {
  return session ? session->runtime : NULL;
}

CXX_C_API const turbo_agent_memory_store_t *
turbo_agent_session_memory_store(const turbo_agent_session_t *session) {
  if (!session) {
    return NULL;
  }
  if (!session->memory_store.get && !session->memory_store.put && !session->memory_store.list &&
      !session->memory_store.query && !session->memory_store.remove) {
    return NULL;
  }
  return &session->memory_store;
}

CXX_C_API const char *turbo_agent_session_model(const turbo_agent_session_t *session) {
  return session ? session->model : NULL;
}

CXX_C_API const char *turbo_agent_session_base_url(const turbo_agent_session_t *session) {
  return session ? session->base_url : NULL;
}

CXX_C_API const char *turbo_agent_session_provider_name(
    const turbo_agent_session_t *session) {
  return session ? session->provider_name : NULL;
}

CXX_C_API int turbo_agent_session_has_api_key(const turbo_agent_session_t *session) {
  return session ? session->has_api_key : 0;
}

CXX_C_API const char *turbo_agent_session_thread_id(const turbo_agent_session_t *session) {
  return session ? session->thread_id : NULL;
}

CXX_C_API const char *turbo_agent_session_last_run_id(
    const turbo_agent_session_t *session) {
  return session ? session->last_run_id : NULL;
}

CXX_C_API const char *turbo_agent_session_last_checkpoint_id(
    const turbo_agent_session_t *session) {
  return session ? session->last_checkpoint_id : NULL;
}

CXX_C_API turbo_agent_session_workflow_kind_t
turbo_agent_session_workflow_kind(const turbo_agent_session_t *session) {
  return session ? session->workflow_kind : TURBO_AGENT_SESSION_WORKFLOW_LOOP;
}

CXX_C_API const char *turbo_agent_session_memory_namespace(
    const turbo_agent_session_t *session) {
  return session ? session->memory_namespace : NULL;
}

CXX_C_API int turbo_agent_session_add_trace_bind_sink(
    turbo_agent_session_t *session, const turbo_agent_trace_bind_sink_t *sink) {
  if (!session || !session->agent || !sink) {
    return -1;
  }
  return turbo_agent_add_trace_bind_sink(session->agent, sink);
}

CXX_C_API int turbo_agent_session_add_observer_bind_sink(
    turbo_agent_session_t *session, const turbo_agent_observer_bind_sink_t *sink) {
  turbo_agent_session_observer_bridge_t *bridge;
  turbo_agent_trace_bind_sink_t trace_sink = {0};

  if (!session || !session->agent || !sink || !sink->callback) {
    return -1;
  }
  bridge = (turbo_agent_session_observer_bridge_t *)calloc(1, sizeof(*bridge));
  if (!bridge) {
    return -1;
  }
  bridge->sink = *sink;
  trace_sink.callback = turbo_agent_session_capture_observer_event;
  trace_sink.user_data = bridge;
  trace_sink.user_data_free = turbo_agent_session_observer_bridge_free;
  if (turbo_agent_add_trace_bind_sink(session->agent, &trace_sink) != 0) {
    free(bridge);
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_session_set_trace_history_enabled(
    turbo_agent_session_t *session, int enabled) {
  if (!session || !session->agent) {
    return -1;
  }
  return turbo_agent_set_trace_history_enabled(session->agent, enabled);
}

CXX_C_API int turbo_agent_session_get_thread(turbo_agent_session_t *session,
                                             json_value_t **out_thread_json) {
  if (!session || !session->runtime || !session->thread_id || !out_thread_json) {
    return -1;
  }
  return turbo_agent_runtime_get_thread(session->runtime, session->thread_id, out_thread_json);
}

CXX_C_API int turbo_agent_session_get_run(turbo_agent_session_t *session, const char *run_id,
                                          json_value_t **out_run_json) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_run(session->runtime, resolved_run_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_latest_run(turbo_agent_session_t *session,
                                                 json_value_t **out_run_json) {
  if (!session || !session->runtime || !session->thread_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_latest_run(session->runtime, session->thread_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_pending_run(turbo_agent_session_t *session,
                                                  json_value_t **out_run_json) {
  if (!session || !session->runtime || !session->thread_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_pending_run(session->runtime, session->thread_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_checkpoint(turbo_agent_session_t *session,
                                                 const char *checkpoint_id,
                                                 json_value_t **out_checkpoint_json) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint(session->runtime, resolved_checkpoint_id,
                                            out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_latest_checkpoint(turbo_agent_session_t *session,
                                                        const char *run_id,
                                                        json_value_t **out_checkpoint_json) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_latest_checkpoint(session->runtime, resolved_run_id,
                                                   out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_thread_state_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_state) {
  if (!session || !session->runtime || !session->thread_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_state_bind(session->runtime, session->thread_id,
                                                   out_state);
}

CXX_C_API int turbo_agent_session_get_thread_trace_events_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_events) {
  if (!session || !session->runtime || !session->thread_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_trace_events_bind(session->runtime, session->thread_id,
                                                          out_events);
}

CXX_C_API int turbo_agent_session_get_run_state_bind(
    turbo_agent_session_t *session, const char *run_id,
    turbo_runtime_data_bind_value_t **out_state) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_run_state_bind(session->runtime, resolved_run_id, out_state);
}

CXX_C_API int turbo_agent_session_get_run_trace_events_bind(
    turbo_agent_session_t *session, const char *run_id,
    turbo_runtime_data_bind_value_t **out_events) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_run_trace_events_bind(session->runtime, resolved_run_id,
                                                       out_events);
}

CXX_C_API int turbo_agent_session_get_checkpoint_state_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_state) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_state_bind(session->runtime, resolved_checkpoint_id,
                                                       out_state);
}

CXX_C_API int turbo_agent_session_update_checkpoint_state_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  return turbo_agent_session_apply_checkpoint_state_patch_bind(session, checkpoint_id, state_patch,
                                                               out_state_override);
}

CXX_C_API int turbo_agent_session_update_thread_state_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  return turbo_agent_session_apply_thread_state_patch_bind(session, state_patch, out_state_override);
}

CXX_C_API int turbo_agent_session_get_supervisor_inbox(
    turbo_agent_session_t *session, json_value_t **out_inbox_json) {
  return turbo_agent_session_get_supervisor_array_json_local(
      session, turbo_agent_state_supervisor_inbox, out_inbox_json);
}

CXX_C_API int turbo_agent_session_get_supervisor_handoff_history(
    turbo_agent_session_t *session, json_value_t **out_history_json) {
  return turbo_agent_session_get_supervisor_array_json_local(
      session, turbo_agent_state_supervisor_handoff_history, out_history_json);
}

CXX_C_API int turbo_agent_session_get_supervisor_inspect(
    turbo_agent_session_t *session, json_value_t **out_inspect_json) {
  return turbo_agent_session_build_supervisor_inspect_local(session, out_inspect_json);
}

CXX_C_API int turbo_agent_session_get_orchestration_inspect(
    turbo_agent_session_t *session, json_value_t **out_inspect_json) {
  return turbo_agent_session_build_orchestration_inspect_local(session, out_inspect_json);
}

CXX_C_API int turbo_agent_session_get_observability_index(
    turbo_agent_session_t *session, json_value_t **out_index_json) {
  if (!session || !session->runtime || !session->thread_id || !out_index_json) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_observability_index(session->runtime, session->thread_id,
                                                            out_index_json);
}

CXX_C_API int turbo_agent_session_append_supervisor_inbox_message_bind(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    turbo_runtime_data_bind_value_t **out_state_override) {
  return turbo_agent_session_build_supervisor_inbox_override_local(
      session, source_agent, text, out_state_override);
}

CXX_C_API int turbo_agent_session_get_checkpoint_trace_events_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_trace_events_bind(session->runtime,
                                                              resolved_checkpoint_id, out_events);
}

CXX_C_API int turbo_agent_session_get_child_run(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_run_json) {
  const char *child_run_id = turbo_agent_session_output_item_child_run_id(output_item);

  if (!session || !session->runtime || !child_run_id || child_run_id[0] == '\0' ||
      !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_run(session->runtime, child_run_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_child_checkpoint(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoint_json) {
  const char *child_checkpoint_id =
      turbo_agent_session_output_item_child_checkpoint_id(output_item);

  if (!session || !session->runtime || !child_checkpoint_id ||
      child_checkpoint_id[0] == '\0' || !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint(session->runtime, child_checkpoint_id,
                                            out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_child_checkpoint_context(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_context_json) {
  const char *child_checkpoint_id =
      turbo_agent_session_output_item_child_checkpoint_id(output_item);

  if (!session || !session->runtime || !child_checkpoint_id || child_checkpoint_id[0] == '\0' ||
      !out_context_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_context(session->runtime, child_checkpoint_id,
                                                    out_context_json);
}

CXX_C_API int turbo_agent_session_get_child_thread_timeline_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_timeline) {
  const char *child_thread_id =
      turbo_agent_session_output_item_child_thread_id(output_item);

  if (!session || !session->runtime || !child_thread_id || child_thread_id[0] == '\0' ||
      !out_timeline) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_timeline_bind(session->runtime, child_thread_id,
                                                      out_timeline);
}

CXX_C_API int turbo_agent_session_get_child_branch_tree(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_branch_tree_json) {
  const char *child_thread_id =
      turbo_agent_session_output_item_child_thread_id(output_item);

  if (!session || !session->runtime || !child_thread_id || child_thread_id[0] == '\0' ||
      !out_branch_tree_json) {
    return -1;
  }
  return turbo_agent_runtime_get_branch_tree(session->runtime, child_thread_id,
                                             out_branch_tree_json);
}

CXX_C_API int turbo_agent_session_get_child_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *run_json = NULL;
  json_value_t *checkpoints_json = NULL;
  json_value_t *latest_checkpoint_json = NULL;
  json_value_t *checkpoint_context_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *trace_events_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  json_value_t *branch_tree_json = NULL;
  turbo_runtime_data_bind_value_t *history_events_bind = NULL;
  turbo_runtime_data_bind_value_t *trace_events_bind = NULL;
  turbo_runtime_data_bind_value_t *thread_timeline_bind = NULL;
  const char *child_checkpoint_id;

  if (!session || !session->runtime || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  child_checkpoint_id = turbo_agent_session_output_item_child_checkpoint_id(output_item);
  if (turbo_agent_session_get_child_run(session, output_item, &run_json) != 0 || !run_json) {
    goto cleanup;
  }
  if (turbo_agent_session_list_child_checkpoints(session, output_item, &checkpoints_json) != 0 ||
      !checkpoints_json) {
    goto cleanup;
  }
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    if (turbo_agent_session_get_child_checkpoint(session, output_item, &latest_checkpoint_json) != 0 ||
        !latest_checkpoint_json) {
      goto cleanup;
    }
    if (turbo_agent_session_get_child_checkpoint_context(session, output_item,
                                                         &checkpoint_context_json) != 0 ||
        !checkpoint_context_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_session_load_child_history_events_bind(session, output_item, &history_events_bind) !=
          0 ||
      !history_events_bind) {
    goto cleanup;
  }
  history_events_json = turbo_runtime_data_bind_value_to_json(history_events_bind);
  if (!history_events_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_child_trace_events_bind(session, output_item, &trace_events_bind) != 0 ||
      !trace_events_bind) {
    goto cleanup;
  }
  trace_events_json = turbo_runtime_data_bind_value_to_json(trace_events_bind);
  if (!trace_events_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_child_thread_timeline_bind(session, output_item,
                                                         &thread_timeline_bind) == 0 &&
      thread_timeline_bind) {
    thread_timeline_json = turbo_runtime_data_bind_value_to_json(thread_timeline_bind);
    if (!thread_timeline_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_session_get_child_branch_tree(session, output_item, &branch_tree_json) != 0) {
    turbo_free_json(&branch_tree_json);
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "run", run_json);
  run_json = NULL;
  turbo_json_object_add(inspect_json, "checkpoints", checkpoints_json);
  checkpoints_json = NULL;
  if (latest_checkpoint_json) {
    turbo_json_object_add(inspect_json, "latest_checkpoint", latest_checkpoint_json);
    latest_checkpoint_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "latest_checkpoint");
  }
  if (checkpoint_context_json) {
    turbo_json_object_add(inspect_json, "checkpoint_context", checkpoint_context_json);
    checkpoint_context_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "checkpoint_context");
  }
  turbo_json_object_add(inspect_json, "history_events", history_events_json);
  history_events_json = NULL;
  turbo_json_object_add(inspect_json, "trace_events", trace_events_json);
  trace_events_json = NULL;
  if (thread_timeline_json) {
    turbo_json_object_add(inspect_json, "thread_timeline", thread_timeline_json);
    thread_timeline_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "thread_timeline");
  }
  if (branch_tree_json) {
    turbo_json_object_add(inspect_json, "branch_tree", branch_tree_json);
    branch_tree_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "branch_tree");
  }

  *out_inspect_json = inspect_json;
  inspect_json = NULL;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_timeline_json);
  turbo_runtime_data_bind_value_destroy(thread_timeline_bind);
  turbo_free_json(&trace_events_json);
  turbo_runtime_data_bind_value_destroy(trace_events_bind);
  turbo_free_json(&history_events_json);
  turbo_runtime_data_bind_value_destroy(history_events_bind);
  turbo_free_json(&checkpoint_context_json);
  turbo_free_json(&latest_checkpoint_json);
  turbo_free_json(&checkpoints_json);
  turbo_free_json(&run_json);
  return *out_inspect_json ? 0 : -1;
}

CXX_C_API int turbo_agent_session_get_child_orchestration_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *child_inspect_json = NULL;
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;

  if (!session || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_child_inspect(session, output_item, &child_inspect_json) != 0 ||
      !child_inspect_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }

  parent_agent_run_id = turbo_agent_state_tool_result_parent_agent_run_id(output_item);
  parent_tool_call_id = turbo_agent_state_tool_result_parent_tool_call_id(output_item);
  parent_tool_name = turbo_agent_state_tool_result_parent_tool_name(output_item);

  if (parent_agent_run_id && parent_agent_run_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_agent_run_id", parent_agent_run_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_agent_run_id");
  }
  if (parent_tool_call_id && parent_tool_call_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_tool_call_id", parent_tool_call_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_tool_call_id");
  }
  if (parent_tool_name && parent_tool_name[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_tool_name", parent_tool_name);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_tool_name");
  }
  turbo_json_object_add(inspect_json, "child_inspect", child_inspect_json);
  child_inspect_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  return 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_inspect_json);
  return -1;
}

CXX_C_API int turbo_agent_session_get_child_multi_agent_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *orchestration_inspect_json = NULL;
  json_value_t *child_orchestration_inspect_json = NULL;

  if (!session || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_supervisor_inspect(session, &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_orchestration_inspect(session, &orchestration_inspect_json) != 0 ||
      !orchestration_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_child_orchestration_inspect(session, output_item,
                                                          &child_orchestration_inspect_json) != 0 ||
      !child_orchestration_inspect_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "supervisor_inspect", supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  turbo_json_object_add(inspect_json, "orchestration_inspect", orchestration_inspect_json);
  orchestration_inspect_json = NULL;
  turbo_json_object_add(inspect_json, "child_orchestration_inspect",
                        child_orchestration_inspect_json);
  child_orchestration_inspect_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  return 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_orchestration_inspect_json);
  turbo_free_json(&orchestration_inspect_json);
  turbo_free_json(&supervisor_inspect_json);
  return -1;
}

CXX_C_API int turbo_agent_session_list_runs(turbo_agent_session_t *session,
                                            json_value_t **out_runs_json) {
  if (!session || !session->runtime || !session->thread_id || !out_runs_json) {
    return -1;
  }
  return turbo_agent_runtime_list_runs(session->runtime, session->thread_id, out_runs_json);
}

CXX_C_API int turbo_agent_session_list_child_runs(
    turbo_agent_session_t *session, const char *parent_agent_run_id,
    json_value_t **out_runs_json) {
  const char *resolved_parent_agent_run_id =
      turbo_agent_session_resolve_parent_agent_run_id(session, parent_agent_run_id);

  if (!session || !session->runtime || !resolved_parent_agent_run_id || !out_runs_json) {
    return -1;
  }
  return turbo_agent_runtime_list_child_runs(session->runtime, resolved_parent_agent_run_id,
                                             out_runs_json);
}

CXX_C_API int turbo_agent_session_list_child_checkpoints(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoints_json) {
  const char *child_run_id = turbo_agent_session_output_item_child_run_id(output_item);

  if (!session || !session->runtime || !child_run_id || child_run_id[0] == '\0' ||
      !out_checkpoints_json) {
    return -1;
  }
  return turbo_agent_runtime_list_checkpoints(session->runtime, child_run_id, out_checkpoints_json);
}

CXX_C_API int turbo_agent_session_list_checkpoints(turbo_agent_session_t *session,
                                                   const char *run_id,
                                                   json_value_t **out_checkpoints_json) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_checkpoints_json) {
    return -1;
  }
  return turbo_agent_runtime_list_checkpoints(session->runtime, resolved_run_id,
                                              out_checkpoints_json);
}

CXX_C_API int turbo_agent_session_list_thread_lineage(turbo_agent_session_t *session,
                                                      json_value_t **out_lineage_json) {
  if (!session || !session->runtime || !session->thread_id || !out_lineage_json) {
    return -1;
  }
  return turbo_agent_runtime_list_thread_lineage(session->runtime, session->thread_id,
                                                 out_lineage_json);
}

CXX_C_API int turbo_agent_session_get_branch_tree(turbo_agent_session_t *session,
                                                  json_value_t **out_branch_tree_json) {
  if (!session || !session->runtime || !session->thread_id || !out_branch_tree_json) {
    return -1;
  }
  return turbo_agent_runtime_get_branch_tree(session->runtime, session->thread_id,
                                             out_branch_tree_json);
}

CXX_C_API int turbo_agent_session_get_checkpoint_context(
    turbo_agent_session_t *session, const char *checkpoint_id, json_value_t **out_context_json) {
  if (!session || !session->runtime || !checkpoint_id || !checkpoint_id[0] || !out_context_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_context(session->runtime, checkpoint_id,
                                                    out_context_json);
}

CXX_C_API int turbo_agent_session_load_history_events_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events) {
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !session->runtime || !out_events) {
    return -1;
  }
  if (run_id && run_id[0] != '\0') {
    resolved_run_id = run_id;
  } else if (checkpoint_id && checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = checkpoint_id;
  } else if (session->last_run_id && session->last_run_id[0] != '\0') {
    resolved_run_id = session->last_run_id;
  } else if (session->last_checkpoint_id && session->last_checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = session->last_checkpoint_id;
  } else {
    return -1;
  }
  return turbo_agent_runtime_load_history_events_bind(session->runtime, resolved_run_id,
                                                      resolved_checkpoint_id, out_events);
}

CXX_C_API int turbo_agent_session_load_thread_history_events_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_events) {
  if (!session || !session->runtime || !session->thread_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_load_thread_history_events_bind(session->runtime, session->thread_id,
                                                             out_events);
}

CXX_C_API int turbo_agent_session_replay_history_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data) {
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !session->runtime || !event_sink) {
    return -1;
  }
  if (run_id && run_id[0] != '\0') {
    resolved_run_id = run_id;
  } else if (checkpoint_id && checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = checkpoint_id;
  } else if (session->last_run_id && session->last_run_id[0] != '\0') {
    resolved_run_id = session->last_run_id;
  } else if (session->last_checkpoint_id && session->last_checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = session->last_checkpoint_id;
  } else {
    return -1;
  }
  return turbo_agent_runtime_replay_history_bind(session->runtime, resolved_run_id,
                                                 resolved_checkpoint_id, event_sink,
                                                 event_sink_user_data);
}

CXX_C_API int turbo_agent_session_replay_thread_history_bind(
    turbo_agent_session_t *session, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data) {
  if (!session || !session->runtime || !session->thread_id || !event_sink) {
    return -1;
  }
  return turbo_agent_runtime_replay_thread_history_bind(session->runtime, session->thread_id,
                                                        event_sink, event_sink_user_data);
}

CXX_C_API int turbo_agent_session_observe_history_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    const turbo_agent_observer_bind_sink_t *sink) {
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !session->runtime || !sink || !sink->callback) {
    return -1;
  }
  if (run_id && run_id[0] != '\0') {
    resolved_run_id = run_id;
  } else if (checkpoint_id && checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = checkpoint_id;
  } else if (session->last_run_id && session->last_run_id[0] != '\0') {
    resolved_run_id = session->last_run_id;
  } else if (session->last_checkpoint_id && session->last_checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = session->last_checkpoint_id;
  } else {
    return -1;
  }
  return turbo_agent_runtime_observe_history_bind(session->runtime, resolved_run_id,
                                                  resolved_checkpoint_id, sink);
}

CXX_C_API int turbo_agent_session_observe_thread_history_bind(
    turbo_agent_session_t *session, const turbo_agent_observer_bind_sink_t *sink) {
  if (!session || !session->runtime || !session->thread_id || !sink || !sink->callback) {
    return -1;
  }
  return turbo_agent_runtime_observe_thread_history_bind(session->runtime, session->thread_id,
                                                         sink);
}

CXX_C_API int turbo_agent_session_get_thread_timeline_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_timeline) {
  if (!session || !session->runtime || !session->thread_id || !out_timeline) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_timeline_bind(session->runtime, session->thread_id,
                                                      out_timeline);
}

CXX_C_API int turbo_agent_session_apply_command_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !command ||
      !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_apply_command_bind(session->runtime, resolved_checkpoint_id, command,
                                                out_state_override);
}

CXX_C_API int turbo_agent_session_apply_checkpoint_command_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!session || !session->runtime || !checkpoint_id || !checkpoint_id[0] || !command ||
      !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_apply_checkpoint_command_bind(session->runtime, checkpoint_id, command,
                                                           out_state_override);
}

CXX_C_API int turbo_agent_session_apply_thread_command_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!session || !session->runtime || !session->thread_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_apply_thread_command_bind(session->runtime, session->thread_id,
                                                       command, out_state_override);
}

CXX_C_API int turbo_agent_session_apply_state_patch_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !state_patch ||
      !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_apply_state_patch_bind(session->runtime, resolved_checkpoint_id,
                                                    state_patch, out_state_override);
}

CXX_C_API int turbo_agent_session_apply_checkpoint_state_patch_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!session || !session->runtime || !checkpoint_id || !checkpoint_id[0] || !state_patch ||
      !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_apply_checkpoint_state_patch_bind(session->runtime, checkpoint_id,
                                                               state_patch, out_state_override);
}

CXX_C_API int turbo_agent_session_apply_thread_state_patch_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  if (!session || !session->runtime || !session->thread_id || !state_patch || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_apply_thread_state_patch_bind(session->runtime, session->thread_id,
                                                           state_patch, out_state_override);
}

CXX_C_API int turbo_agent_session_resume_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  const char *resolved_checkpoint_id;
  int rc;

  if (!session || !session->runtime || !graph || !state_patch || !out_summary_json ||
      !out_state) {
    return -1;
  }
  resolved_checkpoint_id = turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);
  if (!resolved_checkpoint_id) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_state_patch_bind_graph(session->runtime, graph,
                                                         resolved_checkpoint_id, state_patch,
                                                         options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_checkpoint_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !checkpoint_id || !checkpoint_id[0] ||
      !state_patch || !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_checkpoint_state_patch_bind_graph(
      session->runtime, graph, checkpoint_id, state_patch, options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_checkpoint_state_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_session_resume_checkpoint_state_patch_bind_graph(
      session, graph, checkpoint_id, state_patch, options, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_resume_thread_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !session->thread_id || !state_patch ||
      !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_thread_state_patch_bind_graph(
      session->runtime, graph, session->thread_id, state_patch, options, out_summary_json,
      out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_thread_state_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_session_resume_thread_state_patch_bind_graph(
      session, graph, state_patch, options, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_fork_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  const char *resolved_checkpoint_id;
  int rc;

  if (!session || !session->runtime || !graph || !state_patch || !out_summary_json ||
      !out_state) {
    return -1;
  }
  resolved_checkpoint_id = turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);
  if (!resolved_checkpoint_id) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_state_patch_bind_graph(session->runtime, graph,
                                                       resolved_checkpoint_id, state_patch, options,
                                                       out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_checkpoint_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !checkpoint_id || !checkpoint_id[0] ||
      !state_patch || !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_checkpoint_state_patch_bind_graph(
      session->runtime, graph, checkpoint_id, state_patch, options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_thread_state_patch_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !session->thread_id || !state_patch ||
      !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_thread_state_patch_bind_graph(session->runtime, graph,
                                                              session->thread_id, state_patch,
                                                              options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_checkpoint_state_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_session_fork_checkpoint_state_patch_bind_graph(
      session, graph, checkpoint_id, state_patch, options, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_fork_thread_state_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_session_fork_thread_state_patch_bind_graph(
      session, graph, state_patch, options, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_resume_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  const char *resolved_checkpoint_id;
  int rc;

  if (!session || !session->runtime || !graph || !command || !out_summary_json || !out_state) {
    return -1;
  }
  resolved_checkpoint_id = turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);
  if (!resolved_checkpoint_id) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_command_bind(session->runtime, graph, resolved_checkpoint_id,
                                               command, options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_checkpoint_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !checkpoint_id || !checkpoint_id[0] || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_checkpoint_command_bind(session->runtime, graph, checkpoint_id,
                                                          command, options, out_summary_json,
                                                          out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_thread_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !session->thread_id || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_thread_command_bind(session->runtime, graph, session->thread_id,
                                                      command, options, out_summary_json,
                                                      out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  const char *resolved_checkpoint_id;
  int rc;

  if (!session || !session->runtime || !graph || !command || !out_summary_json || !out_state) {
    return -1;
  }
  resolved_checkpoint_id = turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);
  if (!resolved_checkpoint_id) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_command_bind(session->runtime, graph, resolved_checkpoint_id,
                                             command, options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_checkpoint_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !checkpoint_id || !checkpoint_id[0] || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_checkpoint_command_bind(session->runtime, graph, checkpoint_id,
                                                        command, options, out_summary_json,
                                                        out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_thread_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !session->thread_id || !command ||
      !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_thread_command_bind(session->runtime, graph, session->thread_id,
                                                    command, options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_load_child_history_events_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events) {
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !session->runtime || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_runtime_load_history_events_bind(session->runtime, NULL,
                                                        child_checkpoint_id, out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_runtime_load_history_events_bind(session->runtime, child_run_id, NULL,
                                                      out_events);
}

CXX_C_API int turbo_agent_session_get_child_trace_events_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events) {
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !session->runtime || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_runtime_get_checkpoint_trace_events_bind(session->runtime,
                                                                child_checkpoint_id, out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_runtime_get_run_trace_events_bind(session->runtime, child_run_id, out_events);
}

CXX_C_API int turbo_agent_session_start_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  turbo_agent_runtime_parent_link_t parent_link = {0};
  int rc;

  if (!session || !session->runtime || !graph || !state || !out_summary_json || !out_state) {
    return -1;
  }
  if (turbo_agent_session_has_parent_link(session, &parent_link)) {
    rc = turbo_agent_runtime_start_bind_graph_linked(session->runtime, graph, state, options,
                                                     session->thread_id, &parent_link,
                                                     out_summary_json, out_state);
  } else {
    rc = turbo_agent_runtime_start_bind_graph(session->runtime, graph, state, options,
                                              session->thread_id, out_summary_json, out_state);
  }
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  const char *resolved_checkpoint_id;
  int rc;

  if (!session || !session->runtime || !graph || !out_summary_json || !out_state) {
    return -1;
  }
  resolved_checkpoint_id = turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);
  if (!resolved_checkpoint_id) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_bind_graph(session->runtime, graph, resolved_checkpoint_id,
                                             state_override, options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_checkpoint_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !checkpoint_id || !*checkpoint_id ||
      !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_checkpoint_bind_graph(session->runtime, graph, checkpoint_id,
                                                        state_override, options, out_summary_json,
                                                        out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  const char *resolved_checkpoint_id;
  int rc;

  if (!session || !session->runtime || !graph || !out_summary_json || !out_state) {
    return -1;
  }
  resolved_checkpoint_id = turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);
  if (!resolved_checkpoint_id) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_bind_graph(session->runtime, graph, resolved_checkpoint_id,
                                           state_override, options, out_summary_json, out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_checkpoint_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !checkpoint_id || !*checkpoint_id ||
      !out_summary_json || !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_checkpoint_bind_graph(session->runtime, graph, checkpoint_id,
                                                      state_override, options, out_summary_json,
                                                      out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_thread_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !session->thread_id || !out_summary_json ||
      !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_resume_thread_bind_graph(session->runtime, graph, session->thread_id,
                                                    state_override, options, out_summary_json,
                                                    out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_thread_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  int rc;

  if (!session || !session->runtime || !graph || !session->thread_id || !out_summary_json ||
      !out_state) {
    return -1;
  }
  rc = turbo_agent_runtime_fork_thread_bind_graph(session->runtime, graph, session->thread_id,
                                                  state_override, options, out_summary_json,
                                                  out_state);
  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *out_summary_json);
  }
  return rc;
}

static turbo_graph_t *turbo_agent_session_create_graph_base(const turbo_agent_session_t *session,
                                                            const char *graph_name) {
  turbo_graph_t *graph;

  if (!session || !session->agent || !graph_name) {
    return NULL;
  }
  graph = turbo_graph_create(graph_name);
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_loop_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_loop(graph, session->agent, "model", "tools", "end", 1) !=
      TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_review_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-review-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_review_loop(graph, session->agent, session->agent, "planner",
                                      "plan_commit", "plan_step", "review", "executor", "tools",
                                      "plan_advance", "end", 1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_engineering_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-engineering-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_engineering_loop(
          graph, session->agent, session->agent, "planner", "plan_commit", "plan_step", "review",
          "executor", "tools", "detect_failure", "replan_route", "replan_prepare",
          "plan_advance", "end", 1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *turbo_agent_session_create_preset_graph(
    const turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind) {
  switch (kind) {
  case TURBO_AGENT_SESSION_WORKFLOW_LOOP:
    return turbo_agent_session_create_loop_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_REVIEW:
    return turbo_agent_session_create_review_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING:
    return turbo_agent_session_create_engineering_graph(session);
  default:
    return NULL;
  }
}

CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_session_create_input_state_bind(const char *user_text) {
  json_value_t *state;
  turbo_runtime_data_bind_value_t *bind_state;

  state = turbo_agent_state_create();
  if (!state) {
    return NULL;
  }
  if (user_text && user_text[0] != '\0' &&
      turbo_agent_state_add_user_message(state, user_text) != 0) {
    turbo_free_json(&state);
    return NULL;
  }
  bind_state = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bind_state;
}

CXX_C_API turbo_runtime_data_bind_value_t *turbo_agent_session_create_input_messages_state_bind(
    const turbo_runtime_data_bind_value_t *messages) {
  json_value_t *state;
  json_value_t *input;
  turbo_runtime_data_bind_value_t *bind_state = NULL;
  size_t i;

  if (!messages || turbo_runtime_data_bind_value_kind(messages) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return NULL;
  }

  state = turbo_agent_state_create();
  if (!state) {
    return NULL;
  }
  input = turbo_json_object_get(state, "input");
  if (!input || turbo_json_type(input) != TURBO_JSON_ARRAY) {
    turbo_free_json(&state);
    return NULL;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(messages); ++i) {
    const turbo_runtime_data_bind_value_t *message = turbo_runtime_data_bind_array_get(messages, i);
    json_value_t *message_json;

    if (!message || turbo_prompt_message_validate_bind(message) != TURBO_PROMPT_OK) {
      turbo_free_json(&state);
      return NULL;
    }
    message_json = turbo_runtime_data_bind_value_to_json(message);
    if (!message_json) {
      turbo_free_json(&state);
      return NULL;
    }
    turbo_json_array_add(input, message_json);
  }

  bind_state = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bind_state;
}

CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_session_create_input_state_with_memory_bind(const turbo_agent_session_t *session,
                                                        const char *user_text,
                                                        const char *namespace_prefix) {
  turbo_runtime_data_bind_value_t *state;
  json_value_t *json_state;
  turbo_runtime_data_bind_value_t *bound = NULL;

  state = turbo_agent_session_create_input_state_bind(user_text);
  if (!state) {
    return NULL;
  }
  json_state = turbo_runtime_data_bind_value_to_json(state);
  turbo_runtime_data_bind_value_destroy(state);
  if (!json_state) {
    return NULL;
  }
  if (turbo_agent_session_load_memory_context(session, json_state, namespace_prefix) != 0) {
    turbo_free_json(&json_state);
    return NULL;
  }
  bound = turbo_runtime_data_bind_value_from_json(json_state);
  turbo_free_json(&json_state);
  return bound;
}

CXX_C_API turbo_runtime_data_bind_value_t *turbo_agent_session_create_input_messages_state_with_memory_bind(
    const turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const char *namespace_prefix) {
  turbo_runtime_data_bind_value_t *state;
  json_value_t *json_state;
  turbo_runtime_data_bind_value_t *bound = NULL;

  state = turbo_agent_session_create_input_messages_state_bind(messages);
  if (!state) {
    return NULL;
  }
  json_state = turbo_runtime_data_bind_value_to_json(state);
  turbo_runtime_data_bind_value_destroy(state);
  if (!json_state) {
    return NULL;
  }
  if (turbo_agent_session_load_memory_context(session, json_state, namespace_prefix) != 0) {
    turbo_free_json(&json_state);
    return NULL;
  }
  bound = turbo_runtime_data_bind_value_from_json(json_state);
  turbo_free_json(&json_state);
  return bound;
}

CXX_C_API int turbo_agent_session_start_preset_bind_graph(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_start_bind_graph(session, graph, state, options, out_summary_json,
                                            out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_start_preset_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *state;
  int rc;

  state = turbo_agent_session_create_input_state_bind(user_text);
  if (!state) {
    return -1;
  }
  rc = turbo_agent_session_start_preset_bind_graph(session, kind, state, options, out_summary_json,
                                                   out_state);
  turbo_runtime_data_bind_value_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_session_start_preset_text_with_memory(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const char *namespace_prefix, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *state;
  int rc;

  state = turbo_agent_session_create_input_state_with_memory_bind(session, user_text,
                                                                  namespace_prefix);
  if (!state) {
    return -1;
  }
  rc = turbo_agent_session_start_preset_bind_graph(session, kind, state, options, out_summary_json,
                                                   out_state);
  turbo_runtime_data_bind_value_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_session_start_preset_messages(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *state;
  int rc;

  state = turbo_agent_session_create_input_messages_state_bind(messages);
  if (!state) {
    return -1;
  }
  rc = turbo_agent_session_start_preset_bind_graph(session, kind, state, options, out_summary_json,
                                                   out_state);
  turbo_runtime_data_bind_value_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_session_start_messages(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *state;
  int rc;

  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    state = turbo_agent_session_create_input_messages_state_with_memory_bind(
        session, messages, session->memory_namespace);
    if (!state) {
      return -1;
    }
    rc = turbo_agent_session_start_preset_bind_graph(session, session->workflow_kind, state,
                                                     options, out_summary_json, out_state);
    turbo_runtime_data_bind_value_destroy(state);
    return rc;
  }
  return turbo_agent_session_start_preset_messages(session, session->workflow_kind, messages,
                                                   options, out_summary_json, out_state);
}

CXX_C_API int turbo_agent_session_start_text(turbo_agent_session_t *session, const char *user_text,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_summary_json,
                                             turbo_runtime_data_bind_value_t **out_state) {
  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    return turbo_agent_session_start_preset_text_with_memory(
        session, session->workflow_kind, user_text, session->memory_namespace, options,
        out_summary_json, out_state);
  }
  return turbo_agent_session_start_preset_text(session, session->workflow_kind, user_text, options,
                                               out_summary_json, out_state);
}

CXX_C_API char *turbo_agent_session_result_text(
    const turbo_runtime_data_bind_value_t *state) {
  json_value_t *json_state;
  const char *text;
  char *copy = NULL;

  if (!state) {
    return NULL;
  }
  json_state = turbo_runtime_data_bind_value_to_json(state);
  if (!json_state) {
    return NULL;
  }
  text = turbo_agent_state_final_answer_text(json_state);
  if (text && text[0] != '\0') {
    copy = turbo_agent_util_strdup(text);
  }
  turbo_free_json(&json_state);
  return copy;
}

CXX_C_API int turbo_agent_session_invoke_preset_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  char *text;
  int rc;

  if (!out_text || !out_summary_json) {
    return -1;
  }
  *out_text = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_session_start_preset_text(session, kind, user_text, options, out_summary_json,
                                             &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  text = turbo_agent_session_result_text(out_state);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (!text) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  *out_text = text;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_preset_text_with_memory(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const char *namespace_prefix, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  char *text;
  int rc;

  if (!out_text || !out_summary_json) {
    return -1;
  }
  *out_text = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_session_start_preset_text_with_memory(session, kind, user_text,
                                                         namespace_prefix, options,
                                                         out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  text = turbo_agent_session_result_text(out_state);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (!text) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  *out_text = text;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_preset_messages_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  char *text;
  int rc;

  if (!out_text || !out_summary_json) {
    return -1;
  }
  *out_text = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_session_start_preset_messages(session, kind, messages, options,
                                                 out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  text = turbo_agent_session_result_text(out_state);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (!text) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  *out_text = text;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_messages_text(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *state;
  turbo_runtime_data_bind_value_t *out_state = NULL;
  char *text;
  int rc;

  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    if (!out_text || !out_summary_json) {
      return -1;
    }
    *out_text = NULL;
    *out_summary_json = NULL;
    state = turbo_agent_session_create_input_messages_state_with_memory_bind(
        session, messages, session->memory_namespace);
    if (!state) {
      return -1;
    }
    rc = turbo_agent_session_start_preset_bind_graph(session, session->workflow_kind, state,
                                                     options, out_summary_json, &out_state);
    turbo_runtime_data_bind_value_destroy(state);
    if (rc != 0) {
      turbo_runtime_data_bind_value_destroy(out_state);
      turbo_free_json(out_summary_json);
      return rc;
    }
    text = turbo_agent_session_result_text(out_state);
    turbo_runtime_data_bind_value_destroy(out_state);
    if (!text) {
      turbo_free_json(out_summary_json);
      return -1;
    }
    *out_text = text;
    return 0;
  }
  return turbo_agent_session_invoke_preset_messages_text(session, session->workflow_kind, messages,
                                                         options, out_text, out_summary_json);
}

CXX_C_API int turbo_agent_session_invoke_text(turbo_agent_session_t *session,
                                              const char *user_text,
                                              const turbo_graph_run_options_t *options,
                                              char **out_text,
                                              json_value_t **out_summary_json) {
  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    return turbo_agent_session_invoke_preset_text_with_memory(
        session, session->workflow_kind, user_text, session->memory_namespace, options, out_text,
        out_summary_json);
  }
  return turbo_agent_session_invoke_preset_text(session, session->workflow_kind, user_text,
                                                options, out_text, out_summary_json);
}

CXX_C_API int turbo_agent_session_invoke_preset_json(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  json_value_t *json = NULL;
  json_value_t *json_state = NULL;
  int rc;

  if (!out_json || !out_summary_json) {
    return -1;
  }
  *out_json = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_session_start_preset_text(session, kind, user_text, options, out_summary_json,
                                             &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  json_state = turbo_runtime_data_bind_value_to_json(out_state);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (!json_state) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  rc = turbo_agent_state_parse_final_output_json(json_state, &json);
  turbo_free_json(&json_state);
  if (rc != 0 || !json) {
    turbo_free_json(out_summary_json);
    turbo_free_json(&json);
    return -1;
  }
  *out_json = json;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_preset_json_with_memory(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const char *namespace_prefix, const turbo_graph_run_options_t *options,
    json_value_t **out_json, json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  json_value_t *json = NULL;
  json_value_t *json_state = NULL;
  int rc;

  if (!out_json || !out_summary_json) {
    return -1;
  }
  *out_json = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_session_start_preset_text_with_memory(session, kind, user_text,
                                                         namespace_prefix, options,
                                                         out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  json_state = turbo_runtime_data_bind_value_to_json(out_state);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (!json_state) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  rc = turbo_agent_state_parse_final_output_json(json_state, &json);
  turbo_free_json(&json_state);
  if (rc != 0 || !json) {
    turbo_free_json(out_summary_json);
    turbo_free_json(&json);
    return -1;
  }
  *out_json = json;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_preset_messages_json(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    json_value_t **out_json, json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *out_state = NULL;
  json_value_t *json = NULL;
  json_value_t *json_state = NULL;
  int rc;

  if (!out_json || !out_summary_json) {
    return -1;
  }
  *out_json = NULL;
  *out_summary_json = NULL;
  rc = turbo_agent_session_start_preset_messages(session, kind, messages, options,
                                                 out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_data_bind_value_destroy(out_state);
    turbo_free_json(out_summary_json);
    return rc;
  }
  json_state = turbo_runtime_data_bind_value_to_json(out_state);
  turbo_runtime_data_bind_value_destroy(out_state);
  if (!json_state) {
    turbo_free_json(out_summary_json);
    return -1;
  }
  rc = turbo_agent_state_parse_final_output_json(json_state, &json);
  turbo_free_json(&json_state);
  if (rc != 0 || !json) {
    turbo_free_json(out_summary_json);
    turbo_free_json(&json);
    return -1;
  }
  *out_json = json;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_messages_json(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  turbo_runtime_data_bind_value_t *state;
  turbo_runtime_data_bind_value_t *out_state = NULL;
  json_value_t *json = NULL;
  json_value_t *json_state = NULL;
  int rc;

  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    if (!out_json || !out_summary_json) {
      return -1;
    }
    *out_json = NULL;
    *out_summary_json = NULL;
    state = turbo_agent_session_create_input_messages_state_with_memory_bind(
        session, messages, session->memory_namespace);
    if (!state) {
      return -1;
    }
    rc = turbo_agent_session_start_preset_bind_graph(session, session->workflow_kind, state,
                                                     options, out_summary_json, &out_state);
    turbo_runtime_data_bind_value_destroy(state);
    if (rc != 0) {
      turbo_runtime_data_bind_value_destroy(out_state);
      turbo_free_json(out_summary_json);
      return rc;
    }
    json_state = turbo_runtime_data_bind_value_to_json(out_state);
    turbo_runtime_data_bind_value_destroy(out_state);
    if (!json_state) {
      turbo_free_json(out_summary_json);
      return -1;
    }
    rc = turbo_agent_state_parse_final_output_json(json_state, &json);
    turbo_free_json(&json_state);
    if (rc != 0 || !json) {
      turbo_free_json(out_summary_json);
      turbo_free_json(&json);
      return -1;
    }
    *out_json = json;
    return 0;
  }
  return turbo_agent_session_invoke_preset_messages_json(session, session->workflow_kind, messages,
                                                         options, out_json, out_summary_json);
}

CXX_C_API int turbo_agent_session_invoke_json(turbo_agent_session_t *session,
                                              const char *user_text,
                                              const turbo_graph_run_options_t *options,
                                              json_value_t **out_json,
                                              json_value_t **out_summary_json) {
  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    return turbo_agent_session_invoke_preset_json_with_memory(
        session, session->workflow_kind, user_text, session->memory_namespace, options, out_json,
        out_summary_json);
  }
  return turbo_agent_session_invoke_preset_json(session, session->workflow_kind, user_text,
                                                options, out_json, out_summary_json);
}

CXX_C_API int turbo_agent_session_memory_get(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             char **out_value_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->get) {
    return -1;
  }
  return turbo_agent_memory_get(store, memory_namespace, key, out_value_json);
}

CXX_C_API int turbo_agent_session_memory_put(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             const char *value_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->put) {
    return -1;
  }
  return turbo_agent_memory_put(store, memory_namespace, key, value_json);
}

CXX_C_API int turbo_agent_session_memory_put_context(const turbo_agent_session_t *session,
                                                     const char *memory_namespace,
                                                     const char *key, const char *scope,
                                                     const char *path, const char *text) {
  json_value_t *payload;
  char *serialized;
  int rc;

  if (!scope || scope[0] == '\0' || !text || text[0] == '\0') {
    return -1;
  }
  payload = turbo_json_create_object();
  if (!payload) {
    return -1;
  }
  turbo_json_object_set_string(payload, "scope", scope);
  turbo_json_object_set_string(payload, "path", path ? path : "");
  turbo_json_object_set_string(payload, "text", text);
  serialized = turbo_json_serialize(payload, NULL);
  turbo_free_json(&payload);
  if (!serialized) {
    return -1;
  }
  rc = turbo_agent_session_memory_put(session, memory_namespace, key, serialized);
  turbo_json_serialize_free(serialized);
  return rc;
}

CXX_C_API int turbo_agent_session_memory_delete(const turbo_agent_session_t *session,
                                                const char *memory_namespace, const char *key) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->remove) {
    return -1;
  }
  return turbo_agent_memory_delete(store, memory_namespace, key);
}

CXX_C_API int turbo_agent_session_memory_list(const turbo_agent_session_t *session,
                                              const char *namespace_prefix,
                                              json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->list) {
    return -1;
  }
  return turbo_agent_memory_list(store, namespace_prefix, out_records_json);
}

CXX_C_API int turbo_agent_session_memory_list_records(const turbo_agent_session_t *session,
                                                      const char *namespace_prefix,
                                                      json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || (!store->list && !store->query)) {
    return -1;
  }
  return turbo_agent_memory_list_records(store, namespace_prefix, out_records_json);
}

CXX_C_API int turbo_agent_session_memory_query_records(const turbo_agent_session_t *session,
                                                       const char *namespace_prefix,
                                                       const char *kind,
                                                       const char *key_prefix,
                                                       const char *text_substring,
                                                       json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || (!store->list && !store->query)) {
    return -1;
  }
  return turbo_agent_memory_query_records(store, namespace_prefix, kind, key_prefix,
                                          text_substring, out_records_json);
}

CXX_C_API int turbo_agent_session_load_memory_context(const turbo_agent_session_t *session,
                                                      json_value_t *state,
                                                      const char *namespace_prefix) {
  json_value_t *records = NULL;
  size_t i;
  int rc;

  if (!session || !state) {
    return -1;
  }
  rc = turbo_agent_session_memory_list(session, namespace_prefix, &records);
  if (rc != 0 || !records || turbo_json_type(records) != TURBO_JSON_ARRAY) {
    turbo_free_json(&records);
    return -1;
  }
  for (i = 0; i < turbo_json_array_size(records); ++i) {
    if (turbo_agent_session_load_memory_context_record(state, turbo_json_array_get(records, i)) != 0) {
      turbo_free_json(&records);
      return -1;
    }
  }
  turbo_free_json(&records);
  return 0;
}

CXX_C_API int turbo_agent_session_resume_preset_bind_graph(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *checkpoint_id, const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_resume_bind_graph(session, graph, checkpoint_id, state_override,
                                             options, out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_fork_preset_bind_graph(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *checkpoint_id, const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_fork_bind_graph(session, graph, checkpoint_id, state_override, options,
                                           out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_resume_preset_command_bind(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *checkpoint_id, const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_resume_command_bind(session, graph, checkpoint_id, command, options,
                                               out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_resume_thread_preset_command_bind(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_resume_thread_command_bind(session, graph, command, options,
                                                      out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_fork_preset_command_bind(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *checkpoint_id, const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_fork_command_bind(session, graph, checkpoint_id, command, options,
                                             out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_fork_thread_preset_command_bind(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_fork_thread_command_bind(session, graph, command, options,
                                                    out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}
