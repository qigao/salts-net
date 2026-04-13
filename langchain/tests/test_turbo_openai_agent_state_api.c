#include "tinytest.h"
#include "turbo_event_log.h"
#include "turbo_openai_agent_state.h"

spec("turbo openai agent state api") {

  it("should expose state helpers through the state-specific header") {
    void *schema_version = (void *)turbo_openai_agent_state_schema_version;
    void *create_state = (void *)turbo_openai_agent_state_create;
    void *create_state_bind = (void *)turbo_openai_agent_state_create_bind;
    void *events = (void *)turbo_openai_agent_state_events;
    void *plan = (void *)turbo_openai_agent_state_plan;
    void *control_snapshot = (void *)turbo_openai_agent_state_control_snapshot;
    void *control_snapshot_bind = (void *)turbo_openai_agent_state_control_snapshot_bind;
    void *trace_events_bind = (void *)turbo_openai_agent_state_trace_events_bind;
    void *workflow_snapshot_bind = (void *)turbo_openai_agent_state_workflow_snapshot_bind;
    void *final_answer = (void *)turbo_openai_agent_state_final_answer_text;

    check_not_null(schema_version);
    check_not_null(create_state);
    check_not_null(create_state_bind);
    check_not_null(events);
    check_not_null(plan);
    check_not_null(control_snapshot);
    check_not_null(control_snapshot_bind);
    check_not_null(trace_events_bind);
    check_not_null(workflow_snapshot_bind);
    check_not_null(final_answer);
  }

  it("should build state and snapshots through runtime data bind") {
    turbo_runtime_data_bind_value_t *state = turbo_openai_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *control = NULL;
    turbo_runtime_data_bind_value_t *workflow = NULL;

    check_not_null(state);
    check_true(
        turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_object_get(state, "state_version"), 0) > 0);

    control = turbo_openai_agent_state_control_snapshot_bind(state);
    workflow = turbo_openai_agent_state_workflow_snapshot_bind(state);

    check_not_null(control);
    check_not_null(workflow);
    check_true(
        turbo_runtime_data_bind_value_as_int64(
            turbo_runtime_data_bind_object_get(control, "state_version"), 0) > 0);
    check_not_null(turbo_runtime_data_bind_object_get(workflow, "control"));
    check_not_null(turbo_runtime_data_bind_object_get(workflow, "events"));

    turbo_runtime_data_bind_value_destroy(workflow);
    turbo_runtime_data_bind_value_destroy(control);
    turbo_runtime_data_bind_value_destroy(state);
  }

  it("should round-trip bind trace events from state into an event log") {
    turbo_runtime_data_bind_value_t *state = turbo_openai_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *trace_events = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *bound_events = NULL;
    turbo_event_log_t *log = turbo_event_log_create();

    check_not_null(state);
    check_not_null(trace_events);
    check_not_null(log);
    check_int_eq(
        turbo_runtime_data_bind_array_append(
            trace_events, turbo_event_trace_create_bind("agent.trace", "start", "a", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_runtime_data_bind_array_append(
            trace_events, turbo_event_trace_create_bind("agent.trace", "finish", "a", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(state, "trace_events", trace_events),
                 TURBO_RUNTIME_DATA_BIND_OK);

    bound_events = turbo_openai_agent_state_trace_events_bind(state);
    check_not_null(bound_events);
    check_int_eq(turbo_event_log_load_events_bind(log, bound_events), TURBO_EVENT_LOG_OK);
    check_size_eq(turbo_event_log_size(log), 2);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(turbo_event_log_get(log, 1), "detail")),
                 "finish");

    turbo_event_log_destroy(log);
    turbo_runtime_data_bind_value_destroy(bound_events);
    turbo_runtime_data_bind_value_destroy(state);
  }

  it("should append canonical trace events directly into bind-native state") {
    turbo_runtime_data_bind_value_t *state = turbo_openai_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *event =
        turbo_event_trace_create_bind("agent.trace", "finish", "done", 0);
    turbo_runtime_data_bind_value_t *bound_events = NULL;

    check_not_null(state);
    check_not_null(event);
    check_int_eq(turbo_openai_agent_state_add_trace_event_bind(state, event), 0);

    bound_events = turbo_openai_agent_state_trace_events_bind(state);
    check_not_null(bound_events);
    check_size_eq(turbo_runtime_data_bind_value_size(bound_events), 1);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_array_get(bound_events, 0), "detail")),
                 "finish");

    turbo_runtime_data_bind_value_destroy(bound_events);
    turbo_runtime_data_bind_value_destroy(event);
    turbo_runtime_data_bind_value_destroy(state);
  }

  it("should capture canonical trace events through the bind sink helper") {
    turbo_runtime_data_bind_value_t *state = turbo_openai_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *event =
        turbo_event_trace_create_bind("agent.trace", "start", "run", 0);
    turbo_runtime_data_bind_value_t *bound_events = NULL;

    check_not_null(state);
    check_not_null(event);
    turbo_openai_agent_state_capture_trace_event_bind(event, state);

    bound_events = turbo_openai_agent_state_trace_events_bind(state);
    check_not_null(bound_events);
    check_size_eq(turbo_runtime_data_bind_value_size(bound_events), 1);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_array_get(bound_events, 0), "detail")),
                 "start");

    turbo_runtime_data_bind_value_destroy(bound_events);
    turbo_runtime_data_bind_value_destroy(event);
    turbo_runtime_data_bind_value_destroy(state);
  }

  it("should export planner and executor event history versions as bind-native arrays") {
    turbo_runtime_data_bind_value_t *state = turbo_openai_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *planner_versions = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *executor_versions = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *planner_events = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *executor_events = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *planner_bind = NULL;
    turbo_runtime_data_bind_value_t *executor_bind = NULL;
    turbo_event_log_t *planner_log = turbo_event_log_create();
    turbo_event_log_t *executor_log = turbo_event_log_create();

    check_not_null(state);
    check_not_null(planner_versions);
    check_not_null(executor_versions);
    check_not_null(planner_events);
    check_not_null(executor_events);
    check_not_null(planner_log);
    check_not_null(executor_log);

    check_int_eq(
        turbo_runtime_data_bind_array_append(
            planner_events, turbo_event_trace_create_bind("planner.trace", "start", "p", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_runtime_data_bind_array_append(
            executor_events, turbo_event_trace_create_bind("executor.trace", "finish", "e", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(planner_versions, planner_events),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(executor_versions, executor_events),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(state, "planner_event_versions", planner_versions),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(state, "executor_event_versions", executor_versions),
                 TURBO_RUNTIME_DATA_BIND_OK);

    planner_bind = turbo_openai_agent_state_planner_event_version_bind(state, 0);
    executor_bind = turbo_openai_agent_state_executor_event_version_bind(state, 0);
    check_not_null(planner_bind);
    check_not_null(executor_bind);
    check_int_eq(turbo_event_log_load_events_bind(planner_log, planner_bind), TURBO_EVENT_LOG_OK);
    check_int_eq(turbo_event_log_load_events_bind(executor_log, executor_bind), TURBO_EVENT_LOG_OK);
    check_size_eq(turbo_event_log_size(planner_log), 1);
    check_size_eq(turbo_event_log_size(executor_log), 1);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(turbo_event_log_get(planner_log, 0), "name")),
                 "planner.trace");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(turbo_event_log_get(executor_log, 0), "name")),
                 "executor.trace");

    turbo_event_log_destroy(executor_log);
    turbo_event_log_destroy(planner_log);
    turbo_runtime_data_bind_value_destroy(executor_bind);
    turbo_runtime_data_bind_value_destroy(planner_bind);
    turbo_runtime_data_bind_value_destroy(state);
  }

  it("should export the latest planner and executor event history versions directly") {
    turbo_runtime_data_bind_value_t *state = turbo_openai_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *planner_versions = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *executor_versions = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *planner_first = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *planner_last = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *executor_first = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *executor_last = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *planner_bind = NULL;
    turbo_runtime_data_bind_value_t *executor_bind = NULL;

    check_not_null(state);
    check_not_null(planner_versions);
    check_not_null(executor_versions);
    check_not_null(planner_first);
    check_not_null(planner_last);
    check_not_null(executor_first);
    check_not_null(executor_last);

    check_int_eq(
        turbo_runtime_data_bind_array_append(
            planner_first, turbo_event_trace_create_bind("planner.trace", "start", "p1", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_runtime_data_bind_array_append(
            planner_last, turbo_event_trace_create_bind("planner.trace", "finish", "p2", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_runtime_data_bind_array_append(
            executor_first, turbo_event_trace_create_bind("executor.trace", "start", "e1", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_runtime_data_bind_array_append(
            executor_last, turbo_event_trace_create_bind("executor.trace", "finish", "e2", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);

    check_int_eq(turbo_runtime_data_bind_array_append(planner_versions, planner_first),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(planner_versions, planner_last),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(executor_versions, executor_first),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(executor_versions, executor_last),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(state, "planner_event_versions", planner_versions),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(state, "executor_event_versions", executor_versions),
                 TURBO_RUNTIME_DATA_BIND_OK);

    planner_bind = turbo_openai_agent_state_latest_planner_event_version_bind(state);
    executor_bind = turbo_openai_agent_state_latest_executor_event_version_bind(state);
    check_not_null(planner_bind);
    check_not_null(executor_bind);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_array_get(planner_bind, 0), "detail")),
                 "finish");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_array_get(executor_bind, 0), "detail")),
                 "finish");

    turbo_runtime_data_bind_value_destroy(executor_bind);
    turbo_runtime_data_bind_value_destroy(planner_bind);
    turbo_runtime_data_bind_value_destroy(state);
  }

  it("should export latest planner and executor event versions from workflow snapshots") {
    turbo_runtime_data_bind_value_t *state = turbo_openai_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *planner_versions = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *executor_versions = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *planner_events = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *executor_events = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *workflow = NULL;
    turbo_runtime_data_bind_value_t *planner_bind = NULL;
    turbo_runtime_data_bind_value_t *executor_bind = NULL;

    check_not_null(state);
    check_not_null(planner_versions);
    check_not_null(executor_versions);
    check_not_null(planner_events);
    check_not_null(executor_events);

    check_int_eq(
        turbo_runtime_data_bind_array_append(
            planner_events, turbo_event_trace_create_bind("planner.trace", "finish", "wf-p", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_runtime_data_bind_array_append(
            executor_events, turbo_event_trace_create_bind("executor.trace", "finish", "wf-e", 0)),
        TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(planner_versions, planner_events),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(executor_versions, executor_events),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(state, "planner_event_versions", planner_versions),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(state, "executor_event_versions", executor_versions),
                 TURBO_RUNTIME_DATA_BIND_OK);

    workflow = turbo_openai_agent_state_workflow_snapshot_bind(state);
    check_not_null(workflow);

    planner_bind = turbo_openai_agent_state_latest_planner_event_version_bind(workflow);
    executor_bind = turbo_openai_agent_state_latest_executor_event_version_bind(workflow);
    check_not_null(planner_bind);
    check_not_null(executor_bind);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_array_get(planner_bind, 0), "name")),
                 "planner.trace");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_array_get(executor_bind, 0), "name")),
                 "executor.trace");

    turbo_runtime_data_bind_value_destroy(executor_bind);
    turbo_runtime_data_bind_value_destroy(planner_bind);
    turbo_runtime_data_bind_value_destroy(workflow);
    turbo_runtime_data_bind_value_destroy(state);
  }
}
