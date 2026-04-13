#include "tinytest.h"
#include "turbo_event_log.h"
#include "turbo_runnable.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  int count;
} runnable_counter_t;

typedef struct {
  int count;
  const char *last_detail;
  const char *last_name;
  int saw_graph_node;
} runnable_event_capture_t;

static char *test_strdup_local(const char *value) {
  char *copy;
  size_t length;

  if (!value) {
    return NULL;
  }

  length = strlen(value);
  copy = (char *)malloc(length + 1);
  check_not_null(copy);
  memcpy(copy, value, length + 1);
  return copy;
}

static int increment_bind(const turbo_runtime_data_bind_value_t *input,
                          turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  runnable_counter_t *counter = (runnable_counter_t *)user_data;
  turbo_runtime_data_bind_value_t *output;

  check_not_null(input);
  check_not_null(out_output);
  check_not_null(counter);

  counter->count++;
  output = turbo_runtime_data_bind_value_clone(input);
  check_not_null(output);
  check_int_eq(
      turbo_runtime_data_bind_object_set(output, "count",
                                         turbo_runtime_data_bind_value_create_int64(counter->count)),
      TURBO_RUNTIME_DATA_BIND_OK);
  *out_output = output;
  return 0;
}

static int graph_mark_bind(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  return turbo_runtime_data_bind_object_set(
             ctx->bind_state, "graph_done", turbo_runtime_data_bind_value_create_bool(1)) ==
                 TURBO_RUNTIME_DATA_BIND_OK
             ? 0
             : -1;
}

static void capture_runnable_event(const turbo_runtime_data_bind_value_t *event, void *user_data) {
  runnable_event_capture_t *capture = (runnable_event_capture_t *)user_data;

  check_not_null(event);
  check_not_null(capture);
  capture->count++;
  free((void *)capture->last_detail);
  free((void *)capture->last_name);
  capture->last_name = test_strdup_local(
      turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(event, "name")));
  capture->last_detail = test_strdup_local(
      turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(event, "detail")));
  if (capture->last_name && strcmp(capture->last_name, "graph.node") == 0) {
    capture->saw_graph_node = 1;
  }
}

