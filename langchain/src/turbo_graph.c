#include "turbo_graph.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *name;
  turbo_graph_node_fn json_fn;
  turbo_graph_bind_node_fn bind_fn;
  void *user_data;
} turbo_graph_node_entry_t;

typedef struct {
  size_t from_index;
  size_t to_index;
  turbo_graph_edge_predicate_fn json_predicate;
  turbo_graph_bind_edge_predicate_fn bind_predicate;
  void *user_data;
} turbo_graph_edge_entry_t;

struct turbo_graph_s {
  char *name;
  char *entry_node;
  turbo_graph_node_entry_t *nodes;
  size_t node_count;
  size_t node_capacity;
  turbo_graph_edge_entry_t *edges;
  size_t edge_count;
  size_t edge_capacity;
};

struct turbo_graph_checkpoint_s {
  char *next_node;
  size_t steps;
  json_value_t *state;
  char *graph_name;
  uint64_t topology_hash;
  int has_topology;
};

#define TURBO_GRAPH_CHECKPOINT_SCHEMA_VERSION 2
#define TURBO_GRAPH_FNV_OFFSET_BASIS UINT64_C(14695981039346656037)
#define TURBO_GRAPH_FNV_PRIME UINT64_C(1099511628211)

static char *turbo_graph_strdup(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }

  len = strlen(src) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, src, len);
  return copy;
}

static uint64_t turbo_graph_hash_bytes(uint64_t hash, const void *data, size_t len) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t i;

  for (i = 0; i < len; ++i) {
    hash ^= (uint64_t)bytes[i];
    hash *= TURBO_GRAPH_FNV_PRIME;
  }

  return hash;
}

static uint64_t turbo_graph_hash_cstring(uint64_t hash, const char *text) {
  size_t len = text ? strlen(text) : 0;

  hash = turbo_graph_hash_bytes(hash, &len, sizeof(len));
  if (len > 0) {
    hash = turbo_graph_hash_bytes(hash, text, len);
  }
  return hash;
}

static uint64_t turbo_graph_topology_hash(const turbo_graph_t *graph) {
  uint64_t hash = TURBO_GRAPH_FNV_OFFSET_BASIS;
  size_t i;

  if (!graph) {
    return 0;
  }

  hash = turbo_graph_hash_cstring(hash, graph->name);
  hash = turbo_graph_hash_cstring(hash, graph->entry_node);
  hash = turbo_graph_hash_bytes(hash, &graph->node_count, sizeof(graph->node_count));
  hash = turbo_graph_hash_bytes(hash, &graph->edge_count, sizeof(graph->edge_count));

  for (i = 0; i < graph->node_count; ++i) {
    hash = turbo_graph_hash_cstring(hash, graph->nodes[i].name);
  }

  for (i = 0; i < graph->edge_count; ++i) {
    const turbo_graph_edge_entry_t *edge = &graph->edges[i];
    hash = turbo_graph_hash_cstring(hash, graph->nodes[edge->from_index].name);
    hash = turbo_graph_hash_cstring(hash, graph->nodes[edge->to_index].name);
    hash = turbo_graph_hash_bytes(hash, &edge->from_index, sizeof(edge->from_index));
    hash = turbo_graph_hash_bytes(hash, &edge->to_index, sizeof(edge->to_index));
    hash = turbo_graph_hash_bytes(hash, &edge->json_predicate, sizeof(edge->json_predicate));
    hash = turbo_graph_hash_bytes(hash, &edge->bind_predicate, sizeof(edge->bind_predicate));
  }

  return hash;
}

static turbo_graph_exec_status_t turbo_graph_checkpoint_set_topology(
    turbo_graph_checkpoint_t *checkpoint, const turbo_graph_t *graph) {
  char *graph_name_copy = NULL;

  if (!checkpoint || !graph) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  if (graph->name) {
    graph_name_copy = turbo_graph_strdup(graph->name);
    if (!graph_name_copy) {
      return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
    }
  }

  free(checkpoint->graph_name);
  checkpoint->graph_name = graph_name_copy;
  checkpoint->topology_hash = turbo_graph_topology_hash(graph);
  checkpoint->has_topology = 1;
  return TURBO_GRAPH_EXEC_OK;
}

static int turbo_graph_checkpoint_matches_graph(const turbo_graph_checkpoint_t *checkpoint,
                                                const turbo_graph_t *graph) {
  if (!checkpoint || !graph || !checkpoint->has_topology) {
    return 1;
  }

  if (((checkpoint->graph_name && graph->name) &&
       strcmp(checkpoint->graph_name, graph->name) != 0) ||
      ((checkpoint->graph_name == NULL) != (graph->name == NULL))) {
    return 0;
  }

  return checkpoint->topology_hash == turbo_graph_topology_hash(graph);
}

static void turbo_graph_result_init(turbo_graph_run_result_t *result,
                                    turbo_graph_exec_status_t status,
                                    const char *last_node, const char *next_node,
                                    size_t steps) {
  if (!result) {
    return;
  }

  result->status = status;
  result->last_node = last_node;
  result->next_node = next_node;
  result->steps = steps;
}

