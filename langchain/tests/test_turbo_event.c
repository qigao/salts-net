#include "tinytest.h"
#include "turbo_event.h"
#include "turbo_model_provider.h"

spec("turbo event runtime") {
  describe("canonical model events") {
    it("should build and validate bind-native model events") {
      turbo_runtime_data_bind_value_t *tool_calls = turbo_runtime_data_bind_value_create_array();
      turbo_runtime_data_bind_value_t *call = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *event;

      check_not_null(tool_calls);
      check_not_null(call);
      check_int_eq(turbo_runtime_data_bind_object_set(
                       call, "call_id", turbo_runtime_data_bind_value_create_string("call_1")),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runtime_data_bind_object_set(
                       call, "name", turbo_runtime_data_bind_value_create_string("sum")),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runtime_data_bind_object_set(
                       call, "arguments", turbo_runtime_data_bind_value_create_string("{\"a\":1}")),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runtime_data_bind_array_append(tool_calls, call),
                   TURBO_RUNTIME_DATA_BIND_OK);

      event = turbo_event_model_create_bind("resp_1", "hello", tool_calls);
      check_not_null(event);
      check_str_eq(turbo_event_kind_bind(event), "model");
      check_int_eq(turbo_event_validate_bind(event), 0);
      check_int_eq(turbo_event_model_validate_bind(event), 0);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "kind")),
                   "model");

      turbo_runtime_data_bind_value_destroy(event);
      turbo_runtime_data_bind_value_destroy(tool_calls);
    }

    it("should normalize provider responses into bind-native events") {
      json_value_t *chat_response = turbo_json_create_object();
      json_value_t *choices = turbo_json_create_array();
      json_value_t *choice = turbo_json_create_object();
      json_value_t *message = turbo_json_create_object();
      turbo_runtime_data_bind_value_t *event;

      check_not_null(chat_response);
      check_not_null(choices);
      check_not_null(choice);
      check_not_null(message);

      turbo_json_object_set_string(chat_response, "id", "chat_1");
      turbo_json_object_set_string(message, "content", "world");
      turbo_json_object_add(choice, "message", message);
      turbo_json_array_add(choices, choice);
      turbo_json_object_add(chat_response, "choices", choices);

      event = turbo_model_provider_response_to_event_bind(
          turbo_model_provider_openai_chat_completions(), chat_response);
      check_not_null(event);
      check_str_eq(turbo_event_kind_bind(event), "model");
      check_int_eq(turbo_event_validate_bind(event), 0);
      check_int_eq(turbo_event_model_validate_bind(event), 0);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "output_text")),
                   "world");

      turbo_runtime_data_bind_value_destroy(event);
      turbo_free_json(&chat_response);
    }

    it("should build and validate bind-native trace events") {
      turbo_runtime_data_bind_value_t *event =
          turbo_event_trace_create_bind("tool_dispatch", "begin", "{\"ok\":true}", 0);
      check_not_null(event);
      check_str_eq(turbo_event_kind_bind(event), "trace");
      check_int_eq(turbo_event_validate_bind(event), 0);
      check_int_eq(turbo_event_trace_validate_bind(event), 0);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "kind")),
                   "trace");
      turbo_runtime_data_bind_value_destroy(event);
    }

    it("should build and validate bind-native tool result events") {
      turbo_runtime_data_bind_value_t *output_value =
          turbo_runtime_data_bind_value_create_string("structured");
      turbo_runtime_data_bind_value_t *event = turbo_event_tool_result_create_bind(
          "sum", "{\"a\":1,\"b\":2}", "3", output_value, 0);

      check_not_null(output_value);
      check_not_null(event);
      check_str_eq(turbo_event_kind_bind(event), "tool_result");
      check_int_eq(turbo_event_validate_bind(event), 0);
      check_int_eq(turbo_event_tool_result_validate_bind(event), 0);
      check_str_eq(
          turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(event, "name")),
          "sum");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "arguments_json")),
                   "{\"a\":1,\"b\":2}");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "output")),
                   "3");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "output_value")),
                   "structured");

      turbo_runtime_data_bind_value_destroy(event);
      turbo_runtime_data_bind_value_destroy(output_value);
    }

    it("should carry child lineage in canonical bind-native tool result events") {
      turbo_runtime_data_bind_value_t *output_value =
          turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *event;

      check_not_null(output_value);
      check_int_eq(
          turbo_runtime_data_bind_object_set(
              output_value, "child_thread_id",
              turbo_runtime_data_bind_value_create_string("thr_child")),
          TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(
          turbo_runtime_data_bind_object_set(output_value, "child_run_id",
                                             turbo_runtime_data_bind_value_create_string(
                                                 "run_child")),
          TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(
          turbo_runtime_data_bind_object_set(
              output_value, "child_checkpoint_id",
              turbo_runtime_data_bind_value_create_null()),
          TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(
          turbo_runtime_data_bind_object_set(
              output_value, "child_status",
              turbo_runtime_data_bind_value_create_string("completed")),
          TURBO_RUNTIME_DATA_BIND_OK);

      event = turbo_event_tool_result_create_bind(
          "delegate", "{\"input\":\"hello\"}",
          "{\"ok\":true,\"summary\":\"ok\",\"stdout\":\"\",\"stderr\":\"\","
          "\"child_thread_id\":\"thr_child\",\"child_run_id\":\"run_child\","
          "\"child_checkpoint_id\":null,\"child_status\":\"completed\"}",
          output_value, 0);

      check_not_null(event);
      check_int_eq(turbo_event_validate_bind(event), 0);
      check_int_eq(turbo_event_tool_result_validate_bind(event), 0);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "child_thread_id")),
                   "thr_child");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "child_run_id")),
                   "run_child");
      check_int_eq(turbo_runtime_data_bind_value_kind(
                       turbo_runtime_data_bind_object_get(event, "child_checkpoint_id")),
                   TURBO_RUNTIME_DATA_BIND_VALUE_NULL);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "child_status")),
                   "completed");

      turbo_runtime_data_bind_value_destroy(event);
      turbo_runtime_data_bind_value_destroy(output_value);
    }

    it("should carry parent lineage in canonical bind-native tool result events") {
      turbo_runtime_data_bind_value_t *output_value =
          turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *event;

      check_not_null(output_value);
      check_int_eq(
          turbo_runtime_data_bind_object_set(
              output_value, "parent_agent_run_id",
              turbo_runtime_data_bind_value_create_string("run_parent")),
          TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(
          turbo_runtime_data_bind_object_set(
              output_value, "parent_tool_call_id",
              turbo_runtime_data_bind_value_create_string("call_parent")),
          TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(
          turbo_runtime_data_bind_object_set(
              output_value, "parent_tool_name",
              turbo_runtime_data_bind_value_create_string("delegate")),
          TURBO_RUNTIME_DATA_BIND_OK);

      event = turbo_event_tool_result_create_bind(
          "delegate", "{\"input\":\"hello\"}",
          "{\"ok\":true,\"summary\":\"ok\",\"stdout\":\"\",\"stderr\":\"\","
          "\"parent_agent_run_id\":\"run_parent\","
          "\"parent_tool_call_id\":\"call_parent\","
          "\"parent_tool_name\":\"delegate\"}",
          output_value, 0);

      check_not_null(event);
      check_int_eq(turbo_event_validate_bind(event), 0);
      check_int_eq(turbo_event_tool_result_validate_bind(event), 0);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "parent_agent_run_id")),
                   "run_parent");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "parent_tool_call_id")),
                   "call_parent");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(event, "parent_tool_name")),
                   "delegate");

      turbo_runtime_data_bind_value_destroy(event);
      turbo_runtime_data_bind_value_destroy(output_value);
    }
  }
}
