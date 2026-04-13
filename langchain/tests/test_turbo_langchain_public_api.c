#include "tinytest.h"
#include "turbo_langchain.h"

spec("turbo langchain public api") {

  it("should expose the aggregate public header as a usable core surface") {
    turbo_graph_t *graph = turbo_graph_create("public-api");
    turbo_chain_t *chain = turbo_chain_create("public-api");
    turbo_runtime_binary_mir_plan_t *binary_mir_plan = NULL;
    turbo_runtime_binary_schema_t *binary_schema =
        turbo_runtime_binary_schema_create("public-api");
    void *binary_mir_build = (void *)turbo_runtime_binary_mir_compiler_build;
    turbo_runtime_data_bind_value_t *runtime_value =
        turbo_runtime_data_bind_value_create_object();
    turbo_runtime_data_bind_value_t *chain_state = turbo_chain_state_create_bind();
    void *chain_run_bind_stream = (void *)turbo_chain_run_bind_stream;
    void *graph_run_bind_stream = (void *)turbo_graph_run_bind_stream;
    void *graph_run_checkpoint_bind = (void *)turbo_graph_run_checkpoint_bind;
    void *graph_run_checkpoint_bind_stream = (void *)turbo_graph_run_checkpoint_bind_stream;
    void *graph_run_log_create = (void *)turbo_graph_run_log_create;
    void *graph_run_bind_log = (void *)turbo_graph_run_bind_log;
    void *prompt_bind = (void *)turbo_prompt_render_template_bind;
    void *prompt_message_schema = (void *)turbo_prompt_message_schema_bind;
    void *prompt_openai_codec = (void *)turbo_prompt_messages_to_openai_chat_json;
    void *prompt_responses_codec = (void *)turbo_prompt_messages_to_openai_responses_json;
    void *prompt_anthropic_codec = (void *)turbo_prompt_messages_to_anthropic_json;
    void *provider_wire_codec = (void *)turbo_model_provider_messages_to_wire_json;
    void *provider_event_codec = (void *)turbo_model_provider_response_to_event_json;
    void *provider_sse_event_codec = (void *)turbo_model_provider_sse_to_event_json;
    void *provider_event_bind_codec = (void *)turbo_model_provider_response_to_event_bind;
    void *provider_event_emit_bind = (void *)turbo_model_provider_response_emit_bind;
    void *provider_sse_event_bind_codec = (void *)turbo_model_provider_sse_to_event_bind;
    void *provider_sse_emit_bind = (void *)turbo_model_provider_sse_emit_bind;
    void *event_kind_bind = (void *)turbo_event_kind_bind;
    void *event_validate_any_bind = (void *)turbo_event_validate_bind;
    void *event_schema_bind = (void *)turbo_event_model_schema_bind;
    void *event_log_create = (void *)turbo_event_log_create;
    void *event_log_capture = (void *)turbo_event_log_capture_bind;
    void *event_log_events = (void *)turbo_event_log_events_bind;
    void *event_log_reset = (void *)turbo_event_log_reset;
    void *event_log_load_events = (void *)turbo_event_log_load_events_bind;
    void *trace_event_schema_bind = (void *)turbo_event_trace_schema_bind;
    void *tool_result_event_schema_bind = (void *)turbo_event_tool_result_schema_bind;
    void *trace_event_validate_bind = (void *)turbo_event_trace_validate_bind;
    void *trace_event_create_bind = (void *)turbo_event_trace_create_bind;
    void *event_validate_bind = (void *)turbo_event_model_validate_bind;
    void *event_create_bind = (void *)turbo_event_model_create_bind;
    void *tool_result_event_validate_bind = (void *)turbo_event_tool_result_validate_bind;
    void *tool_result_event_create_bind = (void *)turbo_event_tool_result_create_bind;
    void *runnable_create = (void *)turbo_runnable_create;
    void *runnable_invoke_bind = (void *)turbo_runnable_invoke_bind;
    void *runnable_invoke_bind_stream = (void *)turbo_runnable_invoke_bind_stream;
    void *runnable_invoke_bind_log = (void *)turbo_runnable_invoke_bind_log;
    void *runnable_pipe = (void *)turbo_runnable_pipe;
    void *chain_run_bind_log = (void *)turbo_chain_run_bind_log;
    void *agent_trace_bind_sink = (void *)turbo_openai_agent_add_trace_bind_sink;
    void *agent_state_trace_events_bind = (void *)turbo_openai_agent_state_trace_events_bind;
    void *agent_state_add_trace_event_bind = (void *)turbo_openai_agent_state_add_trace_event_bind;
    void *agent_state_capture_trace_event_bind =
        (void *)turbo_openai_agent_state_capture_trace_event_bind;
    void *agent_state_planner_event_version_bind =
        (void *)turbo_openai_agent_state_planner_event_version_bind;
    void *agent_state_executor_event_version_bind =
        (void *)turbo_openai_agent_state_executor_event_version_bind;
    void *agent_state_latest_planner_event_version_bind =
        (void *)turbo_openai_agent_state_latest_planner_event_version_bind;
    void *agent_state_latest_executor_event_version_bind =
        (void *)turbo_openai_agent_state_latest_executor_event_version_bind;
    void *tool_execute_bind = (void *)turbo_tool_registry_execute_bind;
    void *tool_runtime_default = (void *)turbo_tool_runtime_default_create;
    void *tool_runtime_invoke_bind = (void *)turbo_tool_runtime_invoke_bind;
    turbo_tool_registry_t *tool_registry = turbo_tool_registry_create();
    turbo_tool_runtime_t *tool_runtime = turbo_tool_runtime_native_create();
    void *wasm3_runtime_create = (void *)turbo_tool_runtime_wasm3_create;
    turbo_action_tool_registry_t *action_registry = turbo_action_tool_registry_create();
    json_value_t *messages = turbo_prompt_messages_create();
    json_value_t *agent_state = turbo_openai_agent_state_create();
    turbo_graph_checkpoint_t *checkpoint = NULL;
    const turbo_model_provider_t *provider = turbo_model_provider_by_name("openai");

    if (binary_schema) {
      binary_mir_plan = turbo_runtime_binary_mir_plan_create(binary_schema);
    }
    if (runtime_value) {
      turbo_graph_checkpoint_create_bind("entry", 0, runtime_value, &checkpoint);
    }

    check_not_null(graph);
    check_not_null(chain);
    check_not_null(binary_mir_plan);
    check_not_null(binary_schema);
    check_not_null(binary_mir_build);
    check_not_null(runtime_value);
    check_not_null(chain_state);
    check_not_null(chain_run_bind_stream);
    check_not_null(graph_run_bind_stream);
    check_not_null(graph_run_checkpoint_bind);
    check_not_null(graph_run_checkpoint_bind_stream);
    check_not_null(graph_run_log_create);
    check_not_null(graph_run_bind_log);
    check_not_null(prompt_bind);
    check_not_null(prompt_message_schema);
    check_not_null(prompt_openai_codec);
    check_not_null(prompt_responses_codec);
    check_not_null(prompt_anthropic_codec);
    check_not_null(provider_wire_codec);
    check_not_null(provider_event_codec);
    check_not_null(provider_sse_event_codec);
    check_not_null(provider_event_bind_codec);
    check_not_null(provider_event_emit_bind);
    check_not_null(provider_sse_event_bind_codec);
    check_not_null(provider_sse_emit_bind);
    check_not_null(event_kind_bind);
    check_not_null(event_validate_any_bind);
    check_not_null(event_schema_bind);
    check_not_null(event_log_create);
    check_not_null(event_log_capture);
    check_not_null(event_log_events);
    check_not_null(event_log_reset);
    check_not_null(event_log_load_events);
    check_not_null(trace_event_schema_bind);
    check_not_null(tool_result_event_schema_bind);
    check_not_null(trace_event_validate_bind);
    check_not_null(trace_event_create_bind);
    check_not_null(event_validate_bind);
    check_not_null(event_create_bind);
    check_not_null(tool_result_event_validate_bind);
    check_not_null(tool_result_event_create_bind);
    check_not_null(runnable_create);
    check_not_null(runnable_invoke_bind);
    check_not_null(runnable_invoke_bind_stream);
    check_not_null(runnable_invoke_bind_log);
    check_not_null(runnable_pipe);
    check_not_null(chain_run_bind_log);
    check_not_null(agent_trace_bind_sink);
    check_not_null(agent_state_trace_events_bind);
    check_not_null(agent_state_add_trace_event_bind);
    check_not_null(agent_state_capture_trace_event_bind);
    check_not_null(agent_state_planner_event_version_bind);
    check_not_null(agent_state_executor_event_version_bind);
    check_not_null(agent_state_latest_planner_event_version_bind);
    check_not_null(agent_state_latest_executor_event_version_bind);
    check_not_null(tool_execute_bind);
    check_not_null(tool_runtime_default);
    check_not_null(tool_runtime_invoke_bind);
    check_not_null(tool_registry);
    check_not_null(tool_runtime);
    check_not_null(wasm3_runtime_create);
    check_not_null(action_registry);
    check_not_null(messages);
    check_not_null(agent_state);
    check_not_null(checkpoint);
    check_not_null(provider);

    turbo_graph_checkpoint_destroy(checkpoint);
    turbo_runtime_binary_mir_plan_destroy(binary_mir_plan);
    turbo_runtime_binary_schema_destroy(binary_schema);
    turbo_runtime_data_bind_value_destroy(chain_state);
    turbo_runtime_data_bind_value_destroy(runtime_value);
    turbo_free_json(&agent_state);
    turbo_free_json(&messages);
    turbo_action_tool_registry_destroy(action_registry);
    turbo_tool_runtime_destroy(tool_runtime);
    turbo_tool_registry_destroy(tool_registry);
    turbo_chain_destroy(chain);
    turbo_graph_destroy(graph);
  }
}