static turbo_graph_exec_status_t turbo_graph_emit_trace_event(
    const turbo_graph_exec_ctx_t *ctx, const char *name, const char *detail, const char *payload,
    int64_t status) {
  turbo_runtime_data_bind_value_t *event;

  if (!ctx || !ctx->event_sink) {
    return TURBO_GRAPH_EXEC_OK;
  }

  event = turbo_event_trace_create_bind(name, detail, payload, status);
  if (!event) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  ctx->event_sink(event, ctx->event_sink_user_data);
  turbo_runtime_data_bind_value_destroy(event);
  return TURBO_GRAPH_EXEC_OK;
}

static size_t turbo_graph_find_node_index(const turbo_graph_t *graph, const char *name) {
  size_t i;

  if (!graph || !name) {
    return (size_t)-1;
  }

  for (i = 0; i < graph->node_count; ++i) {
    if (strcmp(graph->nodes[i].name, name) == 0) {
      return i;
    }
  }

  return (size_t)-1;
}

static turbo_graph_exec_status_t turbo_graph_reserve_nodes(turbo_graph_t *graph) {
  turbo_graph_node_entry_t *nodes;
  size_t new_capacity;

  if (graph->node_count < graph->node_capacity) {
    return TURBO_GRAPH_EXEC_OK;
  }

  new_capacity = graph->node_capacity == 0 ? 4 : graph->node_capacity * 2;
  nodes = (turbo_graph_node_entry_t *)realloc(graph->nodes,
                                              new_capacity * sizeof(*nodes));
  if (!nodes) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  graph->nodes = nodes;
  graph->node_capacity = new_capacity;
  return TURBO_GRAPH_EXEC_OK;
}

static turbo_graph_exec_status_t turbo_graph_reserve_edges(turbo_graph_t *graph) {
  turbo_graph_edge_entry_t *edges;
  size_t new_capacity;

  if (graph->edge_count < graph->edge_capacity) {
    return TURBO_GRAPH_EXEC_OK;
  }

  new_capacity = graph->edge_capacity == 0 ? 8 : graph->edge_capacity * 2;
  edges = (turbo_graph_edge_entry_t *)realloc(graph->edges,
                                              new_capacity * sizeof(*edges));
  if (!edges) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  graph->edges = edges;
  graph->edge_capacity = new_capacity;
  return TURBO_GRAPH_EXEC_OK;
}

static turbo_graph_exec_status_t
turbo_graph_clone_bind_state(const turbo_runtime_data_bind_value_t *state,
                             turbo_runtime_data_bind_value_t **out_state) {
  if (!out_state) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  *out_state = NULL;
  if (!state) {
    *out_state = turbo_runtime_data_bind_value_create_null();
  } else {
    *out_state = turbo_runtime_data_bind_value_clone(state);
  }

  return *out_state ? TURBO_GRAPH_EXEC_OK : TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
}

