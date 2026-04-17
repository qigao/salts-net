#include "tinytest.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_state.h"

#include <string.h>

static int dummy_transport(const char *request_json, char **out_response_json, void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static int dummy_graph_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)ctx;
  (void)user_data;
  return 0;
}

spec("turbo agent graph api") {

  it("should expose workflow helpers through the graph-specific header") {
    turbo_graph_node_fn model_node = turbo_agent_model_node;
    turbo_graph_node_fn tool_node = turbo_agent_tool_node;
    turbo_graph_edge_predicate_fn has_tools = turbo_agent_has_pending_tool_calls;
    turbo_graph_edge_predicate_fn review_ok = turbo_agent_review_approved_predicate;

    check_not_null((void *)model_node);
    check_not_null((void *)tool_node);
    check_not_null((void *)has_tools);
    check_not_null((void *)review_ok);
  }

  it("should fail tool node on malformed pending tool call records") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    json_value_t *events;
    json_value_t *model_event;
    json_value_t *tool_calls;
    json_value_t *tool_call;
    turbo_graph_exec_ctx_t ctx = {0};

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    model_event = turbo_json_create_object();
    tool_calls = turbo_json_create_array();
    tool_call = turbo_json_create_object();
    check_not_null(model_event);
    check_not_null(tool_calls);
    check_not_null(tool_call);

    turbo_json_object_set_string(model_event, "kind", "model");
    turbo_json_object_set_string(model_event, "output_text", "");
    turbo_json_object_set_string(tool_call, "call_id", "call_1");
    turbo_json_array_add(tool_calls, tool_call);
    turbo_json_object_add(model_event, "tool_calls", tool_calls);
    turbo_json_array_add(events, model_event);

    ctx.state = state;
    check_int_ne(turbo_agent_tool_node(&ctx, agent), 0);
    check_str_eq(turbo_agent_state_model_error_phase(state), "tool");
    check_str_eq(turbo_agent_state_model_error_detail(state), "malformed pending tool call record");

    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should mark failure and request replan when detect_failed_step sees malformed tool results") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *tool_results;
    json_value_t *outputs;
    json_value_t *output_item;
    turbo_graph_exec_ctx_t ctx = {0};

    check_not_null(state);
    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    tool_results = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(tool_results);
    check_not_null(outputs);
    check_not_null(output_item);

    turbo_json_object_set_string(tool_results, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_object_set_string(output_item, "output", "not-json");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results, "outputs", outputs);
    turbo_json_array_add(executor_events, tool_results);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    ctx.state = state;
    check_int_eq(turbo_agent_detect_failed_step_node(&ctx, NULL), 0);
    check_true(turbo_agent_state_replan_requested(state));
    check_str_eq(turbo_agent_state_failure_kind(state), "tool_result");
    check_str_eq(turbo_agent_state_failure_reason(state), "tool output was not valid JSON");
    check_str_eq(turbo_agent_state_replan_reason(state), "tool output was not valid JSON");

    turbo_free_json(&state);
  }

  it("should stop review node when review is required and not approved") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state = turbo_agent_state_create();
    turbo_graph_exec_ctx_t ctx = {0};

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    agent = turbo_agent_create(&config);
    check_not_null(agent);
    check_not_null(state);
    check_int_eq(turbo_agent_state_request_review(state, "needs human check"), 0);
    check_int_eq(turbo_agent_state_set_review_approved(state, 0), 0);

    ctx.state = state;
    check_int_eq(turbo_agent_review_node(&ctx, agent), 0);
    check_true(ctx.stop != 0);

    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should route to the requested replan node when replanning is pending") {
    json_value_t *state = turbo_agent_state_create();
    turbo_graph_t *graph = turbo_graph_create("replan-route");
    turbo_graph_exec_ctx_t ctx = {0};

    check_not_null(state);
    check_not_null(graph);
    check_int_eq(turbo_graph_add_node(graph, "replan", dummy_graph_node, NULL),
                 TURBO_GRAPH_EXEC_OK);
    check_int_eq(turbo_agent_state_request_replan(state, "bad output"), 0);

    ctx.graph = graph;
    ctx.state = state;
    check_int_eq(turbo_agent_replan_route_node(&ctx, "replan"), 0);
    check_str_eq(ctx.next_node, "replan");

    turbo_graph_destroy(graph);
    turbo_free_json(&state);
  }

  it("should mark executor text failures and request replan when failure markers are present") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *model_event;
    turbo_graph_exec_ctx_t ctx = {0};

    check_not_null(state);
    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    model_event = turbo_json_create_object();
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(model_event);

    turbo_json_object_set_string(model_event, "kind", "model");
    turbo_json_object_set_string(model_event, "output_text", "FAILED: executor could not finish");
    turbo_json_array_add(executor_events, model_event);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    ctx.state = state;
    check_int_eq(turbo_agent_detect_failed_step_node(&ctx, NULL), 0);
    check_true(turbo_agent_state_replan_requested(state));
    check_str_eq(turbo_agent_state_failure_kind(state), "executor_text");
    check_str_eq(turbo_agent_state_failure_reason(state), "FAILED: executor could not finish");
    check_str_eq(turbo_agent_state_replan_reason(state), "FAILED: executor could not finish");

    turbo_free_json(&state);
  }

  it("should report pending tool calls only when the latest model event carries tool calls") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *events;
    json_value_t *model_event;
    json_value_t *tool_calls;
    json_value_t *tool_call;
    turbo_graph_exec_ctx_t ctx = {0};

    check_not_null(state);
    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    model_event = turbo_json_create_object();
    tool_calls = turbo_json_create_array();
    tool_call = turbo_json_create_object();
    check_not_null(model_event);
    check_not_null(tool_calls);
    check_not_null(tool_call);

    turbo_json_object_set_string(model_event, "kind", "model");
    turbo_json_object_set_string(tool_call, "call_id", "call_1");
    turbo_json_object_set_string(tool_call, "name", "sum");
    turbo_json_object_set_string(tool_call, "arguments", "{\"a\":2}");
    turbo_json_array_add(tool_calls, tool_call);
    turbo_json_object_add(model_event, "tool_calls", tool_calls);
    turbo_json_array_add(events, model_event);

    ctx.state = state;
    check_true(turbo_agent_has_pending_tool_calls(&ctx, NULL));

    turbo_free_json(&state);
  }

  it("should report review approval through the public predicate") {
    json_value_t *state = turbo_agent_state_create();
    turbo_graph_exec_ctx_t ctx = {0};

    check_not_null(state);
    check_int_eq(turbo_agent_state_request_review(state, "ok"), 0);
    check_int_eq(turbo_agent_state_set_review_approved(state, 1), 0);

    ctx.state = state;
    check_true(turbo_agent_review_approved_predicate(&ctx, NULL));

    turbo_free_json(&state);
  }

  it("should stop and set final answer when detect_failed_step exceeds the replan limit") {
    json_value_t *state = turbo_agent_state_create();
    json_value_t *executor_versions;
    json_value_t *executor_events;
    json_value_t *model_event;
    turbo_graph_exec_ctx_t ctx = {0};
    const char *final_answer;

    check_not_null(state);
    check_int_eq(turbo_agent_state_set_replan_limit(state, 1), 0);
    check_int_eq(turbo_agent_state_request_replan(state, "first failure"), 0);

    executor_versions = turbo_json_create_array();
    executor_events = turbo_json_create_array();
    model_event = turbo_json_create_object();
    check_not_null(executor_versions);
    check_not_null(executor_events);
    check_not_null(model_event);

    turbo_json_object_set_string(model_event, "kind", "model");
    turbo_json_object_set_string(model_event, "output_text", "FAILED: executor could not finish");
    turbo_json_array_add(executor_events, model_event);
    turbo_json_array_add(executor_versions, executor_events);
    turbo_json_object_add(state, "executor_event_versions", executor_versions);

    ctx.state = state;
    check_int_eq(turbo_agent_detect_failed_step_node(&ctx, NULL), 0);
    check_true(ctx.stop != 0);
    check_str_eq(turbo_agent_state_failure_kind(state), "limit");
    check_str_eq(turbo_agent_state_failure_reason(state), "maximum replans exceeded");
    final_answer = turbo_agent_state_final_answer_text(state);
    check_not_null(final_answer);
    check_true(strstr(final_answer, "FAILED: maximum replans exceeded after failure: ") == final_answer);

    turbo_free_json(&state);
  }
}


