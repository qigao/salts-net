#ifndef TURBO_AGENT_SESSION_H
#define TURBO_AGENT_SESSION_H

#include <platform.h>

#include "turbo_agent.h"
#include "turbo_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_session_s turbo_agent_session_t;

typedef enum turbo_agent_session_workflow_kind_e {
  TURBO_AGENT_SESSION_WORKFLOW_LOOP = 0,
  TURBO_AGENT_SESSION_WORKFLOW_REVIEW = 1,
  TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING = 2
} turbo_agent_session_workflow_kind_t;

typedef struct turbo_agent_session_config_s {
  turbo_agent_runtime_store_t runtime_store;
  turbo_agent_memory_store_t memory_store;
  turbo_agent_config_t agent_config;
  const char *thread_id;
  const char *env_path;
  int load_env;
  int overwrite_env;
  turbo_agent_session_workflow_kind_t workflow_kind;
  const char *memory_namespace;
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
} turbo_agent_session_config_t;

/**
 * @brief Create one high-level session wrapper over agent + runtime.
 *
 * The session owns:
 * - one durable runtime
 * - one optional agent created from `agent_config`
 *
 * When `load_env` is non-zero, `agent_config` is first passed through
 * `turbo_agent_config_apply_env(...)`, which in turn reuses the parser layer's
 * `turbo_dotenv_load(...)` / `turbo_dotenv_load_default(...)`. This lets
 * sessions pick up `.env` credentials and base URLs for real network-backed
 * work without making the caller duplicate that bootstrap logic.
 *
 * When `runtime_store` is zeroed, a heap-backed runtime store is created.
 *
 * @param config Session configuration copied by value.
 * @return Session handle or NULL on failure.
 */
CXX_C_API turbo_agent_session_t *
turbo_agent_session_create(const turbo_agent_session_config_t *config);

/**
 * @brief Destroy one session wrapper.
 * @param session Session handle, may be NULL.
 */
CXX_C_API void turbo_agent_session_destroy(turbo_agent_session_t *session);

/**
 * @brief Return the owned agent handle, or NULL when the session is runtime-only.
 */
CXX_C_API turbo_agent_t *turbo_agent_session_agent(const turbo_agent_session_t *session);

/**
 * @brief Return the owned durable runtime handle.
 */
CXX_C_API turbo_agent_runtime_t *
turbo_agent_session_runtime(const turbo_agent_session_t *session);

/**
 * @brief Return the owned optional long-term memory store callbacks.
 */
CXX_C_API const turbo_agent_memory_store_t *
turbo_agent_session_memory_store(const turbo_agent_session_t *session);

/**
 * @brief Return the effective model string known to the session.
 */
CXX_C_API const char *turbo_agent_session_model(const turbo_agent_session_t *session);

/**
 * @brief Return the effective base URL string known to the session.
 */
CXX_C_API const char *turbo_agent_session_base_url(const turbo_agent_session_t *session);

/**
 * @brief Return the effective provider name known to the session.
 */
CXX_C_API const char *turbo_agent_session_provider_name(
    const turbo_agent_session_t *session);

/**
 * @brief Return whether the session resolved any API key.
 */
CXX_C_API int turbo_agent_session_has_api_key(const turbo_agent_session_t *session);

/**
 * @brief Return the current session thread id, or NULL before first start.
 */
CXX_C_API const char *turbo_agent_session_thread_id(const turbo_agent_session_t *session);

/**
 * @brief Return the last run id observed by the session.
 */
CXX_C_API const char *turbo_agent_session_last_run_id(
    const turbo_agent_session_t *session);

/**
 * @brief Return the most recent non-null checkpoint id observed by the session.
 */
CXX_C_API const char *turbo_agent_session_last_checkpoint_id(
    const turbo_agent_session_t *session);

/**
 * @brief Return the default canned workflow kind remembered by the session.
 */
CXX_C_API turbo_agent_session_workflow_kind_t
turbo_agent_session_workflow_kind(const turbo_agent_session_t *session);

/**
 * @brief Return the default long-term memory namespace prefix remembered by the session.
 */