spec("turbo runnable runtime") {
  describe("bind-native composition") {
    it("should invoke and pipe bind-native runnables") {
      runnable_counter_t first_counter = {0};
      runnable_counter_t second_counter = {0};
      turbo_runnable_config_t first_config = {
          .invoke_bind = increment_bind,
          .invoke_bind_stream = NULL,
          .user_data = &first_counter,
          .user_data_free = NULL};
      turbo_runnable_config_t second_config = {
          .invoke_bind = increment_bind,
          .invoke_bind_stream = NULL,
          .user_data = &second_counter,
          .user_data_free = NULL};
      turbo_runnable_t *first = turbo_runnable_create(&first_config);
      turbo_runnable_t *second = turbo_runnable_create(&second_config);
      turbo_runnable_t *pipe = turbo_runnable_pipe(first, second);
      turbo_runtime_data_bind_value_t *input = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *output = NULL;

      check_not_null(first);
      check_not_null(second);
      check_not_null(pipe);
      check_not_null(input);

      check_int_eq(turbo_runtime_data_bind_object_set(
                       input, "value", turbo_runtime_data_bind_value_create_string("ok")),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_runnable_invoke_bind(pipe, input, &output), 0);
      check_int_eq((int)turbo_runtime_data_bind_value_as_int64(
                       turbo_runtime_data_bind_object_get(output, "count"), 0),
                   1);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(output, "value")),
                   "ok");

      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(input);
      turbo_runnable_destroy(pipe);
      turbo_runnable_destroy(second);
      turbo_runnable_destroy(first);
    }

    it("should wrap chains and graphs as runnables") {
      turbo_chain_t *chain = turbo_chain_create("runnable-chain");
      turbo_graph_t *graph = turbo_graph_create("runnable-graph");
      turbo_runnable_t *chain_runnable;
      turbo_runnable_t *graph_runnable;
      turbo_runnable_t *pipeline;
      turbo_runtime_data_bind_value_t *input = turbo_chain_state_create_bind();
      turbo_runtime_data_bind_value_t *result = NULL;
      turbo_graph_run_result_t graph_result = {0};

      check_not_null(chain);
      check_not_null(graph);
      check_not_null(input);

      check_int_eq(turbo_runtime_data_bind_object_set(
                       (turbo_runtime_data_bind_value_t *)turbo_runtime_data_bind_object_get(input,
                                                                                             "input"),
                       "task", turbo_runtime_data_bind_value_create_string("ship")),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "prompt", "user", "Do {{task}} now."),
                   TURBO_CHAIN_OK);

      check_int_eq(turbo_graph_add_bind_node(graph, "done", graph_mark_bind, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "done"), TURBO_GRAPH_EXEC_OK);

      chain_runnable = turbo_runnable_from_chain(chain);
      graph_runnable = turbo_runnable_from_graph(graph, NULL, &graph_result);
      pipeline = turbo_runnable_pipe(chain_runnable, graph_runnable);
      check_not_null(chain_runnable);
      check_not_null(graph_runnable);
      check_not_null(pipeline);

      check_int_eq(turbo_runnable_invoke_bind(pipeline, input, &result), 0);
      check_true(turbo_runtime_data_bind_value_as_bool(
          turbo_runtime_data_bind_object_get(result, "graph_done"), 0));
      check_size_eq(turbo_runtime_data_bind_value_size(
                        turbo_runtime_data_bind_object_get(result, "messages")),
                    1);
      check_str_eq(graph_result.last_node, "done");

      turbo_runtime_data_bind_value_destroy(result);
      turbo_runtime_data_bind_value_destroy(input);
      turbo_runnable_destroy(pipeline);
      turbo_runnable_destroy(graph_runnable);
      turbo_runnable_destroy(chain_runnable);
      turbo_graph_destroy(graph);
      turbo_chain_destroy(chain);
    }

    it("should emit trace events while invoking a runnable stream") {
      runnable_counter_t counter = {0};
      runnable_event_capture_t capture = {0};
      turbo_runnable_config_t config = {
          .invoke_bind = increment_bind,
          .invoke_bind_stream = NULL,
          .user_data = &counter,
          .user_data_free = NULL};
      turbo_runnable_t *runnable = turbo_runnable_create(&config);
      turbo_runtime_data_bind_value_t *input = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *output = NULL;

      check_not_null(runnable);
      check_not_null(input);
      check_int_eq(
          turbo_runnable_invoke_bind_stream(runnable, input, capture_runnable_event, &capture, &output),
          0);
      check_int_eq(capture.count, 2);
      check_str_eq(capture.last_name, "runnable.invoke");
      check_str_eq(capture.last_detail, "finish");

      free((void *)capture.last_name);
      free((void *)capture.last_detail);
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(input);
      turbo_runnable_destroy(runnable);
    }

    it("should forward specialized runnable stream events without generic wrapper noise") {
      turbo_chain_t *chain = turbo_chain_create("runnable-stream-chain");
      turbo_runnable_t *runnable;
      turbo_runtime_data_bind_value_t *input = turbo_chain_state_create_bind();
      turbo_runtime_data_bind_value_t *output = NULL;
      runnable_event_capture_t capture = {0};

      check_not_null(chain);
      check_not_null(input);
      check_int_eq(turbo_runtime_data_bind_object_set(
                       (turbo_runtime_data_bind_value_t *)turbo_runtime_data_bind_object_get(input,
                                                                                             "input"),
                       "task", turbo_runtime_data_bind_value_create_string("stream")),
                   TURBO_RUNTIME_DATA_BIND_OK);
      check_int_eq(turbo_chain_add_prompt_step(chain, "prompt", "user", "Do {{task}} now."),
                   TURBO_CHAIN_OK);

      runnable = turbo_runnable_from_chain(chain);
      check_not_null(runnable);

      check_int_eq(
          turbo_runnable_invoke_bind_stream(runnable, input, capture_runnable_event, &capture, &output),
          0);
      check_int_eq(capture.count, 2);
      check_str_eq(capture.last_name, "runnable.chain");
      check_str_eq(capture.last_detail, "finish");

      free((void *)capture.last_name);
      free((void *)capture.last_detail);
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(input);
      turbo_runnable_destroy(runnable);
      turbo_chain_destroy(chain);
    }

    it("should forward graph stream events through runnable adapters") {
      turbo_graph_t *graph = turbo_graph_create("runnable-stream-graph");
      turbo_runnable_t *runnable;
      turbo_runtime_data_bind_value_t *input = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *output = NULL;
      runnable_event_capture_t capture = {0};

      check_not_null(graph);
      check_not_null(input);
      check_int_eq(turbo_graph_add_bind_node(graph, "done", graph_mark_bind, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "done"), TURBO_GRAPH_EXEC_OK);

      runnable = turbo_runnable_from_graph(graph, NULL, NULL);
      check_not_null(runnable);

      check_int_eq(
          turbo_runnable_invoke_bind_stream(runnable, input, capture_runnable_event, &capture, &output),
          0);
      check_true(capture.count > 2);
      check_true(capture.saw_graph_node);
      check_str_eq(capture.last_name, "runnable.graph");
      check_str_eq(capture.last_detail, "finish");

      free((void *)capture.last_name);
      free((void *)capture.last_detail);
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(input);
      turbo_runnable_destroy(runnable);
      turbo_graph_destroy(graph);
    }

    it("should capture runnable adapter streams into an event log") {
      turbo_graph_t *graph = turbo_graph_create("runnable-event-log-graph");
      turbo_runnable_t *runnable;
      turbo_runtime_data_bind_value_t *input = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *output = NULL;
      turbo_event_log_t *log = turbo_event_log_create();
      const turbo_runtime_data_bind_value_t *last_event;
      size_t i;
      int saw_graph_node = 0;

      check_not_null(graph);
      check_not_null(input);
      check_not_null(log);
      check_int_eq(turbo_graph_add_bind_node(graph, "done", graph_mark_bind, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "done"), TURBO_GRAPH_EXEC_OK);

      runnable = turbo_runnable_from_graph(graph, NULL, NULL);
      check_not_null(runnable);

      check_int_eq(
          turbo_runnable_invoke_bind_stream(runnable, input, turbo_event_log_capture_bind, log, &output),
          0);
      check_not_null(output);
      check_true(turbo_runtime_data_bind_value_as_bool(
          turbo_runtime_data_bind_object_get(output, "graph_done"), 0));
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_true(turbo_event_log_size(log) > 2);

      for (i = 0; i < turbo_event_log_size(log); ++i) {
        const turbo_runtime_data_bind_value_t *event = turbo_event_log_get(log, i);
        const char *name = turbo_runtime_data_bind_value_as_string(
            turbo_runtime_data_bind_object_get(event, "name"));

        if (name && strcmp(name, "graph.node") == 0) {
          saw_graph_node = 1;
          break;
        }
      }
      check_true(saw_graph_node);

      last_event = turbo_event_log_get(log, turbo_event_log_size(log) - 1);
      check_not_null(last_event);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(last_event, "name")),
                   "runnable.graph");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(last_event, "detail")),
                   "finish");

      turbo_event_log_destroy(log);
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(input);
      turbo_runnable_destroy(runnable);
      turbo_graph_destroy(graph);
    }

    it("should capture runnable streams through the log helper") {
      runnable_counter_t counter = {0};
      turbo_runnable_config_t config = {
          .invoke_bind = increment_bind,
          .invoke_bind_stream = NULL,
          .user_data = &counter,
          .user_data_free = NULL};
      turbo_runnable_t *runnable = turbo_runnable_create(&config);
      turbo_runtime_data_bind_value_t *input = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *output = NULL;
      turbo_event_log_t *log = turbo_event_log_create();

      check_not_null(runnable);
      check_not_null(input);
      check_not_null(log);
      check_int_eq(turbo_runnable_invoke_bind_log(runnable, input, log, &output), 0);
      check_not_null(output);
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_size_eq(turbo_event_log_size(log), 2);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(turbo_event_log_get(log, 0), "name")),
                   "runnable.invoke");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(turbo_event_log_get(log, 1), "detail")),
                   "finish");

      turbo_event_log_destroy(log);
      turbo_runtime_data_bind_value_destroy(output);
      turbo_runtime_data_bind_value_destroy(input);
      turbo_runnable_destroy(runnable);
    }
  }
}
