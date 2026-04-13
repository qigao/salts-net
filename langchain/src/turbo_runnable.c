#include "turbo_runnable.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const turbo_runnable_t *first;
  const turbo_runnable_t *second;
} turbo_runnable_pipe_t;

typedef struct {
  turbo_chain_t *chain;
} turbo_runnable_chain_adapter_t;

typedef struct {
  turbo_graph_t *graph;
  turbo_graph_run_options_t options;
  turbo_graph_run_result_t *result_sink;
} turbo_runnable_graph_adapter_t;

struct turbo_runnable_s {
  turbo_runnable_invoke_bind_fn invoke_bind;
  turbo_runnable_invoke_bind_stream_fn invoke_bind_stream;
  void *user_data;
  turbo_runnable_user_data_free_fn user_data_free;
};

static int turbo_runnable_pipe_invoke_bind(const turbo_runtime_data_bind_value_t *input,
                                           turbo_runtime_data_bind_value_t **out_output,
                                           void *user_data) {
  turbo_runnable_pipe_t *pipe = (turbo_runnable_pipe_t *)user_data;
  turbo_runtime_data_bind_value_t *middle = NULL;
  int rc;

  if (!pipe || !pipe->first || !pipe->second || !out_output) {
    return -1;
  }

  rc = turbo_runnable_invoke_bind(pipe->first, input, &middle);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_runnable_invoke_bind(pipe->second, middle, out_output);
  turbo_runtime_data_bind_value_destroy(middle);
  return rc;
}

static int turbo_runnable_pipe_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runnable_pipe_t *pipe = (turbo_runnable_pipe_t *)user_data;
  turbo_runtime_data_bind_value_t *middle = NULL;
  int rc;

  if (!pipe || !pipe->first || !pipe->second || !out_output) {
    return -1;
  }

  rc = turbo_runnable_invoke_bind_stream(pipe->first, input, event_sink, event_sink_user_data,
                                         &middle);
  if (rc != 0) {
    return rc;
  }

  rc = turbo_runnable_invoke_bind_stream(pipe->second, middle, event_sink, event_sink_user_data,
                                         out_output);
  turbo_runtime_data_bind_value_destroy(middle);
  return rc;
}

static int turbo_runnable_chain_invoke_bind(const turbo_runtime_data_bind_value_t *input,
                                            turbo_runtime_data_bind_value_t **out_output,
                                            void *user_data) {
  turbo_runnable_chain_adapter_t *adapter = (turbo_runnable_chain_adapter_t *)user_data;

  if (!adapter || !adapter->chain || !out_output) {
    return -1;
  }

  return turbo_chain_run_bind(adapter->chain, input, out_output) == TURBO_CHAIN_OK ? 0 : -1;
}

static int turbo_runnable_chain_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runtime_data_bind_value_t *event;
  int rc;

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.chain", "start", "", 0);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  {
    turbo_runnable_chain_adapter_t *adapter = (turbo_runnable_chain_adapter_t *)user_data;
    rc = turbo_chain_run_bind_stream(adapter->chain, input, event_sink, event_sink_user_data,
                                     out_output) == TURBO_CHAIN_OK
             ? 0
             : -1;
  }

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.chain", "finish", "", rc);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  return rc;
}

static int turbo_runnable_graph_invoke_bind(const turbo_runtime_data_bind_value_t *input,
                                            turbo_runtime_data_bind_value_t **out_output,
                                            void *user_data) {
  turbo_runnable_graph_adapter_t *adapter = (turbo_runnable_graph_adapter_t *)user_data;
  turbo_graph_run_result_t result = {0};
  turbo_graph_exec_status_t status;

  if (!adapter || !adapter->graph || !out_output) {
    return -1;
  }

  status = turbo_graph_run_bind(adapter->graph, input, &adapter->options, &result, out_output);
  if (adapter->result_sink) {
    *adapter->result_sink = result;
  }

  return (status == TURBO_GRAPH_EXEC_OK || status == TURBO_GRAPH_EXEC_STOP ||
          status == TURBO_GRAPH_EXEC_INTERRUPTED)
             ? 0
             : -1;
}

static int turbo_runnable_graph_invoke_bind_stream(
    const turbo_runtime_data_bind_value_t *input, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_runtime_data_bind_value_t **out_output, void *user_data) {
  turbo_runtime_data_bind_value_t *event;
  int rc;

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.graph", "start", "", 0);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  {
    turbo_runnable_graph_adapter_t *adapter = (turbo_runnable_graph_adapter_t *)user_data;
    turbo_graph_run_result_t result = {0};
    turbo_graph_exec_status_t status =
        turbo_graph_run_bind_stream(adapter->graph, input, &adapter->options, event_sink,
                                    event_sink_user_data, &result, out_output);
    if (adapter->result_sink) {
      *adapter->result_sink = result;
    }
    rc = (status == TURBO_GRAPH_EXEC_OK || status == TURBO_GRAPH_EXEC_STOP ||
          status == TURBO_GRAPH_EXEC_INTERRUPTED)
             ? 0
             : -1;
  }

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.graph", "finish", "", rc);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  return rc;
}