CXX_C_API const char *turbo_agent_session_memory_namespace(
    const turbo_agent_session_t *session);

CXX_C_API int turbo_agent_session_add_trace_bind_sink(
    turbo_agent_session_t *session, const turbo_agent_trace_bind_sink_t *sink);

CXX_C_API int turbo_agent_session_add_observer_bind_sink(
    turbo_agent_session_t *session, const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_session_set_trace_history_enabled(
    turbo_agent_session_t *session, int enabled);

/**
 * @brief Load the current thread record through the owned runtime.
 */
CXX_C_API int turbo_agent_session_get_thread(turbo_agent_session_t *session,
                                             json_value_t **out_thread_json);

/**
 * @brief Load one run record through the owned runtime.
 *
 * When `run_id` is NULL or empty, the session uses its latest cached run id.
 */
CXX_C_API int turbo_agent_session_get_run(turbo_agent_session_t *session, const char *run_id,
                                          json_value_t **out_run_json);

/**
 * @brief Load the newest run record for the session thread through the owned runtime.
 */
CXX_C_API int turbo_agent_session_get_latest_run(turbo_agent_session_t *session,
                                                 json_value_t **out_run_json);

/**
 * @brief Load the newest interrupted run record for the session thread.
 */
CXX_C_API int turbo_agent_session_get_pending_run(turbo_agent_session_t *session,
                                                  json_value_t **out_run_json);

/**
 * @brief Load one checkpoint record through the owned runtime.
 *
 * When `checkpoint_id` is NULL or empty, the session uses its latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_get_checkpoint(turbo_agent_session_t *session,
                                                 const char *checkpoint_id,
                                                 json_value_t **out_checkpoint_json);

/**
 * @brief Load one run's latest checkpoint record through the owned runtime.
 *
 * When `run_id` is NULL or empty, the session uses its latest cached run id.
 */
CXX_C_API int turbo_agent_session_get_latest_checkpoint(turbo_agent_session_t *session,
                                                        const char *run_id,
                                                        json_value_t **out_checkpoint_json);

/**
 * @brief Load the session thread's latest persisted state through the owned runtime.
 */
CXX_C_API int turbo_agent_session_get_thread_state_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_session_get_thread_trace_events_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load one run's latest persisted state through the owned runtime.
 *
 * When `run_id` is NULL or empty, the session uses its latest cached run id.
 */
CXX_C_API int turbo_agent_session_get_run_state_bind(
    turbo_agent_session_t *session, const char *run_id,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_session_get_run_trace_events_bind(
    turbo_agent_session_t *session, const char *run_id,
    turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load one checkpoint's serialized state through the owned runtime.
 *
 * When `checkpoint_id` is NULL or empty, the session uses its latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_get_checkpoint_state_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_session_update_checkpoint_state_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_session_update_thread_state_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Load the current thread supervisor inbox as a JSON array.
 *
 * The mailbox surface is derived from the current persisted thread state.
 * When the thread has no inbox yet, an empty array is returned.
 */
CXX_C_API int turbo_agent_session_get_supervisor_inbox(
    turbo_agent_session_t *session, json_value_t **out_inbox_json);

/**
 * @brief Load the current thread supervisor handoff history as a JSON array.
 *
 * When the thread has no handoff history yet, an empty array is returned.
 */
CXX_C_API int turbo_agent_session_get_supervisor_handoff_history(
    turbo_agent_session_t *session, json_value_t **out_history_json);

/**
 * @brief Load one host-facing supervisor/mailbox inspect bundle for the current thread.
 *
 * The returned JSON object contains `supervisor`, `inbox`, `handoff_history`,
 * `control_snapshot`, and `workflow_snapshot`, all derived from the current
 * thread state.
 */
CXX_C_API int turbo_agent_session_get_supervisor_inspect(
    turbo_agent_session_t *session, json_value_t **out_inspect_json);

/**
 * @brief Load one host-facing orchestration inspect bundle for the current thread.
 *
 * The returned JSON object contains `supervisor_inspect`, `thread_timeline`,
 * `thread_lineage`, `branch_tree`, and `child_runs`.
 */
CXX_C_API int turbo_agent_session_get_orchestration_inspect(
    turbo_agent_session_t *session, json_value_t **out_inspect_json);

/**
 * @brief Load one thread-scoped observability index for the current session thread.
 *
 * The returned JSON bundle is a read-only aggregate over the current thread's
 * timeline, lineage, branch tree, durable history, trace snapshot, and small
 * derived fields such as `current_status`, `current_interrupt_reason`,
 * `current_pending_action`, `current_checkpoint_summary`, `latest_run_status`,
 * `latest_run_updated_at`, `pending_run_id`, `pending_checkpoint_id`,
 * `has_failure`, `has_model_error`, `has_guardrail_rejection`,
 * `replan_requested`, `current_failure_reason`, `current_review_note`,
 * `has_pending_review`, `has_handoff`, `active_agent`, and `counts`.
 */
CXX_C_API int turbo_agent_session_get_observability_index(
    turbo_agent_session_t *session, json_value_t **out_index_json);

/**
 * @brief Append one supervisor inbox message and return a state override bind value.
 *
 * The returned bind value can be fed into the existing replay/resume/fork state
 * override surfaces. This helper does not persist state by itself.
 */
CXX_C_API int turbo_agent_session_append_supervisor_inbox_message_bind(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_session_get_checkpoint_trace_events_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load one child run record referenced by a parent tool-result output item.
 *
 * The `output_item` must be one entry from `tool_results.outputs[]` carrying
 * `child_run_id`.
 */
CXX_C_API int turbo_agent_session_get_child_run(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_run_json);

/**
 * @brief Load one child checkpoint record referenced by a parent tool-result output item.
 *
 * The `output_item` must be one entry from `tool_results.outputs[]` carrying
 * `child_checkpoint_id`.
 */
CXX_C_API int turbo_agent_session_get_child_checkpoint(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoint_json);

/**
 * @brief Load one child checkpoint inspect context referenced by a parent tool-result output item.
 *
 * The `output_item` must be one entry from `tool_results.outputs[]` carrying
 * `child_checkpoint_id`.
 */
CXX_C_API int turbo_agent_session_get_child_checkpoint_context(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_context_json);

/**
 * @brief Load one child thread timeline referenced by a parent tool-result output item.
 *
 * The `output_item` must be one entry from `tool_results.outputs[]` carrying
 * `child_thread_id`.
 */
CXX_C_API int turbo_agent_session_get_child_thread_timeline_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_timeline);

/**
 * @brief Load one child branch tree referenced by a parent tool-result output item.
 *
 * The `output_item` must be one entry from `tool_results.outputs[]` carrying
 * `child_thread_id`.
 */
CXX_C_API int turbo_agent_session_get_child_branch_tree(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_branch_tree_json);

/**
 * @brief Load one child execution inspect bundle from a parent tool-result output item.
 *
 * The returned JSON bundle is a host-facing read-only aggregate containing the
 * child `run`, `checkpoints`, optional `latest_checkpoint`, optional
 * `checkpoint_context`, optional `thread_timeline`, optional `branch_tree`,
 * and child
 * `history_events` / `trace_events`.
 */
CXX_C_API int turbo_agent_session_get_child_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Load one child orchestration inspect bundle from a parent tool-result output item.
 *
 * The returned JSON bundle contains parent lineage fields
 * `parent_agent_run_id` / `parent_tool_call_id` / `parent_tool_name`
 * together with nested `child_inspect`.
 */
CXX_C_API int turbo_agent_session_get_child_orchestration_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief Load one multi-agent inspect bundle for a child tool-result output item.
 *
 * The returned JSON bundle aggregates the current thread's
 * `supervisor_inspect` / `orchestration_inspect` together with the resolved
 * `child_orchestration_inspect`.
 */
CXX_C_API int turbo_agent_session_get_child_multi_agent_inspect(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_inspect_json);

/**
 * @brief List all runs for the current session thread through the owned runtime.
 */
CXX_C_API int turbo_agent_session_list_runs(turbo_agent_session_t *session,
                                            json_value_t **out_runs_json);

/**
 * @brief List child runs by `parent_agent_run_id` through the owned runtime.
 *
 * When `parent_agent_run_id` is NULL or empty, the session uses its configured
 * default parent agent run id when available, else falls back to the current
 * runtime execution context's parent run id when the call happens inside an
 * active parent tool execution.
 */
CXX_C_API int turbo_agent_session_list_child_runs(
    turbo_agent_session_t *session, const char *parent_agent_run_id,
    json_value_t **out_runs_json);

/**
 * @brief List checkpoints for one child run referenced by a parent tool-result output item.
 *
 * The `output_item` must be one entry from `tool_results.outputs[]` carrying
 * `child_run_id`.
 */
CXX_C_API int turbo_agent_session_list_child_checkpoints(
    turbo_agent_session_t *session, const json_value_t *output_item,
    json_value_t **out_checkpoints_json);

/**
 * @brief List checkpoints for one run through the owned runtime.
 *
 * When `run_id` is NULL or empty, the session uses its latest cached run id.
 */
CXX_C_API int turbo_agent_session_list_checkpoints(turbo_agent_session_t *session,
                                                   const char *run_id,
                                                   json_value_t **out_checkpoints_json);

/**
 * @brief List one thread-scoped lineage summary through the owned runtime.
 */
CXX_C_API int turbo_agent_session_list_thread_lineage(turbo_agent_session_t *session,
                                                      json_value_t **out_lineage_json);

CXX_C_API int turbo_agent_session_get_branch_tree(turbo_agent_session_t *session,
                                                  json_value_t **out_branch_tree_json);

/**
 * @brief Load one explicit checkpoint inspect context through the owned runtime.
 */
CXX_C_API int turbo_agent_session_get_checkpoint_context(
    turbo_agent_session_t *session, const char *checkpoint_id,
    json_value_t **out_context_json);

/**
 * @brief Load persisted history events through the owned runtime.
 *
 * When `run_id` is NULL or empty, the session uses its latest cached run id.
 * When `checkpoint_id` is NULL or empty, the session uses its latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_load_history_events_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load current thread history through the owned runtime.
 *
 * The runtime prefers the newest interrupted run on the thread, then falls
 * back to the newest run by `updated_at`.
 */
CXX_C_API int turbo_agent_session_load_thread_history_events_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_session_replay_history_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data);

CXX_C_API int turbo_agent_session_replay_thread_history_bind(
    turbo_agent_session_t *session, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data);

/**
 * @brief Observe persisted history through the runtime-backed observer bridge.
 *
 * When `run_id` is NULL or empty, the session uses its latest cached run id.
 * When `checkpoint_id` is NULL or empty, the session uses its latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_observe_history_bind(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    const turbo_agent_observer_bind_sink_t *sink);

/**
 * @brief Observe the current session thread lineage through the observer bridge.
 */
CXX_C_API int turbo_agent_session_observe_thread_history_bind(
    turbo_agent_session_t *session, const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_session_get_thread_timeline_bind(
    turbo_agent_session_t *session, turbo_runtime_data_bind_value_t **out_timeline);

/**
 * @brief Apply one host-facing runtime command to a checkpoint state.
 *
 * When `checkpoint_id` is NULL or empty, the session uses its latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_apply_command_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Explicit checkpoint-scoped alias for `apply_command_bind(...)`.
 *
 * This entrypoint requires one explicit checkpoint id and does not fall back
 * to the session's cached checkpoint.
 */
CXX_C_API int turbo_agent_session_apply_checkpoint_command_bind(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Apply one host-facing runtime command to the session's owned thread.
 */
CXX_C_API int turbo_agent_session_apply_thread_command_bind(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

/**
 * @brief Apply one host-facing command and resume one checkpointed graph run.
 */
CXX_C_API int turbo_agent_session_resume_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Explicit checkpoint-scoped alias for `resume_command_bind(...)`.
 *
 * This entrypoint requires one explicit checkpoint id and does not fall back
 * to the session's cached checkpoint.
 */
CXX_C_API int turbo_agent_session_resume_checkpoint_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Apply one host-facing command and fork one new run from a checkpoint.
 */
CXX_C_API int turbo_agent_session_fork_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Explicit checkpoint-scoped alias for `fork_command_bind(...)`.
 *
 * This entrypoint requires one explicit checkpoint id and does not fall back
 * to the session's cached checkpoint.
 */
CXX_C_API int turbo_agent_session_fork_checkpoint_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Apply one command to the session's owned thread and resume that run.
 */
CXX_C_API int turbo_agent_session_resume_thread_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Apply one command to the session's owned thread and fork a new run.
 */
CXX_C_API int turbo_agent_session_fork_thread_command_bind(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Load one child run history referenced by a parent tool-result output item.
 *
 * Prefers `child_checkpoint_id` when present, else falls back to `child_run_id`.
 */
CXX_C_API int turbo_agent_session_load_child_history_events_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Load one child run's persisted trace events from a parent tool-result item.
 *
 * Prefers `child_checkpoint_id` when present, else falls back to `child_run_id`.
 * Returns an empty bind-native array when the resolved child state has no trace
 * history yet.
 */
CXX_C_API int turbo_agent_session_get_child_trace_events_bind(
    turbo_agent_session_t *session, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events);

/**
 * @brief Start one new graph run and update cached session ids.
 */
CXX_C_API int turbo_agent_session_start_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resume one checkpointed graph run and update cached session ids.
 *
 * When `checkpoint_id` is NULL or empty, the session uses its latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_resume_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Explicit checkpoint-scoped alias for `resume_bind_graph(...)`.
 *
 * This host-facing time-travel control entrypoint requires one explicit
 * checkpoint id at the callsite, instead of falling back to the session's
 * cached checkpoint id. Summary capture behavior remains unchanged.
 */
CXX_C_API int turbo_agent_session_resume_checkpoint_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_session_resume_checkpoint_state_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Fork one new run from a checkpoint and update cached session ids.
 *
 * When `checkpoint_id` is NULL or empty, the session uses its latest cached
 * checkpoint id.
 */
CXX_C_API int turbo_agent_session_fork_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Explicit checkpoint-scoped alias for `fork_bind_graph(...)`.
 *
 * This host-facing time-travel control entrypoint requires one explicit
 * checkpoint id at the callsite, instead of falling back to the session's
 * cached checkpoint id. Summary capture behavior remains unchanged.
 */
CXX_C_API int turbo_agent_session_fork_checkpoint_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_session_fork_checkpoint_state_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resolve the session's current thread checkpoint, then resume it.
 *
 * This is a thread-scoped replay convenience API. It mirrors the runtime
 * helper, but uses the session's owned thread id so callers can pass a modified
 * thread state back without resolving a checkpoint id first.
 */
CXX_C_API int turbo_agent_session_resume_thread_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_session_resume_thread_state_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resolve the session's current thread checkpoint, then fork it.
 *
 * This is a thread-scoped replay convenience API. It mirrors the runtime
 * helper, but uses the session's owned thread id so callers can pass a modified
 * thread state back without resolving a checkpoint id first.
 */
CXX_C_API int turbo_agent_session_fork_thread_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_session_fork_thread_state_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Build one minimal `model -> tools -> model -> end` graph around the owned agent.
 *
 * Returns NULL when the session has no owned agent.
 */
CXX_C_API turbo_graph_t *
turbo_agent_session_create_loop_graph(const turbo_agent_session_t *session);

/**
 * @brief Build one planner + review + executor graph around the owned agent.
 *
 * Returns NULL when the session has no owned agent.
 */
CXX_C_API turbo_graph_t *
turbo_agent_session_create_review_graph(const turbo_agent_session_t *session);

/**
 * @brief Build the default engineering loop graph around the owned agent.
 *
 * Returns NULL when the session has no owned agent.
 */
CXX_C_API turbo_graph_t *
turbo_agent_session_create_engineering_graph(const turbo_agent_session_t *session);

/**
 * @brief Build one canned workflow graph around the owned agent.
 *
 * Returns NULL when the session has no owned agent or `kind` is unknown.
 */
CXX_C_API turbo_graph_t *turbo_agent_session_create_preset_graph(
    const turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind);

/**
 * @brief Create one bind-native agent state seeded with an optional user message.
 *
 * Returns NULL on allocation or conversion failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_session_create_input_state_bind(const char *user_text);

/**
 * @brief Create one bind-native agent state seeded with canonical prompt messages.
 *
 * `messages` must be a bind-native array of canonical prompt message objects.
 * Returns NULL on validation, allocation, or conversion failure.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_agent_session_create_input_messages_state_bind(
    const turbo_runtime_data_bind_value_t *messages);

/**
 * @brief Start one canned workflow graph and destroy the temporary graph after the run.
 */
CXX_C_API int turbo_agent_session_start_preset_bind_graph(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Create state from one optional user message, then start one canned workflow graph.
 */
CXX_C_API int turbo_agent_session_start_preset_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Create state from one user message plus session memory context, then start one canned workflow graph.
 */
CXX_C_API int turbo_agent_session_start_preset_text_with_memory(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const char *namespace_prefix, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Start one canned workflow graph from canonical prompt messages.
 */
CXX_C_API int turbo_agent_session_start_preset_messages(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Start the session's default canned workflow from canonical prompt messages.
 *
 * When the session remembers a default memory namespace, the workflow starts
 * with long-term memory context loaded from that namespace.
 */
CXX_C_API int turbo_agent_session_start_messages(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Start the session's default canned workflow from one user message.
 *
 * When the session remembers a default memory namespace, the workflow starts
 * with long-term memory context loaded from that namespace.
 */
CXX_C_API int turbo_agent_session_start_text(turbo_agent_session_t *session, const char *user_text,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_summary_json,
                                             turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Extract one user-facing final answer text from a bind-native result state.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API char *turbo_agent_session_result_text(
    const turbo_runtime_data_bind_value_t *state);

/**
 * @brief Start one canned workflow from user text and return the final answer text.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API int turbo_agent_session_invoke_preset_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json);

/**
 * @brief Start one canned workflow from user text plus session memory context and return final answer text.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API int turbo_agent_session_invoke_preset_text_with_memory(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const char *namespace_prefix, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json);

/**
 * @brief Start one canned workflow from canonical prompt messages and return the final answer text.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API int turbo_agent_session_invoke_preset_messages_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default canned workflow from canonical prompt messages.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API int turbo_agent_session_invoke_messages_text(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default canned workflow from one user message.
 *
 * Returned text is allocated and owned by caller.
 */
CXX_C_API int turbo_agent_session_invoke_text(turbo_agent_session_t *session,
                                              const char *user_text,
                                              const turbo_graph_run_options_t *options,
                                              char **out_text,
                                              json_value_t **out_summary_json);

/**
 * @brief Start one canned workflow from user text and return parsed final JSON output.
 *
 * This is intended for structured-output flows where the final model answer is
 * expected to be valid JSON.
 *
 * Returned JSON is owned by caller.
 */
CXX_C_API int turbo_agent_session_invoke_preset_json(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

/**
 * @brief Start one canned workflow from user text plus session memory context and return parsed final JSON.
 */
CXX_C_API int turbo_agent_session_invoke_preset_json_with_memory(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const char *namespace_prefix, const turbo_graph_run_options_t *options,
    json_value_t **out_json, json_value_t **out_summary_json);

/**
 * @brief Start one canned workflow from canonical prompt messages and return parsed final JSON.
 */
CXX_C_API int turbo_agent_session_invoke_preset_messages_json(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *messages, const turbo_graph_run_options_t *options,
    json_value_t **out_json, json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default canned workflow from canonical prompt messages and parse final JSON.
 */
CXX_C_API int turbo_agent_session_invoke_messages_json(
    turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

/**
 * @brief Invoke the session's default canned workflow from one user message and parse final JSON.
 */
CXX_C_API int turbo_agent_session_invoke_json(turbo_agent_session_t *session,
                                              const char *user_text,
                                              const turbo_graph_run_options_t *options,
                                              json_value_t **out_json,
                                              json_value_t **out_summary_json);

/**
 * @brief Load one long-term memory record through the session-owned memory store.
 */
CXX_C_API int turbo_agent_session_memory_get(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             char **out_value_json);

/**
 * @brief Persist one long-term memory record through the session-owned memory store.
 */
CXX_C_API int turbo_agent_session_memory_put(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             const char *value_json);

/**
 * @brief Persist one formal memory-context record through the session-owned store.
 *
 * The stored JSON payload uses the stable shape:
 * - `scope`
 * - `path`
 * - `text`
 */
CXX_C_API int turbo_agent_session_memory_put_context(const turbo_agent_session_t *session,
                                                     const char *memory_namespace,
                                                     const char *key, const char *scope,
                                                     const char *path, const char *text);

/**
 * @brief Delete one long-term memory record through the session-owned memory store.
 */
CXX_C_API int turbo_agent_session_memory_delete(const turbo_agent_session_t *session,
                                                const char *memory_namespace, const char *key);

/**
 * @brief List long-term memory records by namespace prefix through the session-owned store.
 */
CXX_C_API int turbo_agent_session_memory_list(const turbo_agent_session_t *session,
                                              const char *namespace_prefix,
                                              json_value_t **out_records_json);

CXX_C_API int turbo_agent_session_memory_list_records(const turbo_agent_session_t *session,
                                                      const char *namespace_prefix,
                                                      json_value_t **out_records_json);

CXX_C_API int turbo_agent_session_memory_query_records(const turbo_agent_session_t *session,
                                                       const char *namespace_prefix,
                                                       const char *kind,
                                                       const char *key_prefix,
                                                       const char *text_substring,
                                                       json_value_t **out_records_json);

/**
 * @brief Load formal memory-context records from the session-owned store into state.
 *
 * Every selected record must serialize a JSON object with `scope` and `text`
 * strings plus an optional `path` string. Invalid records fail loudly.
 */
CXX_C_API int turbo_agent_session_load_memory_context(const turbo_agent_session_t *session,
                                                      json_value_t *state,
                                                      const char *namespace_prefix);

/**
 * @brief Create one bind-native agent state from user text and session memory context.
 */
CXX_C_API turbo_runtime_data_bind_value_t *
turbo_agent_session_create_input_state_with_memory_bind(const turbo_agent_session_t *session,
                                                        const char *user_text,
                                                        const char *namespace_prefix);

/**
 * @brief Create one bind-native agent state from canonical messages and session memory context.
 */
CXX_C_API turbo_runtime_data_bind_value_t *turbo_agent_session_create_input_messages_state_with_memory_bind(
    const turbo_agent_session_t *session, const turbo_runtime_data_bind_value_t *messages,
    const char *namespace_prefix);

/**
 * @brief Resume one canned workflow graph from a checkpoint and destroy the temporary graph.
 */
CXX_C_API int turbo_agent_session_resume_preset_bind_graph(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *checkpoint_id, const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Fork one canned workflow graph from a checkpoint and destroy the temporary graph.
 */
CXX_C_API int turbo_agent_session_fork_preset_bind_graph(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *checkpoint_id, const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Apply one host-facing command and resume one canned workflow graph.
 */
CXX_C_API int turbo_agent_session_resume_preset_command_bind(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *checkpoint_id, const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Build one canned workflow graph and resume the session's owned thread with a command.
 */
CXX_C_API int turbo_agent_session_resume_thread_preset_command_bind(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Apply one host-facing command and fork one canned workflow graph.
 */
CXX_C_API int turbo_agent_session_fork_preset_command_bind(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *checkpoint_id, const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Build one canned workflow graph and fork the session's owned thread with a command.
 */
CXX_C_API int turbo_agent_session_fork_thread_preset_command_bind(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

#ifdef __cplusplus
}
#endif

#endif