static turbo_graph_exec_status_t turbo_graph_clone_json(const json_value_t *state,
                                                        json_value_t **out_state) {
  if (!out_state) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  *out_state = NULL;
  if (!state) {
    *out_state = turbo_json_create_null();
    return *out_state ? TURBO_GRAPH_EXEC_OK : TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  *out_state = turbo_json_clone(state);
  return *out_state ? TURBO_GRAPH_EXEC_OK : TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
}

static turbo_graph_exec_status_t turbo_graph_bind_state_to_json(
    const turbo_runtime_data_bind_value_t *state, json_value_t **out_state) {
  if (!out_state) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  *out_state = NULL;
  if (!state) {
    *out_state = turbo_json_create_null();
    return *out_state ? TURBO_GRAPH_EXEC_OK : TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  *out_state = turbo_runtime_data_bind_value_to_json(state);
  if (!*out_state) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  return TURBO_GRAPH_EXEC_OK;
}

static turbo_graph_exec_status_t
turbo_graph_emit_checkpoint_json(const turbo_graph_t *graph,
                                 const turbo_graph_run_options_t *options,
                                 const char *next_node, size_t steps,
                                 const json_value_t *state) {
  turbo_graph_checkpoint_t *checkpoint = NULL;
  turbo_graph_exec_status_t status;

  if (!options || !options->checkpoint_cb || !next_node) {
    return TURBO_GRAPH_EXEC_OK;
  }

  status = turbo_graph_checkpoint_create(next_node, steps, state, &checkpoint);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_checkpoint_set_topology(checkpoint, graph);
  if (status != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_checkpoint_destroy(checkpoint);
    return status;
  }

  options->checkpoint_cb(checkpoint, options->checkpoint_user_data);
  turbo_graph_checkpoint_destroy(checkpoint);
  return TURBO_GRAPH_EXEC_OK;
}

static turbo_graph_exec_status_t
turbo_graph_emit_checkpoint_bind(const turbo_graph_t *graph,
                                 const turbo_graph_run_options_t *options,
                                 const char *next_node, size_t steps,
                                 const turbo_runtime_data_bind_value_t *state) {
  turbo_graph_checkpoint_t *checkpoint = NULL;
  json_value_t *json_state = NULL;
  turbo_graph_exec_status_t status;

  if (!options || !options->checkpoint_cb || !next_node) {
    return TURBO_GRAPH_EXEC_OK;
  }

  status = turbo_graph_bind_state_to_json(state, &json_state);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_checkpoint_create(next_node, steps, json_state, &checkpoint);
  turbo_free_json(&json_state);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_checkpoint_set_topology(checkpoint, graph);
  if (status != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_checkpoint_destroy(checkpoint);
    return status;
  }

  options->checkpoint_cb(checkpoint, options->checkpoint_user_data);
  turbo_graph_checkpoint_destroy(checkpoint);
  return TURBO_GRAPH_EXEC_OK;
}

static int turbo_graph_should_interrupt_before(const turbo_graph_run_options_t *options,
                                               const char *node_name) {
  size_t i;

  if (!options || !node_name || !options->interrupt_before_nodes ||
      options->interrupt_before_count == 0) {
    return 0;
  }

  for (i = 0; i < options->interrupt_before_count; ++i) {
    const char *candidate = options->interrupt_before_nodes[i];
    if (candidate && strcmp(candidate, node_name) == 0) {
      return 1;
    }
  }

  return 0;
}

static turbo_graph_exec_status_t
turbo_graph_run_internal(turbo_graph_t *graph, json_value_t *state, const char *start_node,
                         size_t initial_steps, const turbo_graph_run_options_t *options,
                         turbo_graph_run_result_t *out_result) {
  const char *current_node;
  const char *last_node = NULL;
  size_t steps = initial_steps;
  size_t max_steps = 0;
  int skip_initial_interrupt = options && options->skip_initial_interrupt ? 1 : 0;

  if (!graph || !state) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INVALID_ARGUMENT, NULL, NULL, 0);
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  current_node = start_node;
  max_steps = options ? options->max_steps : 0;
  if (!current_node) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INVALID_ARGUMENT, NULL, NULL, 0);
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  while (current_node) {
    turbo_graph_exec_ctx_t ctx;
    size_t current_index;
    turbo_graph_exec_status_t checkpoint_status;
    const char *resolved_next = NULL;
    size_t i;
    size_t outgoing_count = 0;
    int node_status;

    if (max_steps > 0 && steps >= max_steps) {
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_STEP_LIMIT, last_node,
                              current_node, steps);
      return TURBO_GRAPH_EXEC_STEP_LIMIT;
    }

    if (!skip_initial_interrupt && turbo_graph_should_interrupt_before(options, current_node)) {
      checkpoint_status =
          turbo_graph_emit_checkpoint_json(graph, options, current_node, steps, state);
      if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
        turbo_graph_result_init(out_result, checkpoint_status, last_node, current_node, steps);
        return checkpoint_status;
      }

      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INTERRUPTED, last_node,
                              current_node, steps);
      return TURBO_GRAPH_EXEC_INTERRUPTED;
    }
    skip_initial_interrupt = 0;

    current_index = turbo_graph_find_node_index(graph, current_node);
    if (current_index == (size_t)-1) {
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_NODE_NOT_FOUND, last_node,
                              current_node, steps);
      return TURBO_GRAPH_EXEC_NODE_NOT_FOUND;
    }

    ctx.graph = graph;
    ctx.state = state;
    ctx.current_node = graph->nodes[current_index].name;
    ctx.next_node = NULL;
    ctx.step = steps;
    ctx.stop = 0;

    if (!graph->nodes[current_index].json_fn) {
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INVALID_ARGUMENT, last_node, current_node,
                              steps);
      return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
    }

    node_status =
        graph->nodes[current_index].json_fn(&ctx, graph->nodes[current_index].user_data);
    steps++;
    last_node = graph->nodes[current_index].name;

    if (node_status < 0) {
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_ERROR, last_node, NULL, steps);
      return TURBO_GRAPH_EXEC_ERROR;
    }

    if (ctx.stop || node_status > 0) {
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_STOP, last_node, NULL, steps);
      return TURBO_GRAPH_EXEC_STOP;
    }

    if (ctx.next_node) {
      if (turbo_graph_find_node_index(graph, ctx.next_node) == (size_t)-1) {
        turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_NODE_NOT_FOUND, last_node,
                                ctx.next_node, steps);
        return TURBO_GRAPH_EXEC_NODE_NOT_FOUND;
      }
      resolved_next = ctx.next_node;
    } else {
      for (i = 0; i < graph->edge_count; ++i) {
        int predicate_status;

        if (graph->edges[i].from_index != current_index) {
          continue;
        }

        outgoing_count++;
        if (!graph->edges[i].json_predicate) {
          resolved_next = graph->nodes[graph->edges[i].to_index].name;
          break;
        }

        predicate_status =
            graph->edges[i].json_predicate(&ctx, graph->edges[i].user_data);
        if (predicate_status < 0) {
          turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_ERROR, last_node, NULL,
                                  steps);
          return TURBO_GRAPH_EXEC_ERROR;
        }

        if (predicate_status > 0) {
          resolved_next = graph->nodes[graph->edges[i].to_index].name;
          break;
        }
      }
    }

    if (!resolved_next) {
      if (outgoing_count > 0) {
        turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_ROUTE_NOT_FOUND, last_node,
                                NULL, steps);
        return TURBO_GRAPH_EXEC_ROUTE_NOT_FOUND;
      }

      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_OK, last_node, NULL, steps);
      return TURBO_GRAPH_EXEC_OK;
    }

    if (turbo_graph_should_interrupt_before(options, resolved_next)) {
      checkpoint_status =
          turbo_graph_emit_checkpoint_json(graph, options, resolved_next, steps, state);
      if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
        turbo_graph_result_init(out_result, checkpoint_status, last_node, resolved_next, steps);
        return checkpoint_status;
      }

      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INTERRUPTED, last_node,
                              resolved_next, steps);
      return TURBO_GRAPH_EXEC_INTERRUPTED;
    }

    checkpoint_status =
        turbo_graph_emit_checkpoint_json(graph, options, resolved_next, steps, state);
    if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
      turbo_graph_result_init(out_result, checkpoint_status, last_node, resolved_next, steps);
      return checkpoint_status;
    }

    current_node = resolved_next;
  }

  turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_OK, last_node, NULL, steps);
  return TURBO_GRAPH_EXEC_OK;
}

