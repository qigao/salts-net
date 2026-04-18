#include "tinytest.h"

#include "turbo_agent_subagent.h"
#include "turbo_parser.h"

#include <stdlib.h>
#include <string.h>

static int subagent_success_transport(const char *request_json, char **out_response_json,
                                      void *user_data) {
  const char *response =
      "{\"id\":\"resp_subagent\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}";
  char *copy;
  (void)request_json;
  (void)user_data;

  if (!out_response_json) {
    return -1;
  }
  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_response_json = copy;
  return 0;
}

static int subagent_json_transport(const char *request_json, char **out_response_json,
                                   void *user_data) {
  const char *response =
      "{\"id\":\"resp_subagent_json\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"ok\\\":true,\\\"value\\\":42}\"}]}]}";
  char *copy;
  (void)request_json;
  (void)user_data;

  if (!out_response_json) {
    return -1;
  }
  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_response_json = copy;
  return 0;
}

static turbo_runtime_data_bind_value_t *subagent_create_text_args(const char *text) {
  turbo_runtime_data_bind_value_t *arguments = turbo_runtime_data_bind_value_create_object();

  if (!arguments) {
    return NULL;
  }
  check_int_eq(turbo_runtime_data_bind_object_set(
                   arguments, "input", turbo_runtime_data_bind_value_create_string(text)),
               TURBO_RUNTIME_DATA_BIND_OK);
  return arguments;
}