turbo_runnable_t *turbo_runnable_create(const turbo_runnable_config_t *config) {
  turbo_runnable_t *runnable;

  if (!config || !config->invoke_bind) {
    return NULL;
  }

  runnable = (turbo_runnable_t *)calloc(1, sizeof(*runnable));
  if (!runnable) {
    return NULL;
  }

  runnable->invoke_bind = config->invoke_bind;
  runnable->invoke_bind_stream = config->invoke_bind_stream;
  runnable->user_data = config->user_data;
  runnable->user_data_free = config->user_data_free;
  return runnable;
}

void turbo_runnable_destroy(turbo_runnable_t *runnable) {
  if (!runnable) {
    return;
  }

  if (runnable->user_data_free) {
    runnable->user_data_free(runnable->user_data);
  }
  free(runnable);
}

int turbo_runnable_invoke_bind(const turbo_runnable_t *runnable,
                               const turbo_runtime_data_bind_value_t *input,
                               turbo_runtime_data_bind_value_t **out_output) {
  if (!runnable || !runnable->invoke_bind || !out_output) {
    return -1;
  }

  *out_output = NULL;
  return runnable->invoke_bind(input, out_output, runnable->user_data);
}

int turbo_runnable_invoke_bind_stream(
    const turbo_runnable_t *runnable, const turbo_runtime_data_bind_value_t *input,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    turbo_runtime_data_bind_value_t **out_output) {
  turbo_runtime_data_bind_value_t *event;
  int rc;

  if (!runnable || !out_output) {
    return -1;
  }

  if (runnable->invoke_bind_stream) {
    return runnable->invoke_bind_stream(input, event_sink, event_sink_user_data, out_output,
                                        runnable->user_data);
  }

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.invoke", "start", "", 0);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  rc = turbo_runnable_invoke_bind(runnable, input, out_output);

  if (event_sink) {
    event = turbo_event_trace_create_bind("runnable.invoke", "finish", "", rc);
    if (event) {
      event_sink(event, event_sink_user_data);
      turbo_runtime_data_bind_value_destroy(event);
    }
  }

  return rc;
}

int turbo_runnable_invoke_bind_log(const turbo_runnable_t *runnable,
                                   const turbo_runtime_data_bind_value_t *input,
                                   turbo_event_log_t *log,
                                   turbo_runtime_data_bind_value_t **out_output) {
  int rc;

  if (!log) {
    return -1;
  }
  if (turbo_event_log_reset(log) != TURBO_EVENT_LOG_OK) {
    return -1;
  }

  rc = turbo_runnable_invoke_bind_stream(runnable, input, turbo_event_log_capture_bind, log,
                                         out_output);
  if (turbo_event_log_status(log) != TURBO_EVENT_LOG_OK) {
    if (out_output && *out_output) {
      turbo_runtime_data_bind_value_destroy(*out_output);
      *out_output = NULL;
    }
    return -1;
  }
  return rc;
}

turbo_runnable_t *turbo_runnable_pipe(const turbo_runnable_t *first,
                                      const turbo_runnable_t *second) {
  turbo_runnable_pipe_t *pipe;
  turbo_runnable_config_t config;

  if (!first || !second) {
    return NULL;
  }

  pipe = (turbo_runnable_pipe_t *)calloc(1, sizeof(*pipe));
  if (!pipe) {
    return NULL;
  }

  pipe->first = first;
  pipe->second = second;
  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_pipe_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_pipe_invoke_bind_stream;
  config.user_data = pipe;
  config.user_data_free = free;
  return turbo_runnable_create(&config);
}

turbo_runnable_t *turbo_runnable_from_chain(turbo_chain_t *chain) {
  turbo_runnable_chain_adapter_t *adapter;
  turbo_runnable_config_t config;

  if (!chain) {
    return NULL;
  }

  adapter = (turbo_runnable_chain_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }

  adapter->chain = chain;
  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_chain_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_chain_invoke_bind_stream;
  config.user_data = adapter;
  config.user_data_free = free;
  return turbo_runnable_create(&config);
}

turbo_runnable_t *turbo_runnable_from_graph(turbo_graph_t *graph,
                                            const turbo_graph_run_options_t *options,
                                            turbo_graph_run_result_t *result_sink) {
  turbo_runnable_graph_adapter_t *adapter;
  turbo_runnable_config_t config;

  if (!graph) {
    return NULL;
  }

  adapter = (turbo_runnable_graph_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }

  adapter->graph = graph;
  if (options) {
    adapter->options = *options;
  } else {
    memset(&adapter->options, 0, sizeof(adapter->options));
  }
  adapter->result_sink = result_sink;

  memset(&config, 0, sizeof(config));
  config.invoke_bind = turbo_runnable_graph_invoke_bind;
  config.invoke_bind_stream = turbo_runnable_graph_invoke_bind_stream;
  config.user_data = adapter;
  config.user_data_free = free;
  return turbo_runnable_create(&config);
}