static turbo_graph_exec_status_t
turbo_graph_run_internal_bind(turbo_graph_t *graph, turbo_runtime_data_bind_value_t **state_ptr,
                              const char *start_node, size_t initial_steps,
                              const turbo_graph_run_options_t *options,
                              turbo_event_sink_bind_fn event_sink,
                              void *event_sink_user_data,
                              turbo_graph_run_result_t *out_result) {
  turbo_runtime_data_bind_value_t *state;
  const char *current_node;
  const char *last_node = NULL;
  size_t steps = initial_steps;
  size_t max_steps = 0;
  int skip_initial_interrupt = options && options->skip_initial_interrupt ? 1 : 0;

  if (!graph || !state_ptr || !*state_ptr) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INVALID_ARGUMENT, NULL, NULL, 0);
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  state = *state_ptr;

  current_node = start_node;
  max_steps = options ? options->max_steps : 0;
  if (!current_node) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INVALID_ARGUMENT, NULL, NULL, 0);
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  while (current_node) {
    turbo_graph_exec_ctx_t ctx;
    size_t current_index;
    turbo_graph_exec_status_t checkpoint_status;
    const char *resolved_next = NULL;
    size_t i;
    size_t outgoing_count = 0;
    int node_status;

    if (max_steps > 0 && steps >= max_steps) {
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_STEP_LIMIT, last_node, current_node,
                              steps);
      return TURBO_GRAPH_EXEC_STEP_LIMIT;
    }

    if (!skip_initial_interrupt && turbo_graph_should_interrupt_before(options, current_node)) {
      checkpoint_status =
          turbo_graph_emit_checkpoint_bind(graph, options, current_node, steps, state);
      if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
        turbo_graph_result_init(out_result, checkpoint_status, last_node, current_node, steps);
        return checkpoint_status;
      }

      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INTERRUPTED, last_node, current_node,
                              steps);
      return TURBO_GRAPH_EXEC_INTERRUPTED;
    }
    skip_initial_interrupt = 0;

    current_index = turbo_graph_find_node_index(graph, current_node);
    if (current_index == (size_t)-1) {
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_NODE_NOT_FOUND, last_node, current_node,
                              steps);
      return TURBO_GRAPH_EXEC_NODE_NOT_FOUND;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.graph = graph;
    ctx.bind_state = state;
    ctx.current_node = graph->nodes[current_index].name;
    ctx.step = steps;
    ctx.event_sink = event_sink;
    ctx.event_sink_user_data = event_sink_user_data;

    checkpoint_status =
        turbo_graph_emit_trace_event(&ctx, "graph.node", "start", ctx.current_node, 0);
    if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
      turbo_graph_result_init(out_result, checkpoint_status, last_node, current_node, steps);
      return checkpoint_status;
    }

    if (graph->nodes[current_index].bind_fn) {
      node_status = graph->nodes[current_index].bind_fn(&ctx, graph->nodes[current_index].user_data);
    } else if (graph->nodes[current_index].json_fn) {
      json_value_t *json_state = turbo_runtime_data_bind_value_to_json(state);
      turbo_runtime_data_bind_value_t *updated_state;

      if (!json_state) {
        turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_OUT_OF_MEMORY, last_node, current_node,
                                steps);
        return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
      }
      ctx.state = json_state;
      node_status =
          graph->nodes[current_index].json_fn(&ctx, graph->nodes[current_index].user_data);
      updated_state = turbo_runtime_data_bind_value_from_json(json_state);
      turbo_free_json(&json_state);
      if (!updated_state) {
        turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_OUT_OF_MEMORY, last_node, current_node,
                                steps);
        return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
      }
      turbo_runtime_data_bind_value_destroy(state);
      state = updated_state;
      *state_ptr = state;
      ctx.bind_state = state;
      ctx.state = NULL;
    } else {
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INVALID_ARGUMENT, last_node, current_node,
                              steps);
      return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
    }
    steps++;
    last_node = graph->nodes[current_index].name;

    if (node_status < 0) {
      turbo_graph_emit_trace_event(&ctx, "graph.node", "error", last_node, node_status);
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_ERROR, last_node, NULL, steps);
      return TURBO_GRAPH_EXEC_ERROR;
    }

    if (ctx.stop || node_status > 0) {
      checkpoint_status =
          turbo_graph_emit_trace_event(&ctx, "graph.node", "stop", last_node, node_status);
      if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
        turbo_graph_result_init(out_result, checkpoint_status, last_node, NULL, steps);
        return checkpoint_status;
      }
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_STOP, last_node, NULL, steps);
      return TURBO_GRAPH_EXEC_STOP;
    }

    checkpoint_status =
        turbo_graph_emit_trace_event(&ctx, "graph.node", "finish", last_node, 0);
    if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
      turbo_graph_result_init(out_result, checkpoint_status, last_node, NULL, steps);
      return checkpoint_status;
    }

    if (ctx.next_node) {
      if (turbo_graph_find_node_index(graph, ctx.next_node) == (size_t)-1) {
        turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_NODE_NOT_FOUND, last_node, ctx.next_node,
                                steps);
        return TURBO_GRAPH_EXEC_NODE_NOT_FOUND;
      }
      resolved_next = ctx.next_node;
    } else {
      for (i = 0; i < graph->edge_count; ++i) {
        int predicate_status;

        if (graph->edges[i].from_index != current_index) {
          continue;
        }

        outgoing_count++;
        if (!graph->edges[i].bind_predicate && !graph->edges[i].json_predicate) {
          resolved_next = graph->nodes[graph->edges[i].to_index].name;
          break;
        }

        if (graph->edges[i].bind_predicate) {
          predicate_status = graph->edges[i].bind_predicate(&ctx, graph->edges[i].user_data);
        } else {
          json_value_t *json_state = turbo_runtime_data_bind_value_to_json(state);

          if (!json_state) {
            turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_OUT_OF_MEMORY, last_node, NULL,
                                    steps);
            return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
          }

          ctx.state = json_state;
          predicate_status = graph->edges[i].json_predicate(&ctx, graph->edges[i].user_data);
          turbo_free_json(&json_state);
          ctx.state = NULL;
        }
        if (predicate_status < 0) {
          turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_ERROR, last_node, NULL, steps);
          return TURBO_GRAPH_EXEC_ERROR;
        }

        if (predicate_status > 0) {
          resolved_next = graph->nodes[graph->edges[i].to_index].name;
          break;
        }
      }
    }

    if (!resolved_next) {
      if (outgoing_count > 0) {
        turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_ROUTE_NOT_FOUND, last_node, NULL, steps);
        return TURBO_GRAPH_EXEC_ROUTE_NOT_FOUND;
      }

      checkpoint_status =
          turbo_graph_emit_trace_event(&ctx, "graph.route", "complete", last_node, 0);
      if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
        turbo_graph_result_init(out_result, checkpoint_status, last_node, NULL, steps);
        return checkpoint_status;
      }
      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_OK, last_node, NULL, steps);
      return TURBO_GRAPH_EXEC_OK;
    }

    checkpoint_status =
        turbo_graph_emit_trace_event(&ctx, "graph.route", "next", resolved_next, 0);
    if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
      turbo_graph_result_init(out_result, checkpoint_status, last_node, resolved_next, steps);
      return checkpoint_status;
    }

    if (turbo_graph_should_interrupt_before(options, resolved_next)) {
      checkpoint_status =
          turbo_graph_emit_checkpoint_bind(graph, options, resolved_next, steps, state);
      if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
        turbo_graph_result_init(out_result, checkpoint_status, last_node, resolved_next, steps);
        return checkpoint_status;
      }

      turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INTERRUPTED, last_node, resolved_next,
                              steps);
      return TURBO_GRAPH_EXEC_INTERRUPTED;
    }

    checkpoint_status =
        turbo_graph_emit_checkpoint_bind(graph, options, resolved_next, steps, state);
    if (checkpoint_status != TURBO_GRAPH_EXEC_OK) {
      turbo_graph_result_init(out_result, checkpoint_status, last_node, resolved_next, steps);
      return checkpoint_status;
    }

    current_node = resolved_next;
  }

  turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_OK, last_node, NULL, steps);
  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_t *turbo_graph_create(const char *name) {
  turbo_graph_t *graph = (turbo_graph_t *)calloc(1, sizeof(*graph));
  if (!graph) {
    return NULL;
  }

  if (name) {
    graph->name = turbo_graph_strdup(name);
    if (!graph->name) {
      free(graph);
      return NULL;
    }
  }

  return graph;
}

