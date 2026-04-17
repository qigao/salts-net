#ifndef TURBO_AGENT_APP_H
#define TURBO_AGENT_APP_H

#include "turbo_agent_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_app_s turbo_agent_app_t;

typedef struct turbo_agent_app_config_s {
  const turbo_agent_session_config_t *session_config;
} turbo_agent_app_config_t;

CXX_C_API turbo_agent_app_t *turbo_agent_app_create(const turbo_agent_app_config_t *config);
CXX_C_API void turbo_agent_app_destroy(turbo_agent_app_t *app);

CXX_C_API turbo_agent_session_t *turbo_agent_app_session(const turbo_agent_app_t *app);

CXX_C_API const char *turbo_agent_app_thread_id(const turbo_agent_app_t *app);

CXX_C_API const char *turbo_agent_app_last_run_id(const turbo_agent_app_t *app);

CXX_C_API const char *turbo_agent_app_last_checkpoint_id(const turbo_agent_app_t *app);

CXX_C_API turbo_agent_session_workflow_kind_t
turbo_agent_app_workflow_kind(const turbo_agent_app_t *app);

CXX_C_API const char *turbo_agent_app_memory_namespace(const turbo_agent_app_t *app);

CXX_C_API const turbo_agent_memory_store_t *
turbo_agent_app_memory_store(const turbo_agent_app_t *app);

CXX_C_API int turbo_agent_app_add_trace_bind_sink(
    turbo_agent_app_t *app, const turbo_agent_trace_bind_sink_t *sink);