static turbo_runtime_data_bind_value_t *subagent_create_messages_args(void) {
  turbo_runtime_data_bind_value_t *arguments = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *messages = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *system_message = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *user_message = turbo_runtime_data_bind_value_create_object();

  if (!arguments || !messages || !system_message || !user_message) {
    turbo_runtime_data_bind_value_destroy(arguments);
    turbo_runtime_data_bind_value_destroy(messages);
    turbo_runtime_data_bind_value_destroy(system_message);
    turbo_runtime_data_bind_value_destroy(user_message);
    return NULL;
  }

  check_int_eq(turbo_runtime_data_bind_object_set(
                   system_message, "role", turbo_runtime_data_bind_value_create_string("system")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   system_message, "content",
                   turbo_runtime_data_bind_value_create_string("Be terse.")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   user_message, "role", turbo_runtime_data_bind_value_create_string("user")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   user_message, "content",
                   turbo_runtime_data_bind_value_create_string("hello from tool")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(messages, system_message),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(messages, user_message),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(arguments, "messages", messages),
               TURBO_RUNTIME_DATA_BIND_OK);
  return arguments;
}

spec("turbo agent subagent api") {

  it("should reuse thread lineage when one shared app tool is invoked repeatedly") {
    turbo_tool_runtime_t *runtime = turbo_tool_runtime_native_create();
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_subagent_tool_config_t tool_config = {0};
    turbo_runtime_data_bind_value_t *args1 = NULL;
    turbo_runtime_data_bind_value_t *args2 = NULL;
    turbo_runtime_data_bind_value_t *result1 = NULL;
    turbo_runtime_data_bind_value_t *result2 = NULL;
    const char *thread1;
    const char *thread2;
    const char *run1;
    const char *run2;
    const char *child_thread1;
    const char *child_run1;
    const char *child_status1;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = subagent_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    tool_config.name = "delegate";
    tool_config.description = "Delegate one task to a shared subagent.";
    tool_config.mode = TURBO_AGENT_SUBAGENT_SHARED_APP;
    tool_config.result_kind = TURBO_AGENT_SUBAGENT_RESULT_TEXT;
    tool_config.session_config = &session_config;

    check_not_null(runtime);
    check_int_eq(turbo_agent_subagent_add_tool_runtime(runtime, &tool_config), TURBO_TOOL_OK);

    args1 = subagent_create_text_args("first");
    args2 = subagent_create_text_args("second");
    check_not_null(args1);
    check_not_null(args2);

    check_int_eq(turbo_tool_runtime_invoke_bind(runtime, "delegate", args1, &result1),
                 TURBO_TOOL_OK);
    check_int_eq(turbo_tool_runtime_invoke_bind(runtime, "delegate", args2, &result2),
                 TURBO_TOOL_OK);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(result1, "output_text")),
                 "ok");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(result2, "output_text")),
                 "ok");

    thread1 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result1, "thread_id"));
    thread2 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result2, "thread_id"));
    run1 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result1, "run_id"));
    run2 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result2, "run_id"));
    child_thread1 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result1, "child_thread_id"));
    child_run1 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result1, "child_run_id"));
    child_status1 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result1, "child_status"));

    check_not_null(thread1);
    check_not_null(thread2);
    check_not_null(run1);
    check_not_null(run2);
    check_str_eq(thread1, thread2);
    check(strcmp(run1, run2) != 0);
    check_str_eq(child_thread1, thread1);
    check_str_eq(child_run1, run1);
    check_str_eq(child_status1, "completed");
    check_int_eq(turbo_runtime_data_bind_value_kind(
                     turbo_runtime_data_bind_object_get(result1, "checkpoint_id")),
                 TURBO_RUNTIME_DATA_BIND_VALUE_NULL);
    check_int_eq(turbo_runtime_data_bind_value_kind(
                     turbo_runtime_data_bind_object_get(result1, "child_checkpoint_id")),
                 TURBO_RUNTIME_DATA_BIND_VALUE_NULL);

    turbo_runtime_data_bind_value_destroy(result2);
    turbo_runtime_data_bind_value_destroy(result1);
    turbo_runtime_data_bind_value_destroy(args2);
    turbo_runtime_data_bind_value_destroy(args1);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should create a fresh lineage for each stateless subagent tool call") {
    turbo_tool_runtime_t *runtime = turbo_tool_runtime_native_create();
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_subagent_tool_config_t tool_config = {0};
    turbo_runtime_data_bind_value_t *args1 = NULL;
    turbo_runtime_data_bind_value_t *args2 = NULL;
    turbo_runtime_data_bind_value_t *result1 = NULL;
    turbo_runtime_data_bind_value_t *result2 = NULL;
    const char *thread1;
    const char *thread2;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = subagent_success_transport;
    session_config.agent_config = agent_config;
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    tool_config.name = "delegate_once";
    tool_config.description = "Delegate one task to a stateless subagent.";
    tool_config.mode = TURBO_AGENT_SUBAGENT_STATELESS;
    tool_config.result_kind = TURBO_AGENT_SUBAGENT_RESULT_TEXT;
    tool_config.session_config = &session_config;

    check_not_null(runtime);
    check_int_eq(turbo_agent_subagent_add_tool_runtime(runtime, &tool_config), TURBO_TOOL_OK);

    args1 = subagent_create_text_args("first");
    args2 = subagent_create_text_args("second");
    check_not_null(args1);
    check_not_null(args2);

    check_int_eq(turbo_tool_runtime_invoke_bind(runtime, "delegate_once", args1, &result1),
                 TURBO_TOOL_OK);
    check_int_eq(turbo_tool_runtime_invoke_bind(runtime, "delegate_once", args2, &result2),
                 TURBO_TOOL_OK);

    thread1 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result1, "thread_id"));
    thread2 = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(result2, "thread_id"));
    check_not_null(thread1);
    check_not_null(thread2);
    check(strcmp(thread1, thread2) != 0);

    turbo_runtime_data_bind_value_destroy(result2);
    turbo_runtime_data_bind_value_destroy(result1);
    turbo_runtime_data_bind_value_destroy(args2);
    turbo_runtime_data_bind_value_destroy(args1);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should add one json-returning subagent tool directly to a registry") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_subagent_tool_config_t tool_config = {0};
    char *output = NULL;
    json_value_t *result_json = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = subagent_json_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    tool_config.name = "delegate_json";
    tool_config.description = "Delegate one task and return structured JSON.";
    tool_config.mode = TURBO_AGENT_SUBAGENT_SHARED_APP;
    tool_config.result_kind = TURBO_AGENT_SUBAGENT_RESULT_JSON;
    tool_config.session_config = &session_config;

    check_not_null(registry);
    check_int_eq(turbo_agent_subagent_add_tool_registry(registry, &tool_config), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_execute(registry, "delegate_json", "{\"input\":\"hello\"}",
                                             &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(
        turbo_parse_json((const uint8_t *)output, strlen(output), &result_json), 0);
    check_true(turbo_json_get_bool(result_json, "ok", false));
    check_str_eq(turbo_json_get_string(result_json, "status"), "completed");
    check_int_eq(turbo_json_get_int(turbo_json_object_get(result_json, "output_json"), "value", 0),
                 42);
    check_not_null(turbo_json_get_string(result_json, "thread_id"));
    check_not_null(turbo_json_get_string(result_json, "run_id"));
    check_str_eq(turbo_json_get_string(result_json, "child_status"), "completed");
    check_not_null(turbo_json_get_string(result_json, "child_thread_id"));
    check_not_null(turbo_json_get_string(result_json, "child_run_id"));
    check_true(turbo_json_object_get(result_json, "child_checkpoint_id") != NULL);
    check(strstr(turbo_json_get_string(result_json, "output_text"), "\"value\":42") != NULL);

    turbo_free_json(&result_json);
    turbo_json_serialize_free(output);
    turbo_tool_registry_destroy(registry);
  }

  it("should forward canonical messages into a shared subagent tool") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_subagent_tool_config_t tool_config = {0};
    turbo_runtime_data_bind_value_t *args = NULL;
    turbo_runtime_data_bind_value_t *result = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = subagent_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    tool_config.name = "delegate_messages";
    tool_config.description = "Delegate one canonical-message task.";
    tool_config.mode = TURBO_AGENT_SUBAGENT_SHARED_APP;
    tool_config.result_kind = TURBO_AGENT_SUBAGENT_RESULT_TEXT;
    tool_config.session_config = &session_config;

    check_not_null(registry);
    check_int_eq(turbo_agent_subagent_add_tool_registry(registry, &tool_config), TURBO_TOOL_OK);

    args = subagent_create_messages_args();
    check_not_null(args);
    check_int_eq(turbo_tool_registry_execute_bind(registry, "delegate_messages", args, &result),
                 TURBO_TOOL_OK);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(result, "status")),
                 "completed");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(result, "output_text")),
                 "ok");

    turbo_runtime_data_bind_value_destroy(result);
    turbo_runtime_data_bind_value_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("should expose top-level child lineage fields consistently with summary") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_subagent_tool_config_t tool_config = {0};
    char *output = NULL;
    json_value_t *result_json = NULL;
    const json_value_t *summary;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = subagent_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    tool_config.name = "delegate_child_surface";
    tool_config.description = "Return one subagent result with canonical child lineage.";
    tool_config.mode = TURBO_AGENT_SUBAGENT_SHARED_APP;
    tool_config.result_kind = TURBO_AGENT_SUBAGENT_RESULT_TEXT;
    tool_config.session_config = &session_config;

    check_not_null(registry);
    check_int_eq(turbo_agent_subagent_add_tool_registry(registry, &tool_config), TURBO_TOOL_OK);
    check_int_eq(
        turbo_tool_registry_execute(registry, "delegate_child_surface", "{\"input\":\"hello\"}",
                                    &output),
        TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(
        turbo_parse_json((const uint8_t *)output, strlen(output), &result_json), 0);
    check_not_null(result_json);
    summary = turbo_json_object_get(result_json, "summary");
    check_not_null(summary);
    check_str_eq(turbo_json_get_string(result_json, "child_thread_id"),
                 turbo_json_get_string(result_json, "thread_id"));
    check_str_eq(turbo_json_get_string(result_json, "child_run_id"),
                 turbo_json_get_string(result_json, "run_id"));
    check_str_eq(turbo_json_get_string(result_json, "child_status"),
                 turbo_json_get_string(result_json, "status"));
    check_str_eq(turbo_json_get_string(result_json, "child_thread_id"),
                 turbo_json_get_string(summary, "thread_id"));
    check_str_eq(turbo_json_get_string(result_json, "child_run_id"),
                 turbo_json_get_string(summary, "run_id"));
    check_str_eq(turbo_json_get_string(result_json, "child_status"),
                 turbo_json_get_string(summary, "status"));
    check_true(turbo_json_object_get(result_json, "active_agent") != NULL);
    check_true(turbo_json_object_get(result_json, "handoff_target_agent") != NULL);
    check_true(turbo_json_object_get(result_json, "handoff_reason") != NULL);
    check_true(turbo_json_object_get(summary, "active_agent") != NULL);
    check_true(turbo_json_object_get(summary, "handoff_target_agent") != NULL);
    check_true(turbo_json_object_get(summary, "handoff_reason") != NULL);
    check_null(turbo_json_get_string(result_json, "active_agent"));
    check_null(turbo_json_get_string(result_json, "handoff_target_agent"));
    check_null(turbo_json_get_string(result_json, "handoff_reason"));
    check_null(turbo_json_get_string(summary, "active_agent"));
    check_null(turbo_json_get_string(summary, "handoff_target_agent"));
    check_null(turbo_json_get_string(summary, "handoff_reason"));
    check_true(turbo_json_object_get(result_json, "child_checkpoint_id") != NULL);
    check_true(turbo_json_object_get(summary, "checkpoint_id") != NULL);

    turbo_free_json(&result_json);
    turbo_json_serialize_free(output);
    turbo_tool_registry_destroy(registry);
  }

  it("should expose nullable parent lineage fields on top-level subagent results") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_subagent_tool_config_t tool_config = {0};
    char *output = NULL;
    json_value_t *result_json = NULL;
    const json_value_t *summary;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = subagent_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    tool_config.name = "delegate_null_parent_surface";
    tool_config.description = "Return one subagent result with nullable parent lineage.";
    tool_config.mode = TURBO_AGENT_SUBAGENT_SHARED_APP;
    tool_config.result_kind = TURBO_AGENT_SUBAGENT_RESULT_TEXT;
    tool_config.session_config = &session_config;

    check_not_null(registry);
    check_int_eq(turbo_agent_subagent_add_tool_registry(registry, &tool_config), TURBO_TOOL_OK);
    check_int_eq(
        turbo_tool_registry_execute(registry, "delegate_null_parent_surface",
                                    "{\"input\":\"hello\"}", &output),
        TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(
        turbo_parse_json((const uint8_t *)output, strlen(output), &result_json), 0);
    check_not_null(result_json);
    summary = turbo_json_object_get(result_json, "summary");
    check_not_null(summary);
    check_true(turbo_json_object_get(result_json, "parent_agent_run_id") != NULL);
    check_true(turbo_json_object_get(result_json, "parent_tool_call_id") != NULL);
    check_true(turbo_json_object_get(result_json, "parent_tool_name") != NULL);
    check_true(turbo_json_object_get(result_json, "parent_graph_run_id") != NULL);
    check_true(turbo_json_object_get(result_json, "call_frame_id") != NULL);
    check_true(turbo_json_object_get(summary, "parent_agent_run_id") != NULL);
    check_true(turbo_json_object_get(summary, "parent_tool_call_id") != NULL);
    check_true(turbo_json_object_get(summary, "parent_tool_name") != NULL);
    check_true(turbo_json_object_get(summary, "parent_graph_run_id") != NULL);
    check_true(turbo_json_object_get(summary, "call_frame_id") != NULL);
    check_true(turbo_json_object_get(result_json, "active_agent") != NULL);
    check_true(turbo_json_object_get(result_json, "handoff_target_agent") != NULL);
    check_true(turbo_json_object_get(result_json, "handoff_reason") != NULL);
    check_true(turbo_json_object_get(summary, "active_agent") != NULL);
    check_true(turbo_json_object_get(summary, "handoff_target_agent") != NULL);
    check_true(turbo_json_object_get(summary, "handoff_reason") != NULL);
    check_null(turbo_json_get_string(result_json, "parent_agent_run_id"));
    check_null(turbo_json_get_string(result_json, "parent_tool_call_id"));
    check_null(turbo_json_get_string(result_json, "parent_tool_name"));
    check_null(turbo_json_get_string(result_json, "parent_graph_run_id"));
    check_null(turbo_json_get_string(result_json, "call_frame_id"));
    check_null(turbo_json_get_string(summary, "parent_agent_run_id"));
    check_null(turbo_json_get_string(summary, "parent_tool_call_id"));
    check_null(turbo_json_get_string(summary, "parent_tool_name"));
    check_null(turbo_json_get_string(summary, "parent_graph_run_id"));
    check_null(turbo_json_get_string(summary, "call_frame_id"));
    check_null(turbo_json_get_string(result_json, "active_agent"));
    check_null(turbo_json_get_string(result_json, "handoff_target_agent"));
    check_null(turbo_json_get_string(result_json, "handoff_reason"));
    check_null(turbo_json_get_string(summary, "active_agent"));
    check_null(turbo_json_get_string(summary, "handoff_target_agent"));
    check_null(turbo_json_get_string(summary, "handoff_reason"));

    turbo_free_json(&result_json);
    turbo_json_serialize_free(output);
    turbo_tool_registry_destroy(registry);
  }

  it("should expose configured parent lineage on top-level subagent results") {
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_subagent_tool_config_t tool_config = {0};
    char *output = NULL;
    json_value_t *result_json = NULL;
    const json_value_t *summary;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = subagent_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session_config.parent_agent_run_id = "run_parent";
    session_config.parent_tool_call_id = "call_parent";
    session_config.parent_tool_name = "delegate";
    session_config.parent_graph_run_id = "run_graph_parent";
    session_config.call_frame_id = "frame_parent";
    tool_config.name = "delegate_parent_surface";
    tool_config.description = "Return one subagent result with configured parent lineage.";
    tool_config.mode = TURBO_AGENT_SUBAGENT_SHARED_APP;
    tool_config.result_kind = TURBO_AGENT_SUBAGENT_RESULT_TEXT;
    tool_config.session_config = &session_config;

    check_not_null(registry);
    check_int_eq(turbo_agent_subagent_add_tool_registry(registry, &tool_config), TURBO_TOOL_OK);
    check_int_eq(
        turbo_tool_registry_execute(registry, "delegate_parent_surface", "{\"input\":\"hello\"}",
                                    &output),
        TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(
        turbo_parse_json((const uint8_t *)output, strlen(output), &result_json), 0);
    check_not_null(result_json);
    summary = turbo_json_object_get(result_json, "summary");
    check_not_null(summary);
    check_str_eq(turbo_json_get_string(result_json, "parent_agent_run_id"), "run_parent");
    check_str_eq(turbo_json_get_string(result_json, "parent_tool_call_id"), "call_parent");
    check_str_eq(turbo_json_get_string(result_json, "parent_tool_name"), "delegate");
    check_str_eq(turbo_json_get_string(result_json, "parent_graph_run_id"),
                 "run_graph_parent");
    check_str_eq(turbo_json_get_string(result_json, "call_frame_id"), "frame_parent");
    check_str_eq(turbo_json_get_string(result_json, "parent_agent_run_id"),
                 turbo_json_get_string(summary, "parent_agent_run_id"));
    check_str_eq(turbo_json_get_string(result_json, "parent_tool_call_id"),
                 turbo_json_get_string(summary, "parent_tool_call_id"));
    check_str_eq(turbo_json_get_string(result_json, "parent_tool_name"),
                 turbo_json_get_string(summary, "parent_tool_name"));
    check_str_eq(turbo_json_get_string(result_json, "parent_graph_run_id"),
                 turbo_json_get_string(summary, "parent_graph_run_id"));
    check_str_eq(turbo_json_get_string(result_json, "call_frame_id"),
                 turbo_json_get_string(summary, "call_frame_id"));

    turbo_free_json(&result_json);
    turbo_json_serialize_free(output);
    turbo_tool_registry_destroy(registry);
  }
}