void turbo_graph_destroy(turbo_graph_t *graph) {
  size_t i;

  if (!graph) {
    return;
  }

  free(graph->name);
  free(graph->entry_node);

  for (i = 0; i < graph->node_count; ++i) {
    free(graph->nodes[i].name);
  }

  free(graph->nodes);
  free(graph->edges);
  free(graph);
}

turbo_graph_exec_status_t turbo_graph_add_node(turbo_graph_t *graph, const char *name,
                                               turbo_graph_node_fn fn, void *user_data) {
  turbo_graph_exec_status_t status;
  char *name_copy;

  if (!graph || !name || !fn) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  if (turbo_graph_find_node_index(graph, name) != (size_t)-1) {
    return TURBO_GRAPH_EXEC_DUPLICATE_NODE;
  }

  status = turbo_graph_reserve_nodes(graph);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  name_copy = turbo_graph_strdup(name);
  if (!name_copy) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  graph->nodes[graph->node_count].name = name_copy;
  graph->nodes[graph->node_count].json_fn = fn;
  graph->nodes[graph->node_count].bind_fn = NULL;
  graph->nodes[graph->node_count].user_data = user_data;
  graph->node_count++;
  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_graph_add_bind_node(turbo_graph_t *graph, const char *name, turbo_graph_bind_node_fn fn,
                          void *user_data) {
  char *name_copy;

  if (!graph || !name || !fn) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  if (turbo_graph_find_node_index(graph, name) != (size_t)-1) {
    return TURBO_GRAPH_EXEC_DUPLICATE_NODE;
  }

  if (turbo_graph_reserve_nodes(graph) != TURBO_GRAPH_EXEC_OK) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  name_copy = turbo_graph_strdup(name);
  if (!name_copy) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  graph->nodes[graph->node_count].name = name_copy;
  graph->nodes[graph->node_count].json_fn = NULL;
  graph->nodes[graph->node_count].bind_fn = fn;
  graph->nodes[graph->node_count].user_data = user_data;
  graph->node_count++;
  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_graph_add_edge(turbo_graph_t *graph, const char *from, const char *to,
                     turbo_graph_edge_predicate_fn predicate, void *user_data) {
  size_t from_index;
  size_t to_index;
  turbo_graph_exec_status_t status;

  if (!graph || !from || !to) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  from_index = turbo_graph_find_node_index(graph, from);
  to_index = turbo_graph_find_node_index(graph, to);
  if (from_index == (size_t)-1 || to_index == (size_t)-1) {
    return TURBO_GRAPH_EXEC_NODE_NOT_FOUND;
  }

  status = turbo_graph_reserve_edges(graph);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  graph->edges[graph->edge_count].from_index = from_index;
  graph->edges[graph->edge_count].to_index = to_index;
  graph->edges[graph->edge_count].json_predicate = predicate;
  graph->edges[graph->edge_count].bind_predicate = NULL;
  graph->edges[graph->edge_count].user_data = user_data;
  graph->edge_count++;
  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_graph_add_bind_edge(turbo_graph_t *graph, const char *from, const char *to,
                          turbo_graph_bind_edge_predicate_fn predicate, void *user_data) {
  size_t from_index;
  size_t to_index;

  if (!graph || !from || !to) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  from_index = turbo_graph_find_node_index(graph, from);
  to_index = turbo_graph_find_node_index(graph, to);
  if (from_index == (size_t)-1 || to_index == (size_t)-1) {
    return TURBO_GRAPH_EXEC_NODE_NOT_FOUND;
  }

  if (turbo_graph_reserve_edges(graph) != TURBO_GRAPH_EXEC_OK) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  graph->edges[graph->edge_count].from_index = from_index;
  graph->edges[graph->edge_count].to_index = to_index;
  graph->edges[graph->edge_count].json_predicate = NULL;
  graph->edges[graph->edge_count].bind_predicate = predicate;
  graph->edges[graph->edge_count].user_data = user_data;
  graph->edge_count++;
  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_graph_set_entry(turbo_graph_t *graph, const char *name) {
  char *entry_copy;

  if (!graph || !name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  if (turbo_graph_find_node_index(graph, name) == (size_t)-1) {
    return TURBO_GRAPH_EXEC_NODE_NOT_FOUND;
  }

  entry_copy = turbo_graph_strdup(name);
  if (!entry_copy) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  free(graph->entry_node);
  graph->entry_node = entry_copy;
  return TURBO_GRAPH_EXEC_OK;
}

const char *turbo_graph_get_entry(const turbo_graph_t *graph) {
  return graph ? graph->entry_node : NULL;
}

size_t turbo_graph_node_count(const turbo_graph_t *graph) {
  return graph ? graph->node_count : 0;
}

size_t turbo_graph_edge_count(const turbo_graph_t *graph) {
  return graph ? graph->edge_count : 0;
}

turbo_graph_exec_status_t turbo_graph_ctx_set_next(turbo_graph_exec_ctx_t *ctx,
                                                   const char *next_node) {
  if (!ctx || !ctx->graph || !next_node) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  if (turbo_graph_find_node_index(ctx->graph, next_node) == (size_t)-1) {
    return TURBO_GRAPH_EXEC_NODE_NOT_FOUND;
  }

  ctx->next_node = next_node;
  return TURBO_GRAPH_EXEC_OK;
}

void turbo_graph_ctx_stop(turbo_graph_exec_ctx_t *ctx) {
  if (ctx) {
    ctx->stop = 1;
  }
}

turbo_graph_exec_status_t
turbo_graph_run(turbo_graph_t *graph, json_value_t *state,
                const turbo_graph_run_options_t *options,
                turbo_graph_run_result_t *out_result) {
  const char *start_node =
      options && options->start_node ? options->start_node : graph ? graph->entry_node : NULL;
  return turbo_graph_run_internal(graph, state, start_node, 0, options, out_result);
}

turbo_graph_exec_status_t
turbo_graph_run_bind(turbo_graph_t *graph, const turbo_runtime_data_bind_value_t *state,
                     const turbo_graph_run_options_t *options,
                     turbo_graph_run_result_t *out_result,
                     turbo_runtime_data_bind_value_t **out_state) {
  return turbo_graph_run_bind_stream(graph, state, options, NULL, NULL, out_result, out_state);
}

turbo_graph_exec_status_t turbo_graph_run_bind_stream(
    turbo_graph_t *graph, const turbo_runtime_data_bind_value_t *state,
    const turbo_graph_run_options_t *options, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_graph_run_result_t *out_result,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *bound_state = NULL;
  turbo_graph_exec_status_t status;

  if (!out_state) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  *out_state = NULL;

  status = turbo_graph_clone_bind_state(state, &bound_state);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_run_internal_bind(graph, &bound_state,
                                         options && options->start_node ? options->start_node
                                                                         : turbo_graph_get_entry(graph),
                                         0, options, event_sink, event_sink_user_data, out_result);
  if (status != TURBO_GRAPH_EXEC_OK && status != TURBO_GRAPH_EXEC_STOP &&
      status != TURBO_GRAPH_EXEC_INTERRUPTED) {
    turbo_runtime_data_bind_value_destroy(bound_state);
    return status;
  }

  *out_state = bound_state;
  return status;
}

turbo_graph_exec_status_t
turbo_graph_run_checkpoint(turbo_graph_t *graph, turbo_graph_checkpoint_t *checkpoint,
                           const turbo_graph_run_options_t *options,
                           turbo_graph_run_result_t *out_result) {
  if (!graph || !checkpoint) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INVALID_ARGUMENT, NULL, NULL, 0);
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  if (!turbo_graph_checkpoint_matches_graph(checkpoint, graph)) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_CHECKPOINT_MISMATCH, NULL,
                            checkpoint->next_node, checkpoint->steps);
    return TURBO_GRAPH_EXEC_CHECKPOINT_MISMATCH;
  }
  return turbo_graph_run_internal(graph, checkpoint->state, checkpoint->next_node,
                                  checkpoint->steps, options, out_result);
}

turbo_graph_exec_status_t
turbo_graph_run_checkpoint_bind(turbo_graph_t *graph, const turbo_graph_checkpoint_t *checkpoint,
                                const turbo_graph_run_options_t *options,
                                turbo_graph_run_result_t *out_result,
                                turbo_runtime_data_bind_value_t **out_state) {
  return turbo_graph_run_checkpoint_bind_stream(graph, checkpoint, options, NULL, NULL, out_result,
                                                out_state);
}

turbo_graph_exec_status_t turbo_graph_run_checkpoint_bind_stream(
    turbo_graph_t *graph, const turbo_graph_checkpoint_t *checkpoint,
    const turbo_graph_run_options_t *options, turbo_event_sink_bind_fn event_sink,
    void *event_sink_user_data, turbo_graph_run_result_t *out_result,
    turbo_runtime_data_bind_value_t **out_state) {
  turbo_runtime_data_bind_value_t *bound_state = NULL;
  turbo_graph_exec_status_t status;

  if (!graph || !checkpoint || !out_state) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_INVALID_ARGUMENT, NULL, NULL, 0);
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  if (!turbo_graph_checkpoint_matches_graph(checkpoint, graph)) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_CHECKPOINT_MISMATCH, NULL,
                            checkpoint->next_node, checkpoint->steps);
    return TURBO_GRAPH_EXEC_CHECKPOINT_MISMATCH;
  }

  *out_state = NULL;
  bound_state = turbo_runtime_data_bind_value_from_json(checkpoint->state);
  if (!bound_state) {
    turbo_graph_result_init(out_result, TURBO_GRAPH_EXEC_OUT_OF_MEMORY, NULL, checkpoint->next_node,
                            checkpoint->steps);
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  status = turbo_graph_run_internal_bind(graph, &bound_state, checkpoint->next_node,
                                         checkpoint->steps, options, event_sink,
                                         event_sink_user_data, out_result);
  if (status != TURBO_GRAPH_EXEC_OK && status != TURBO_GRAPH_EXEC_STOP &&
      status != TURBO_GRAPH_EXEC_INTERRUPTED) {
    turbo_runtime_data_bind_value_destroy(bound_state);
    return status;
  }

  *out_state = bound_state;
  return status;
}