CXX_C_API int turbo_agent_app_add_observer_bind_sink(
    turbo_agent_app_t *app, const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_app_set_trace_history_enabled(turbo_agent_app_t *app, int enabled);

CXX_C_API int turbo_agent_app_get_thread(turbo_agent_app_t *app, json_value_t **out_thread_json);

CXX_C_API int turbo_agent_app_get_run(turbo_agent_app_t *app, const char *run_id,
                                      json_value_t **out_run_json);

CXX_C_API int turbo_agent_app_get_latest_run(turbo_agent_app_t *app, json_value_t **out_run_json);

CXX_C_API int turbo_agent_app_get_pending_run(turbo_agent_app_t *app, json_value_t **out_run_json);

CXX_C_API int turbo_agent_app_get_checkpoint(turbo_agent_app_t *app, const char *checkpoint_id,
                                             json_value_t **out_checkpoint_json);

CXX_C_API int turbo_agent_app_get_latest_checkpoint(turbo_agent_app_t *app, const char *run_id,
                                                    json_value_t **out_checkpoint_json);

CXX_C_API int turbo_agent_app_get_thread_state_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_get_thread_trace_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_get_run_state_bind(
    turbo_agent_app_t *app, const char *run_id,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_get_run_trace_events_bind(
    turbo_agent_app_t *app, const char *run_id,
    turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_get_checkpoint_state_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_update_checkpoint_state_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_update_thread_state_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_get_supervisor_inbox(
    turbo_agent_app_t *app, json_value_t **out_inbox_json);

CXX_C_API int turbo_agent_app_get_supervisor_handoff_history(
    turbo_agent_app_t *app, json_value_t **out_history_json);

CXX_C_API int turbo_agent_app_get_supervisor_inspect(
    turbo_agent_app_t *app, json_value_t **out_inspect_json);

CXX_C_API int turbo_agent_app_get_orchestration_inspect(
    turbo_agent_app_t *app, json_value_t **out_inspect_json);

/**
 * @brief Load one thread-scoped observability index for the app's current thread.
 *
 * The returned JSON bundle is a read-only aggregate over the current thread's
 * runtime inspect surfaces and derived fields such as `current_status`,
 * `current_interrupt_reason`, `current_pending_action`,
 * `current_checkpoint_summary`, `latest_run_status`, `latest_run_updated_at`,
 * `pending_run_id`, `pending_checkpoint_id`, `has_failure`, `has_model_error`,
 * `has_guardrail_rejection`, `replan_requested`, `current_failure_reason`,
 * `current_review_note`, `has_pending_review`, `has_handoff`,
 * `active_agent`, and `counts`.
 */
CXX_C_API int turbo_agent_app_get_observability_index(
    turbo_agent_app_t *app, json_value_t **out_index_json);

CXX_C_API int turbo_agent_app_append_supervisor_inbox_message_bind(
    turbo_agent_app_t *app, const char *source_agent, const char *text,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_get_checkpoint_trace_events_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_get_child_run(turbo_agent_app_t *app,
                                            const json_value_t *output_item,
                                            json_value_t **out_run_json);

CXX_C_API int turbo_agent_app_get_child_checkpoint(turbo_agent_app_t *app,
                                                   const json_value_t *output_item,
                                                   json_value_t **out_checkpoint_json);

CXX_C_API int turbo_agent_app_get_child_checkpoint_context(turbo_agent_app_t *app,
                                                           const json_value_t *output_item,
                                                           json_value_t **out_context_json);

CXX_C_API int turbo_agent_app_get_child_thread_timeline_bind(
    turbo_agent_app_t *app, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_timeline);

CXX_C_API int turbo_agent_app_get_child_branch_tree(
    turbo_agent_app_t *app, const json_value_t *output_item,
    json_value_t **out_branch_tree_json);

CXX_C_API int turbo_agent_app_get_child_inspect(turbo_agent_app_t *app,
                                                const json_value_t *output_item,
                                                json_value_t **out_inspect_json);

CXX_C_API int turbo_agent_app_get_child_orchestration_inspect(
    turbo_agent_app_t *app, const json_value_t *output_item,
    json_value_t **out_inspect_json);

CXX_C_API int turbo_agent_app_get_child_multi_agent_inspect(
    turbo_agent_app_t *app, const json_value_t *output_item,
    json_value_t **out_inspect_json);

CXX_C_API int turbo_agent_app_list_runs(turbo_agent_app_t *app, json_value_t **out_runs_json);

CXX_C_API int turbo_agent_app_list_child_runs(turbo_agent_app_t *app,
                                              const char *parent_agent_run_id,
                                              json_value_t **out_runs_json);

CXX_C_API int turbo_agent_app_list_child_checkpoints(turbo_agent_app_t *app,
                                                     const json_value_t *output_item,
                                                     json_value_t **out_checkpoints_json);

CXX_C_API int turbo_agent_app_list_checkpoints(turbo_agent_app_t *app, const char *run_id,
                                               json_value_t **out_checkpoints_json);

CXX_C_API int turbo_agent_app_list_thread_lineage(turbo_agent_app_t *app,
                                                  json_value_t **out_lineage_json);

CXX_C_API int turbo_agent_app_get_branch_tree(turbo_agent_app_t *app,
                                              json_value_t **out_branch_tree_json);

CXX_C_API int turbo_agent_app_get_checkpoint_context(turbo_agent_app_t *app,
                                                     const char *checkpoint_id,
                                                     json_value_t **out_context_json);

CXX_C_API int turbo_agent_app_load_history_events_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_load_thread_history_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_replay_history_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data);

CXX_C_API int turbo_agent_app_replay_thread_history_bind(
    turbo_agent_app_t *app, turbo_event_sink_bind_fn event_sink, void *event_sink_user_data);

CXX_C_API int turbo_agent_app_observe_history_bind(
    turbo_agent_app_t *app, const char *run_id, const char *checkpoint_id,
    const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_app_observe_thread_history_bind(
    turbo_agent_app_t *app, const turbo_agent_observer_bind_sink_t *sink);

CXX_C_API int turbo_agent_app_get_thread_timeline_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_timeline);

CXX_C_API int turbo_agent_app_apply_command_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_apply_checkpoint_command_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_apply_thread_command_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);

CXX_C_API int turbo_agent_app_resume_command_bind(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_resume_checkpoint_command_bind(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_resume_thread_command_bind(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_fork_command_bind(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_fork_checkpoint_command_bind(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_fork_thread_command_bind(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Explicit checkpoint-scoped alias for session-backed graph resume.
 *
 * This host-facing time-travel control entrypoint requires one explicit
 * checkpoint id at the callsite, instead of falling back to the app session's
 * cached checkpoint id.
 */
CXX_C_API int turbo_agent_app_resume_checkpoint_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_resume_checkpoint_state_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Explicit checkpoint-scoped alias for session-backed graph fork.
 *
 * This host-facing time-travel control entrypoint requires one explicit
 * checkpoint id at the callsite, instead of falling back to the app session's
 * cached checkpoint id.
 */
CXX_C_API int turbo_agent_app_fork_checkpoint_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_fork_checkpoint_state_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resolve the app's current thread checkpoint, then resume it.
 *
 * This is a thread-scoped replay convenience API. It mirrors the runtime and
 * session helpers, but uses the app's owned session thread so callers can pass
 * a modified thread state back without resolving a checkpoint id first.
 */
CXX_C_API int turbo_agent_app_resume_thread_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_resume_thread_state_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

/**
 * @brief Resolve the app's current thread checkpoint, then fork it.
 *
 * This is a thread-scoped replay convenience API. It mirrors the runtime and
 * session helpers, but uses the app's owned session thread so callers can pass
 * a modified thread state back without resolving a checkpoint id first.
 */
CXX_C_API int turbo_agent_app_fork_thread_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_fork_thread_state_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_resume_preset_bind_graph(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_fork_preset_bind_graph(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_resume_preset_command_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_resume_thread_preset_command_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_fork_preset_command_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_fork_thread_preset_command_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_load_child_history_events_bind(
    turbo_agent_app_t *app, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_get_child_trace_events_bind(
    turbo_agent_app_t *app, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events);

CXX_C_API int turbo_agent_app_start_text(turbo_agent_app_t *app, const char *user_text,
                                         const turbo_graph_run_options_t *options,
                                         json_value_t **out_summary_json,
                                         turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_start_messages(turbo_agent_app_t *app,
                                             const turbo_runtime_data_bind_value_t *messages,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_summary_json,
                                             turbo_runtime_data_bind_value_t **out_state);

CXX_C_API int turbo_agent_app_invoke_text(turbo_agent_app_t *app, const char *user_text,
                                          const turbo_graph_run_options_t *options,
                                          char **out_text, json_value_t **out_summary_json);

CXX_C_API int turbo_agent_app_invoke_messages_text(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, char **out_text, json_value_t **out_summary_json);

CXX_C_API int turbo_agent_app_invoke_json(turbo_agent_app_t *app, const char *user_text,
                                          const turbo_graph_run_options_t *options,
                                          json_value_t **out_json,
                                          json_value_t **out_summary_json);

CXX_C_API int turbo_agent_app_invoke_messages_json(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *messages,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

CXX_C_API int turbo_agent_app_memory_get(const turbo_agent_app_t *app, const char *memory_namespace,
                                         const char *key, char **out_value_json);

CXX_C_API int turbo_agent_app_memory_put(const turbo_agent_app_t *app, const char *memory_namespace,
                                         const char *key, const char *value_json);

CXX_C_API int turbo_agent_app_memory_put_context(const turbo_agent_app_t *app,
                                                 const char *memory_namespace, const char *key,
                                                 const char *scope, const char *path,
                                                 const char *text);

CXX_C_API int turbo_agent_app_memory_delete(const turbo_agent_app_t *app,
                                            const char *memory_namespace, const char *key);

CXX_C_API int turbo_agent_app_memory_list(const turbo_agent_app_t *app,
                                          const char *namespace_prefix,
                                          json_value_t **out_records_json);

CXX_C_API int turbo_agent_app_memory_list_records(const turbo_agent_app_t *app,
                                                  const char *namespace_prefix,
                                                  json_value_t **out_records_json);

CXX_C_API int turbo_agent_app_memory_query_records(const turbo_agent_app_t *app,
                                                   const char *namespace_prefix,
                                                   const char *kind,
                                                   const char *key_prefix,
                                                   const char *text_substring,
                                                   json_value_t **out_records_json);

#ifdef __cplusplus
}
#endif

#endif