turbo_graph_exec_status_t
turbo_graph_checkpoint_create(const char *next_node, size_t steps, const json_value_t *state,
                              turbo_graph_checkpoint_t **out_checkpoint) {
  turbo_graph_checkpoint_t *checkpoint;
  turbo_graph_exec_status_t status;

  if (!next_node || !out_checkpoint) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  checkpoint = (turbo_graph_checkpoint_t *)calloc(1, sizeof(*checkpoint));
  if (!checkpoint) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  checkpoint->next_node = turbo_graph_strdup(next_node);
  if (!checkpoint->next_node) {
    free(checkpoint);
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  checkpoint->steps = steps;
  status = turbo_graph_clone_json(state, &checkpoint->state);
  if (status != TURBO_GRAPH_EXEC_OK) {
    free(checkpoint->next_node);
    free(checkpoint);
    return status;
  }

  *out_checkpoint = checkpoint;
  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t
turbo_graph_checkpoint_create_bind(const char *next_node, size_t steps,
                                   const turbo_runtime_data_bind_value_t *state,
                                   turbo_graph_checkpoint_t **out_checkpoint) {
  json_value_t *json_state = NULL;
  turbo_graph_exec_status_t status;

  status = turbo_graph_bind_state_to_json(state, &json_state);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_checkpoint_create(next_node, steps, json_state, out_checkpoint);
  turbo_free_json(&json_state);
  return status;
}

size_t turbo_graph_checkpoint_schema_version(void) {
  return TURBO_GRAPH_CHECKPOINT_SCHEMA_VERSION;
}

void turbo_graph_checkpoint_destroy(turbo_graph_checkpoint_t *checkpoint) {
  if (!checkpoint) {
    return;
  }

  free(checkpoint->next_node);
  free(checkpoint->graph_name);
  turbo_free_json(&checkpoint->state);
  free(checkpoint);
}

char *turbo_graph_checkpoint_serialize(const turbo_graph_checkpoint_t *checkpoint,
                                       size_t *out_len) {
  char *serialized;
  json_value_t *root;
  json_value_t *state_copy = NULL;
  char topology_hash_hex[17];

  if (!checkpoint || !checkpoint->next_node || !checkpoint->state) {
    return NULL;
  }

  root = turbo_json_create_object();
  if (!root) {
    return NULL;
  }

  if (turbo_graph_clone_json(checkpoint->state, &state_copy) != TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&root);
    return NULL;
  }

  turbo_json_object_set_number(root, "checkpoint_version",
                               (double)TURBO_GRAPH_CHECKPOINT_SCHEMA_VERSION);
  turbo_json_object_set_string(root, "next_node", checkpoint->next_node);
  turbo_json_object_set_number(root, "steps", (double)checkpoint->steps);
  if (checkpoint->has_topology) {
    snprintf(topology_hash_hex, sizeof(topology_hash_hex), "%016llx",
             (unsigned long long)checkpoint->topology_hash);
    turbo_json_object_set_string(root, "graph_name",
                                 checkpoint->graph_name ? checkpoint->graph_name : "");
    turbo_json_object_set_string(root, "topology_hash", topology_hash_hex);
  }
  turbo_json_object_add(root, "state", state_copy);

  serialized = turbo_json_serialize(root, out_len);
  turbo_free_json(&root);
  return serialized;
}

turbo_graph_exec_status_t
turbo_graph_checkpoint_deserialize(const char *json, size_t len,
                                   turbo_graph_checkpoint_t **out_checkpoint) {
  turbo_graph_checkpoint_t *checkpoint = NULL;
  json_value_t *root = NULL;
  json_value_t *state = NULL;
  const char *next_node;
  const char *graph_name;
  const char *topology_hash_text;
  size_t checkpoint_version;
  turbo_graph_exec_status_t status;

  if (!json || !out_checkpoint) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  if (turbo_parse_json((const uint8_t *)json, len, &root) != 0) {
    return TURBO_GRAPH_EXEC_ERROR;
  }

  next_node = turbo_json_get_string(root, "next_node");
  graph_name = turbo_json_get_string(root, "graph_name");
  topology_hash_text = turbo_json_get_string(root, "topology_hash");
  state = turbo_json_object_get(root, "state");
  checkpoint_version = (size_t)turbo_json_get_double(root, "checkpoint_version", 0);
  if ((checkpoint_version != 0 &&
       checkpoint_version != TURBO_GRAPH_CHECKPOINT_SCHEMA_VERSION) ||
      !next_node || !state) {
    turbo_free_json(&root);
    return TURBO_GRAPH_EXEC_ERROR;
  }

  status = turbo_graph_checkpoint_create(next_node, (size_t)turbo_json_get_double(root, "steps", 0),
                                         state, &checkpoint);
  if (status != TURBO_GRAPH_EXEC_OK) {
    turbo_free_json(&root);
    return status;
  }

  if (checkpoint_version >= 2) {
    checkpoint->has_topology = topology_hash_text && topology_hash_text[0] != '\0' ? 1 : 0;
    if (checkpoint->has_topology) {
      checkpoint->topology_hash = (uint64_t)strtoull(topology_hash_text, NULL, 16);
    }
    if (graph_name && graph_name[0] != '\0') {
      checkpoint->graph_name = turbo_graph_strdup(graph_name);
      if (!checkpoint->graph_name) {
        turbo_graph_checkpoint_destroy(checkpoint);
        turbo_free_json(&root);
        return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
      }
    }
  }

  turbo_free_json(&root);

  *out_checkpoint = checkpoint;
  return TURBO_GRAPH_EXEC_OK;
}

const char *turbo_graph_checkpoint_next_node(const turbo_graph_checkpoint_t *checkpoint) {
  return checkpoint ? checkpoint->next_node : NULL;
}

size_t turbo_graph_checkpoint_steps(const turbo_graph_checkpoint_t *checkpoint) {
  return checkpoint ? checkpoint->steps : 0;
}

json_value_t *turbo_graph_checkpoint_state(turbo_graph_checkpoint_t *checkpoint) {
  return checkpoint ? checkpoint->state : NULL;
}

turbo_runtime_data_bind_value_t *
turbo_graph_checkpoint_state_bind(const turbo_graph_checkpoint_t *checkpoint) {
  if (!checkpoint) {
    return NULL;
  }

  return turbo_runtime_data_bind_value_from_json(checkpoint->state);
}
