#include "tinytest.h"
#include "turbo_agent_test_support.h"
#include "turbo_event.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_state.h"

#include <platform.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define runtime_find_command_descriptor turbo_agent_test_find_command_descriptor
#define runtime_check_string_array_contains_all turbo_agent_test_check_string_array_contains_all
#define runtime_check_thread_timeline_bind turbo_agent_test_check_thread_timeline_bind
#define runtime_check_lineage_string_field turbo_agent_test_check_lineage_string_field
#define runtime_find_lineage_branch turbo_agent_test_find_lineage_branch
#define runtime_check_lineage_branch turbo_agent_test_check_lineage_branch
#define runtime_check_thread_lineage_bind turbo_agent_test_check_thread_lineage_bind
#define runtime_check_checkpoint_context turbo_agent_test_check_checkpoint_context
#define runtime_check_command_descriptor_example turbo_agent_test_check_command_descriptor_example
#define runtime_check_command_descriptor_fixture turbo_agent_test_check_command_descriptor_fixture
#define runtime_load_fixture_json turbo_agent_test_load_fixture_json
#define runtime_check_string_array_equals_fixture turbo_agent_test_check_string_array_equals_fixture
#define runtime_find_branch_tree_branch turbo_agent_test_find_branch_tree_branch
#define runtime_find_branch_tree_edge turbo_agent_test_find_branch_tree_edge
#define runtime_check_branch_tree_branch turbo_agent_test_check_branch_tree_branch
#define runtime_check_branch_tree_edge turbo_agent_test_check_branch_tree_edge
#define runtime_check_branch_tree turbo_agent_test_check_branch_tree
#define runtime_replay_capture_t turbo_agent_test_replay_capture_t
#define runtime_capture_replayed_history_event turbo_agent_test_capture_replayed_history_event

CXX_C_API int turbo_agent_runtime_apply_state_patch_bind(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_runtime_apply_checkpoint_state_patch_bind(
    turbo_agent_runtime_t *runtime, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_runtime_resume_checkpoint_state_patch_bind_graph(
    turbo_agent_runtime_t *runtime, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state);
#define runtime_observer_capture_t turbo_agent_test_observer_capture_t
#define runtime_capture_observer_event turbo_agent_test_capture_observer_event

typedef struct {
  const char *key;
  int value;
} runtime_bool_write_t;

typedef struct {
  const char *name;
  const char *theme;
  const char *locale;
} runtime_patch_state_write_t;

static char *runtime_test_strdup(const char *text) {
  size_t len;
  char *copy;

  check_not_null(text);
  len = strlen(text);
  copy = (char *)malloc(len + 1);
  check_not_null(copy);
  memcpy(copy, text, len + 1);
  return copy;
}

static int runtime_write_bool_bind_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  runtime_bool_write_t *write = (runtime_bool_write_t *)user_data;
  turbo_runtime_data_bind_value_t *value;

  value = turbo_runtime_data_bind_value_create_bool(write->value);
  check_not_null(value);
  return turbo_runtime_data_bind_object_set(ctx->bind_state, write->key, value) ==
                 TURBO_RUNTIME_DATA_BIND_OK
             ? 0
             : -1;
}

static turbo_graph_t *create_runtime_review_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("runtime-review");
  static runtime_bool_write_t start = {"visited_start", 1};
  static runtime_bool_write_t end = {"visited_end", 1};

  check_not_null(graph);
  check_int_eq(turbo_graph_add_bind_node(graph, "start", runtime_write_bool_bind_node, &start),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_node(graph, "end", runtime_write_bool_bind_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "start", "review", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "review", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static turbo_graph_t *create_runtime_simple_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("runtime-simple");
  static runtime_bool_write_t start = {"visited_start", 1};
  static runtime_bool_write_t end = {"visited_end", 1};

  check_not_null(graph);
  check_int_eq(turbo_graph_add_bind_node(graph, "start", runtime_write_bool_bind_node, &start),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_node(graph, "end", runtime_write_bool_bind_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "start", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static turbo_runtime_data_bind_value_t *create_review_state_bind(int approved) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_request_review(state, "need review"), 0);
  check_int_eq(turbo_agent_state_set_review_approved(state, approved), 0);
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static turbo_runtime_data_bind_value_t *create_supervisor_review_state_bind(void) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
  check_int_eq(turbo_agent_state_request_handoff(state, "executor", "delegate execution"), 0);
  check_int_eq(turbo_agent_state_request_review(state, "need review"), 0);
  check_int_eq(turbo_agent_state_set_review_approved(state, 0), 0);
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static turbo_runtime_data_bind_value_t *create_replan_error_state_bind(void) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_request_replan(state, "manual retry"), 0);
  check_int_eq(turbo_agent_state_set_model_error(state, "transport", "timeout"), 0);
  check_int_eq(turbo_agent_state_set_guardrail_rejection(state, "after_tool", "unsafe output"),
               0);
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static const json_value_t *find_runtime_observability_index(const json_value_t *indexes_json,
                                                            const char *thread_id) {
  size_t count;
  size_t i;

  if (!indexes_json || turbo_json_type(indexes_json) != TURBO_JSON_ARRAY || !thread_id) {
    return NULL;
  }
  count = turbo_json_array_size(indexes_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *item_json = turbo_json_array_get(indexes_json, i);
    const json_value_t *thread_json;
    const char *item_thread_id;

    if (!item_json || turbo_json_type(item_json) != TURBO_JSON_OBJECT) {
      continue;
    }
    thread_json = turbo_json_object_get(item_json, "thread");
    item_thread_id =
        (thread_json && turbo_json_type(thread_json) == TURBO_JSON_OBJECT)
            ? turbo_json_get_string(thread_json, "id")
            : NULL;
    if (item_thread_id && strcmp(item_thread_id, thread_id) == 0) {
      return item_json;
    }
  }
  return NULL;
}

static json_value_t *runtime_observability_thread_ids_json(const json_value_t *indexes_json) {
  json_value_t *thread_ids_json = NULL;
  size_t count;
  size_t i;

  if (!indexes_json || turbo_json_type(indexes_json) != TURBO_JSON_ARRAY) {
    return NULL;
  }
  thread_ids_json = turbo_json_create_array();
  if (!thread_ids_json) {
    return NULL;
  }
  count = turbo_json_array_size(indexes_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *item_json = turbo_json_array_get(indexes_json, i);
    const json_value_t *thread_json;
    const char *thread_id;

    if (!item_json || turbo_json_type(item_json) != TURBO_JSON_OBJECT) {
      turbo_free_json(&thread_ids_json);
      return NULL;
    }
    thread_json = turbo_json_object_get(item_json, "thread");
    thread_id = (thread_json && turbo_json_type(thread_json) == TURBO_JSON_OBJECT)
                    ? turbo_json_get_string(thread_json, "id")
                    : NULL;
    if (!thread_id) {
      turbo_free_json(&thread_ids_json);
      return NULL;
    }
    turbo_json_array_add(thread_ids_json, turbo_json_create_string(thread_id));
  }
  return thread_ids_json;
}

static turbo_runtime_data_bind_value_t *create_trace_seed_state_bind(void) {
  turbo_runtime_data_bind_value_t *state = turbo_agent_state_create_bind();
  turbo_runtime_data_bind_value_t *event =
      turbo_event_trace_create_bind("seed_trace", "start", "seed", 0);

  check_not_null(state);
  check_not_null(event);
  check_int_eq(turbo_agent_state_add_trace_event_bind(state, event), 0);
  turbo_runtime_data_bind_value_destroy(event);
  return state;
}

static int runtime_patch_state_write_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  runtime_patch_state_write_t *write = (runtime_patch_state_write_t *)user_data;
  turbo_runtime_data_bind_value_t *profile = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *settings = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *labels = turbo_runtime_data_bind_value_create_array();

  (void)write;
  check_not_null(profile);
  check_not_null(settings);
  check_not_null(labels);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   ctx->bind_state, "visited_start",
                   turbo_runtime_data_bind_value_create_bool(1)),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   profile, "name", turbo_runtime_data_bind_value_create_string("alpha")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "theme", turbo_runtime_data_bind_value_create_string("light")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "locale", turbo_runtime_data_bind_value_create_string("en")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "notes", turbo_runtime_data_bind_value_create_string("keep")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(profile, "settings", settings),
               TURBO_RUNTIME_DATA_BIND_OK);
  settings = NULL;
  check_int_eq(turbo_runtime_data_bind_array_append(
                   labels, turbo_runtime_data_bind_value_create_string("seed")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(profile, "labels", labels),
               TURBO_RUNTIME_DATA_BIND_OK);
  labels = NULL;
  return turbo_runtime_data_bind_object_set(ctx->bind_state, "profile", profile) ==
                 TURBO_RUNTIME_DATA_BIND_OK
             ? 0
             : -1;
}

static turbo_graph_t *create_runtime_state_patch_graph(void) {
  static runtime_patch_state_write_t start = {"alpha", "light", "en"};
  static runtime_bool_write_t end = {"visited_end", 1};
  turbo_graph_t *graph = turbo_graph_create("runtime-state-patch");

  check_not_null(graph);
  check_int_eq(turbo_graph_add_bind_node(graph, "start", runtime_patch_state_write_node, &start),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_node(graph, "end", runtime_write_bool_bind_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "start", "review", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "review", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static turbo_runtime_data_bind_value_t *create_runtime_state_patch_bind(void) {
  turbo_runtime_data_bind_value_t *patch = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *profile = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *settings = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *labels = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *flags_array;

  check_not_null(patch);
  check_not_null(profile);
  check_not_null(settings);
  check_not_null(labels);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "theme", turbo_runtime_data_bind_value_create_string("dark")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "extra", turbo_runtime_data_bind_value_create_string("enabled")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(settings, "notes",
                                                  turbo_runtime_data_bind_value_create_null()),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(
                   labels, turbo_runtime_data_bind_value_create_string("patched")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(
                   labels, turbo_runtime_data_bind_value_create_string("v3")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(profile, "settings", settings),
               TURBO_RUNTIME_DATA_BIND_OK);
  settings = NULL;
  check_int_eq(turbo_runtime_data_bind_object_set(profile, "labels", labels),
               TURBO_RUNTIME_DATA_BIND_OK);
  labels = NULL;
  check_int_eq(turbo_runtime_data_bind_object_set(patch, "profile", profile),
               TURBO_RUNTIME_DATA_BIND_OK);
  profile = NULL;
  check_int_eq(turbo_runtime_data_bind_object_set(
                   patch, "flags", turbo_runtime_data_bind_value_create_array()),
               TURBO_RUNTIME_DATA_BIND_OK);
  flags_array = (turbo_runtime_data_bind_value_t *)turbo_runtime_data_bind_object_get(patch,
                                                                                      "flags");
  check_not_null(flags_array);
  check_int_eq(turbo_runtime_data_bind_array_append(
                   flags_array,
                   turbo_runtime_data_bind_value_create_string("flag-a")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(
                   flags_array,
                   turbo_runtime_data_bind_value_create_string("flag-b")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   patch, "patch_version", turbo_runtime_data_bind_value_create_int64(3)),
               TURBO_RUNTIME_DATA_BIND_OK);
  return patch;
}

static turbo_runtime_data_bind_value_t *
create_override_from_result(const turbo_runtime_data_bind_value_t *result_state, int approved) {
  json_value_t *state = turbo_runtime_data_bind_value_to_json(result_state);
  turbo_runtime_data_bind_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_review_approved(state, approved), 0);
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static char *create_runtime_temp_root(void) {
  int needed;
  char *path;
  unsigned long long tick = (unsigned long long)turbo_hrtime();

#ifdef _WIN32
  char temp_dir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, temp_dir);
  check_true(len > 0);
  needed = snprintf(NULL, 0, "%sturbonet_runtime_%llx", temp_dir, tick);
#else
  const char *temp_dir = "/tmp/";
  needed = snprintf(NULL, 0, "%sturbonet_runtime_%llx", temp_dir, tick);
#endif
  path = (char *)malloc((size_t)needed + 1);
  check_not_null(path);
#ifdef _WIN32
  snprintf(path, (size_t)needed + 1, "%sturbonet_runtime_%llx", temp_dir, tick);
  _mkdir(path);
#else
  snprintf(path, (size_t)needed + 1, "%sturbonet_runtime_%llx", temp_dir, tick);
  mkdir(path, 0777);
#endif
  return path;
}

static char *runtime_test_record_file(const char *root, const char *collection, const char *id) {
  int needed;
  char *path;

  check_not_null(root);
  check_not_null(collection);
  check_not_null(id);
  needed = snprintf(NULL, 0, "%s/%s/%s.json", root, collection, id);
  path = (char *)malloc((size_t)needed + 1);
  check_not_null(path);
  snprintf(path, (size_t)needed + 1, "%s/%s/%s.json", root, collection, id);
  return path;
}

static void runtime_test_write_text(const char *path, const char *text) {
  FILE *fp;

  check_not_null(path);
  check_not_null(text);
  fp = fopen(path, "wb");
  check_not_null(fp);
  check_size_eq(fwrite(text, 1, strlen(text), fp), strlen(text));
  fclose(fp);
}

spec("turbo agent runtime api") {

  it("should persist one completed run without checkpoints in memory store") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_simple_graph();
    turbo_runtime_data_bind_value_t *state = turbo_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *runs = NULL;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, NULL, "", &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(strncmp(turbo_json_get_string(summary, "thread_id"), "thr_", 4) == 0);
    check_true(turbo_json_type(turbo_json_object_get(summary, "checkpoint_id")) == TURBO_JSON_NULL);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(result_state, "visited_end"), 0));
    check_int_eq(turbo_agent_runtime_list_runs(runtime, turbo_json_get_string(summary, "thread_id"),
                                               &runs),
                 0);
    check_size_eq(turbo_json_array_size(runs), 1);

    turbo_free_json(&runs);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should expose thread timeline for an interrupted thread") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *timeline = NULL;
    turbo_runtime_data_bind_value_t *history_events = NULL;
    json_value_t *branch_tree = NULL;
    json_value_t *lineage = NULL;
    json_value_t *summary = NULL;
    runtime_replay_capture_t replay = {0};
    turbo_graph_run_options_t options = {0};
    const char *interrupt_before_review[] = {"review"};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, "",
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_runtime_get_thread_timeline_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"), &timeline),
                 0);
    check_int_eq(turbo_agent_runtime_load_thread_history_events_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"), &history_events),
                 0);
    check_int_eq(turbo_agent_runtime_replay_thread_history_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"),
                     runtime_capture_replayed_history_event, &replay),
                 0);
    check_size_eq(replay.count, turbo_runtime_data_bind_value_size(history_events));
    check_true(replay.count >= 1);
    runtime_check_thread_timeline_bind(timeline, turbo_json_get_string(summary, "thread_id"),
                                       turbo_json_get_string(summary, "run_id"),
                                       turbo_json_get_string(summary, "checkpoint_id"), 1, 1, 1,
                                       1);
    check_int_eq(turbo_agent_runtime_list_thread_lineage(runtime,
                                                         turbo_json_get_string(summary, "thread_id"),
                                                         &lineage),
                 0);
    runtime_check_thread_lineage_bind(lineage, turbo_json_get_string(summary, "thread_id"),
                                      turbo_json_get_string(summary, "run_id"),
                                      turbo_json_get_string(summary, "run_id"),
                                      turbo_json_get_string(summary, "checkpoint_id"));
    check_size_eq(turbo_json_array_size(turbo_json_object_get(lineage, "branches")), 1);
    runtime_check_lineage_branch(turbo_json_array_get(turbo_json_object_get(lineage, "branches"), 0),
                                 NULL, NULL, NULL,
                                 turbo_json_get_string(summary, "checkpoint_id"));
    check_int_eq(turbo_agent_runtime_get_branch_tree(runtime, turbo_json_get_string(summary, "thread_id"),
                                                     &branch_tree),
                 0);
    runtime_check_branch_tree(branch_tree, turbo_json_get_string(summary, "thread_id"),
                              turbo_json_get_string(summary, "run_id"),
                              turbo_json_get_string(summary, "run_id"),
                              turbo_json_get_string(summary, "run_id"),
                              turbo_json_get_string(summary, "checkpoint_id"), 1, 0);
    runtime_check_branch_tree_branch(
        runtime_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"),
                                        turbo_json_get_string(summary, "run_id")),
        turbo_json_get_string(summary, "run_id"), NULL, NULL,
        turbo_json_get_string(summary, "checkpoint_id"));

    turbo_free_json(&branch_tree);
    turbo_free_json(&lineage);
    turbo_runtime_data_bind_value_destroy(history_events);
    turbo_runtime_data_bind_value_destroy(timeline);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should bridge durable history through runtime observer events") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(1);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    runtime_observer_capture_t thread_capture = {0};
    runtime_observer_capture_t checkpoint_capture = {0};
    turbo_agent_observer_bind_sink_t thread_sink = {0};
    turbo_agent_observer_bind_sink_t checkpoint_sink = {0};
    const char *interrupt_before_end[] = {"end"};
    turbo_graph_run_options_t options = {0};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_end;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, "",
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    thread_sink.callback = runtime_capture_observer_event;
    thread_sink.user_data = &thread_capture;
    checkpoint_sink.callback = runtime_capture_observer_event;
    checkpoint_sink.user_data = &checkpoint_capture;

    check_int_eq(turbo_agent_runtime_observe_thread_history_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"), &thread_sink),
                 0);
    check_int_eq(turbo_agent_runtime_observe_history_bind(
                     runtime, NULL, turbo_json_get_string(summary, "checkpoint_id"),
                     &checkpoint_sink),
                 0);
    check_true(thread_capture.count >= 1);
    check_true(thread_capture.interrupted_count >= 1);
    check_size_eq(checkpoint_capture.count, thread_capture.count);
    check_size_eq(checkpoint_capture.interrupted_count, thread_capture.interrupted_count);

    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should expose thread timeline for a completed-only thread") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_simple_graph();
    turbo_runtime_data_bind_value_t *state = turbo_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *thread_state = NULL;
    turbo_runtime_data_bind_value_t *run_state = NULL;
    turbo_runtime_data_bind_value_t *replay_override = NULL;
    turbo_runtime_data_bind_value_t *replay_state = NULL;
    turbo_runtime_data_bind_value_t *timeline = NULL;
    runtime_replay_capture_t replay = {0};
    json_value_t *branch_tree = NULL;
    json_value_t *summary = NULL;
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *replay_summary = NULL;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, NULL, "",
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(turbo_json_type(turbo_json_object_get(summary, "checkpoint_id")) == TURBO_JSON_NULL);

    check_int_eq(turbo_agent_runtime_get_thread_timeline_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"), &timeline),
                 0);
    check_int_eq(turbo_agent_runtime_replay_thread_history_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"),
                     runtime_capture_replayed_history_event, &replay),
                 0);
    check_size_eq(replay.count, 0);
    runtime_check_thread_timeline_bind(timeline, turbo_json_get_string(summary, "thread_id"),
                                       turbo_json_get_string(summary, "run_id"), NULL, 0, 1, 0,
                                       0);
    check_int_eq(turbo_agent_runtime_get_thread_state_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"), &thread_state),
                 0);
    check_int_eq(
        turbo_agent_runtime_get_run_state_bind(runtime, turbo_json_get_string(summary, "run_id"),
                                               &run_state),
        0);
    replay_override = thread_state;
    check_true(turbo_agent_runtime_resume_thread_bind_graph(
                   runtime, graph, turbo_json_get_string(summary, "thread_id"), replay_override,
                   NULL, &replay_summary, &replay_state) != 0);
    check_null(replay_summary);
    check_null(replay_state);
    check_int_eq(turbo_agent_runtime_get_branch_tree(runtime, turbo_json_get_string(summary, "thread_id"),
                                                     &branch_tree),
                 0);
    runtime_check_branch_tree(branch_tree, turbo_json_get_string(summary, "thread_id"),
                              turbo_json_get_string(summary, "run_id"),
                              turbo_json_get_string(summary, "run_id"), NULL, NULL, 1, 0);
    runtime_check_branch_tree_branch(
        runtime_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"),
                                        turbo_json_get_string(summary, "run_id")),
        turbo_json_get_string(summary, "run_id"), NULL, NULL, NULL);

    turbo_free_json(&branch_tree);
    turbo_runtime_data_bind_value_destroy(timeline);
    turbo_runtime_data_bind_value_destroy(run_state);
    turbo_runtime_data_bind_value_destroy(thread_state);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should expose one observability index bundle through the runtime surface") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_supervisor_review_state_bind();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *index_json = NULL;
    const json_value_t *counts = NULL;
    const json_value_t *history_events = NULL;
    const json_value_t *trace_events = NULL;
    const json_value_t *current_checkpoint_summary = NULL;
    const json_value_t *thread_timeline = NULL;
    const json_value_t *thread_lineage = NULL;
    const json_value_t *branch_tree = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, "",
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_runtime_get_thread_observability_index(
                     runtime, turbo_json_get_string(summary, "thread_id"), &index_json),
                 0);
    check_not_null(index_json);
    check_str_eq(turbo_json_get_string(turbo_json_object_get(index_json, "thread"), "id"),
                 turbo_json_get_string(summary, "thread_id"));
    check_str_eq(turbo_json_get_string(turbo_json_object_get(index_json, "latest_run"), "id"),
                 turbo_json_get_string(summary, "run_id"));
    check_str_eq(turbo_json_get_string(turbo_json_object_get(index_json, "pending_run"), "id"),
                 turbo_json_get_string(summary, "run_id"));

    thread_timeline = turbo_json_object_get(index_json, "thread_timeline");
    thread_lineage = turbo_json_object_get(index_json, "thread_lineage");
    branch_tree = turbo_json_object_get(index_json, "branch_tree");
    history_events = turbo_json_object_get(index_json, "history_events");
    trace_events = turbo_json_object_get(index_json, "trace_events");
    counts = turbo_json_object_get(index_json, "counts");
    check_not_null(thread_timeline);
    check_not_null(thread_lineage);
    check_not_null(branch_tree);
    check_true(turbo_json_type(history_events) == TURBO_JSON_ARRAY);
    check_true(turbo_json_type(trace_events) == TURBO_JSON_ARRAY);
    check_str_eq(turbo_json_get_string(index_json, "current_status"), "interrupted");
    check_str_eq(turbo_json_get_string(index_json, "current_interrupt_reason"),
                 "review_required");
    check_str_eq(turbo_json_get_string(index_json, "current_pending_action"), "review");
    current_checkpoint_summary = turbo_json_object_get(index_json, "current_checkpoint_summary");
    check_not_null(current_checkpoint_summary);
    check_str_eq(turbo_json_get_string(current_checkpoint_summary, "id"),
                 turbo_json_get_string(summary, "checkpoint_id"));
    check_str_eq(turbo_json_get_string(index_json, "latest_run_status"), "interrupted");
    check_not_null(turbo_json_get_string(index_json, "latest_run_updated_at"));
    check_str_eq(turbo_json_get_string(index_json, "pending_run_id"),
                 turbo_json_get_string(summary, "run_id"));
    check_str_eq(turbo_json_get_string(index_json, "pending_checkpoint_id"),
                 turbo_json_get_string(summary, "checkpoint_id"));
    check_false(turbo_json_get_bool(index_json, "has_failure", true));
    check_false(turbo_json_get_bool(index_json, "has_model_error", true));
    check_false(turbo_json_get_bool(index_json, "has_guardrail_rejection", true));
    check_false(turbo_json_get_bool(index_json, "replan_requested", true));
    check_true(turbo_json_is_null(turbo_json_object_get(index_json, "current_failure_reason")));
    check_str_eq(turbo_json_get_string(index_json, "current_review_note"), "need review");
    check_true(turbo_json_get_bool(index_json, "has_pending_review", false));
    check_true(turbo_json_get_bool(index_json, "has_handoff", false));
    check_str_eq(turbo_json_get_string(index_json, "active_agent"), "planner");
    check_not_null(counts);
    check_int_eq(turbo_json_get_int(counts, "runs", -1), 1);
    check_int_eq(turbo_json_get_int(counts, "interrupted_runs", -1), 1);
    check_int_eq(turbo_json_get_int(counts, "completed_runs", -1), 0);
    check_int_eq(turbo_json_get_int(counts, "current_run_checkpoints", -1), 1);
    check_int_eq(turbo_json_get_int(counts, "branches", -1), 1);
    check_int_eq(turbo_json_get_int(counts, "edges", -1), 0);
    check_int_eq(turbo_json_get_int(counts, "history_events", -1),
                 (int)turbo_json_array_size(history_events));
    check_int_eq(turbo_json_get_int(counts, "trace_events", -1),
                 (int)turbo_json_array_size(trace_events));

    turbo_free_json(&index_json);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should list lightweight observability summaries across persisted threads") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *interrupted_state = create_supervisor_review_state_bind();
    turbo_runtime_data_bind_value_t *completed_state = create_review_state_bind(1);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *interrupted_summary = NULL;
    json_value_t *completed_summary = NULL;
    json_value_t *indexes_json = NULL;
    const json_value_t *interrupted_index = NULL;
    const json_value_t *completed_index = NULL;
    const json_value_t *interrupted_counts = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(interrupted_state);
    check_not_null(completed_state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, interrupted_state, &options,
                                                      "thread_interrupt", &interrupted_summary,
                                                      &result_state),
                 0);
    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;
    check_str_eq(turbo_json_get_string(interrupted_summary, "status"), "interrupted");

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, completed_state, &options,
                                                      "thread_complete", &completed_summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(completed_summary, "status"), "completed");

    check_int_eq(turbo_agent_runtime_list_observability_indexes(runtime, &indexes_json), 0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    check_true(turbo_json_array_size(indexes_json) >= 2);

    interrupted_index = find_runtime_observability_index(indexes_json, "thread_interrupt");
    completed_index = find_runtime_observability_index(indexes_json, "thread_complete");
    check_not_null(interrupted_index);
    check_not_null(completed_index);

    check_str_eq(turbo_json_get_string(turbo_json_object_get(interrupted_index, "thread"), "id"),
                 "thread_interrupt");
    check_str_eq(
        turbo_json_get_string(turbo_json_object_get(interrupted_index, "latest_run"), "id"),
        turbo_json_get_string(interrupted_summary, "run_id"));
    check_str_eq(turbo_json_get_string(interrupted_index, "current_status"), "interrupted");
    check_str_eq(turbo_json_get_string(interrupted_index, "current_interrupt_reason"),
                 "review_required");
    check_str_eq(turbo_json_get_string(interrupted_index, "pending_run_id"),
                 turbo_json_get_string(interrupted_summary, "run_id"));
    check_str_eq(
        turbo_json_get_string(turbo_json_object_get(interrupted_index, "current_checkpoint_summary"),
                              "id"),
        turbo_json_get_string(interrupted_summary, "checkpoint_id"));
    interrupted_counts = turbo_json_object_get(interrupted_index, "counts");
    check_not_null(interrupted_counts);
    check_int_eq(turbo_json_get_int(interrupted_counts, "runs", -1), 1);

    check_str_eq(turbo_json_get_string(turbo_json_object_get(completed_index, "thread"), "id"),
                 "thread_complete");
    check_str_eq(turbo_json_get_string(completed_index, "current_status"), "completed");
    check_str_eq(turbo_json_get_string(completed_index, "latest_run_status"), "completed");
    check_true(turbo_json_is_null(turbo_json_object_get(completed_index, "pending_run")));
    check_true(
        turbo_json_is_null(turbo_json_object_get(completed_index, "current_checkpoint_summary")));

    turbo_free_json(&indexes_json);
    turbo_free_json(&completed_summary);
    turbo_free_json(&interrupted_summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(completed_state);
    turbo_runtime_data_bind_value_destroy(interrupted_state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should filter lightweight observability summaries across persisted threads") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_graph_t *simple_graph = create_runtime_simple_graph();
    turbo_runtime_data_bind_value_t *interrupted_state = create_supervisor_review_state_bind();
    turbo_runtime_data_bind_value_t *completed_state = create_review_state_bind(1);
    turbo_runtime_data_bind_value_t *replan_error_state = create_replan_error_state_bind();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *interrupted_summary = NULL;
    json_value_t *completed_summary = NULL;
    json_value_t *replan_summary = NULL;
    json_value_t *filters_json = NULL;
    json_value_t *indexes_json = NULL;
    json_value_t *query_fixture_root = NULL;
    json_value_t *thread_ids_json = NULL;
    const json_value_t *matched_index = NULL;
    const json_value_t *query_fixture = NULL;
    const char *interrupt_before_review[] = {"review"};
    const char *interrupt_before_end[] = {"end"};
    turbo_graph_run_options_t options = {0};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(simple_graph);
    check_not_null(interrupted_state);
    check_not_null(completed_state);
    check_not_null(replan_error_state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, interrupted_state, &options,
                                                      "thread_interrupt", &interrupted_summary,
                                                      &result_state),
                 0);
    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, completed_state, &options,
                                                      "thread_complete", &completed_summary,
                                                      &result_state),
                 0);
    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;

    options.interrupt_before_nodes = interrupt_before_end;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, simple_graph, replan_error_state,
                                                      &options, "thread_replan", &replan_summary,
                                                      &result_state),
                 0);
    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;

    query_fixture_root = runtime_load_fixture_json("observability_queries.golden.json");
    check_not_null(query_fixture_root);

    query_fixture =
        turbo_agent_test_find_named_fixture_entry(query_fixture_root, "review_threads_sorted");
    check_not_null(query_fixture);
    filters_json = turbo_json_clone(turbo_json_object_get(query_fixture, "filters"));
    check_not_null(filters_json);
    check_int_eq(
        turbo_agent_runtime_list_observability_indexes_filtered(runtime, filters_json, &indexes_json),
        0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    thread_ids_json = runtime_observability_thread_ids_json(indexes_json);
    check_not_null(thread_ids_json);
    runtime_check_string_array_equals_fixture(thread_ids_json,
                                              turbo_json_object_get(query_fixture,
                                                                    "expected_thread_ids"));
    matched_index = find_runtime_observability_index(indexes_json, "thread_interrupt");
    check_not_null(matched_index);
    check_str_eq(turbo_json_get_string(matched_index, "current_status"), "interrupted");
    check_true(turbo_json_get_bool(matched_index, "has_pending_review", false));
    check_true(turbo_json_get_bool(matched_index, "has_handoff", false));
    check_str_eq(turbo_json_get_string(matched_index, "active_agent"), "planner");
    turbo_free_json(&thread_ids_json);
    turbo_free_json(&indexes_json);
    turbo_free_json(&filters_json);

    filters_json = turbo_json_create_object();
    check_not_null(filters_json);
    turbo_json_object_set_string(filters_json, "status", "completed");
    turbo_json_object_set_bool(filters_json, "has_handoff", false);
    check_int_eq(
        turbo_agent_runtime_list_observability_indexes_filtered(runtime, filters_json, &indexes_json),
        0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    check_int_eq((int)turbo_json_array_size(indexes_json), 1);
    matched_index = find_runtime_observability_index(indexes_json, "thread_complete");
    check_not_null(matched_index);
    check_str_eq(turbo_json_get_string(matched_index, "current_status"), "completed");
    turbo_free_json(&indexes_json);
    turbo_free_json(&filters_json);

    query_fixture =
        turbo_agent_test_find_named_fixture_entry(query_fixture_root, "replan_error_threads");
    check_not_null(query_fixture);
    filters_json = turbo_json_clone(turbo_json_object_get(query_fixture, "filters"));
    check_not_null(filters_json);
    check_int_eq(
        turbo_agent_runtime_list_observability_indexes_filtered(runtime, filters_json, &indexes_json),
        0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    thread_ids_json = runtime_observability_thread_ids_json(indexes_json);
    check_not_null(thread_ids_json);
    runtime_check_string_array_equals_fixture(thread_ids_json,
                                              turbo_json_object_get(query_fixture,
                                                                    "expected_thread_ids"));
    matched_index = find_runtime_observability_index(indexes_json, "thread_replan");
    check_not_null(matched_index);
    check_true(turbo_json_get_bool(matched_index, "has_model_error", false));
    check_true(turbo_json_get_bool(matched_index, "has_guardrail_rejection", false));
    check_true(turbo_json_get_bool(matched_index, "replan_requested", false));
    check_str_eq(turbo_json_get_string(matched_index, "current_interrupt_reason"),
                 "replan_requested");
    check_str_eq(turbo_json_get_string(matched_index, "latest_run_status"), "interrupted");
    turbo_free_json(&thread_ids_json);
    turbo_free_json(&indexes_json);
    turbo_free_json(&filters_json);

    filters_json = turbo_json_create_object();
    check_not_null(filters_json);
    turbo_json_object_set_string(filters_json, "thread_id_prefix", "thread_rep");
    check_int_eq(
        turbo_agent_runtime_list_observability_indexes_filtered(runtime, filters_json, &indexes_json),
        0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    check_int_eq((int)turbo_json_array_size(indexes_json), 1);
    matched_index = find_runtime_observability_index(indexes_json, "thread_replan");
    check_not_null(matched_index);
    turbo_free_json(&indexes_json);
    turbo_free_json(&filters_json);

    filters_json = turbo_json_create_object();
    check_not_null(filters_json);
    turbo_json_object_set_string(filters_json, "latest_run_updated_after",
                                 "9999-12-31T23:59:59Z");
    check_int_eq(
        turbo_agent_runtime_list_observability_indexes_filtered(runtime, filters_json, &indexes_json),
        0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    check_int_eq((int)turbo_json_array_size(indexes_json), 0);
    turbo_free_json(&indexes_json);
    turbo_free_json(&filters_json);

    query_fixture =
        turbo_agent_test_find_named_fixture_entry(query_fixture_root, "thread_id_sorted_limit");
    check_not_null(query_fixture);
    filters_json = turbo_json_clone(turbo_json_object_get(query_fixture, "filters"));
    check_not_null(filters_json);
    check_int_eq(
        turbo_agent_runtime_list_observability_indexes_filtered(runtime, filters_json, &indexes_json),
        0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    thread_ids_json = runtime_observability_thread_ids_json(indexes_json);
    check_not_null(thread_ids_json);
    runtime_check_string_array_equals_fixture(thread_ids_json,
                                              turbo_json_object_get(query_fixture,
                                                                    "expected_thread_ids"));
    turbo_free_json(&thread_ids_json);
    turbo_free_json(&indexes_json);
    turbo_free_json(&filters_json);

    filters_json = turbo_json_create_object();
    check_not_null(filters_json);
    turbo_json_object_set_string(filters_json, "sort_by", "thread_id");
    turbo_json_object_set_string(filters_json, "sort_order", "desc");
    turbo_json_object_set_number(filters_json, "limit", 1);
    check_int_eq(
        turbo_agent_runtime_list_observability_indexes_filtered(runtime, filters_json, &indexes_json),
        0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    check_int_eq((int)turbo_json_array_size(indexes_json), 1);
    check_str_eq(
        turbo_json_get_string(turbo_json_object_get(turbo_json_array_get(indexes_json, 0), "thread"),
                              "id"),
        "thread_replan");
    turbo_free_json(&indexes_json);
    turbo_free_json(&filters_json);

    filters_json = turbo_json_create_object();
    check_not_null(filters_json);
    turbo_json_object_set_string(filters_json, "latest_run_updated_before",
                                 "0000-01-01T00:00:00Z");
    check_int_eq(
        turbo_agent_runtime_list_observability_indexes_filtered(runtime, filters_json, &indexes_json),
        0);
    check_true(turbo_json_type(indexes_json) == TURBO_JSON_ARRAY);
    check_int_eq((int)turbo_json_array_size(indexes_json), 0);

    turbo_free_json(&indexes_json);
    turbo_free_json(&filters_json);
    turbo_free_json(&query_fixture_root);
    turbo_free_json(&replan_summary);
    turbo_free_json(&completed_summary);
    turbo_free_json(&interrupted_summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(replan_error_state);
    turbo_runtime_data_bind_value_destroy(completed_state);
    turbo_runtime_data_bind_value_destroy(interrupted_state);
    turbo_graph_destroy(simple_graph);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should expose persisted trace snapshots through runtime trace accessors") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_simple_graph();
    turbo_runtime_data_bind_value_t *state = create_trace_seed_state_bind();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *thread_trace_events = NULL;
    turbo_runtime_data_bind_value_t *run_trace_events = NULL;
    json_value_t *summary = NULL;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, NULL, "",
                                                      &summary, &result_state),
                 0);
    check_int_eq(turbo_agent_runtime_get_thread_trace_events_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"), &thread_trace_events),
                 0);
    check_not_null(thread_trace_events);
    check_size_eq(turbo_runtime_data_bind_value_size(thread_trace_events), 1);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_array_get(thread_trace_events, 0), "name")),
                 "seed_trace");
    check_int_eq(turbo_agent_runtime_get_run_trace_events_bind(
                     runtime, turbo_json_get_string(summary, "run_id"), &run_trace_events),
                 0);
    check_not_null(run_trace_events);
    check_size_eq(turbo_runtime_data_bind_value_size(run_trace_events), 1);

    turbo_runtime_data_bind_value_destroy(run_trace_events);
    turbo_runtime_data_bind_value_destroy(thread_trace_events);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should keep host contract golden fixtures parseable") {
    json_value_t *checkpoint_context =
        runtime_load_fixture_json("checkpoint_context.golden.json");
    json_value_t *branch_tree = runtime_load_fixture_json("forked_branch_tree.golden.json");
    json_value_t *timeline_json =
        runtime_load_fixture_json("interrupted_timeline.golden.json");
    json_value_t *observability_queries =
        runtime_load_fixture_json("observability_queries.golden.json");
    turbo_runtime_data_bind_value_t *timeline = turbo_runtime_data_bind_value_from_json(timeline_json);
    const json_value_t *query_fixture;

    check_not_null(checkpoint_context);
    check_not_null(branch_tree);
    check_not_null(timeline_json);
    check_not_null(observability_queries);
    check_not_null(timeline);

    runtime_check_checkpoint_context(checkpoint_context, "thr_123", "run_123", "ckpt_123", NULL, 1);
    runtime_check_thread_timeline_bind(timeline, "thr_123", "run_123", "ckpt_123", 1, 1, 1, 1);
    runtime_check_branch_tree(branch_tree, "thr_123", "run_456", "run_456", NULL, NULL, 2, 1);
    runtime_check_branch_tree_branch(
        runtime_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"), "run_123"),
        "run_123", NULL, NULL, "ckpt_123");
    runtime_check_branch_tree_branch(
        runtime_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"), "run_456"),
        "run_456", "run_123", "ckpt_123", "ckpt_123");
    runtime_check_branch_tree_edge(
        runtime_find_branch_tree_edge(turbo_json_object_get(branch_tree, "edges"), "ckpt_123",
                                      "run_456"),
        "ckpt_123", "run_456");
    query_fixture =
        turbo_agent_test_find_named_fixture_entry(observability_queries, "thread_id_sorted_limit");
    check_not_null(query_fixture);
    check_true(turbo_json_type(turbo_json_object_get(query_fixture, "filters")) ==
               TURBO_JSON_OBJECT);
    check_true(turbo_json_type(turbo_json_object_get(query_fixture, "expected_thread_ids")) ==
               TURBO_JSON_ARRAY);

    turbo_runtime_data_bind_value_destroy(timeline);
    turbo_free_json(&observability_queries);
    turbo_free_json(&timeline_json);
    turbo_free_json(&branch_tree);
    turbo_free_json(&checkpoint_context);
  }

  it("should merge bind-native state patches through checkpoint replay convenience") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_state_patch_graph();
    turbo_runtime_data_bind_value_t *state = turbo_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *patched_state = NULL;
    turbo_runtime_data_bind_value_t *resumed_state = NULL;
    turbo_runtime_data_bind_value_t *patch = create_runtime_state_patch_bind();
    json_value_t *summary = NULL;
    json_value_t *resumed_summary = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    const turbo_runtime_data_bind_value_t *profile;
    const turbo_runtime_data_bind_value_t *settings;
    const turbo_runtime_data_bind_value_t *labels;
    const turbo_runtime_data_bind_value_t *flags;
    const char *checkpoint_id;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);
    check_not_null(patch);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL,
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
    check_not_null(checkpoint_id);

    check_int_eq(turbo_agent_runtime_update_checkpoint_state_bind(runtime, checkpoint_id, patch,
                                                                  &patched_state),
                 0);
    check_not_null(patched_state);
    profile = turbo_runtime_data_bind_object_get(patched_state, "profile");
    settings = profile ? turbo_runtime_data_bind_object_get(profile, "settings") : NULL;
    labels = profile ? turbo_runtime_data_bind_object_get(profile, "labels") : NULL;
    flags = turbo_runtime_data_bind_object_get(patched_state, "flags");
    check_not_null(profile);
    check_not_null(settings);
    check_not_null(labels);
    check_not_null(flags);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(profile, "name")),
                 "alpha");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(settings, "theme")),
                 "dark");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(settings, "locale")),
                 "en");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(settings, "extra")),
                 "enabled");
    check_true(turbo_runtime_data_bind_value_kind(
                   turbo_runtime_data_bind_object_get(settings, "notes")) ==
               TURBO_RUNTIME_DATA_BIND_VALUE_NULL);
    check_size_eq(turbo_runtime_data_bind_value_size(labels), 2);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_array_get(labels, 0)),
                 "patched");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_array_get(labels, 1)),
                 "v3");
    check_size_eq(turbo_runtime_data_bind_value_size(flags), 2);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_array_get(flags, 0)),
                 "flag-a");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_array_get(flags, 1)),
                 "flag-b");
    check_true(turbo_runtime_data_bind_value_as_int64(
                   turbo_runtime_data_bind_object_get(patched_state, "patch_version"), 0) == 3);

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_runtime_resume_checkpoint_state_bind_graph(
                     runtime, graph, checkpoint_id, patch, &options, &resumed_summary, &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(resumed_summary, "status"), "completed");
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(resumed_state, "visited_end"), 0));
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_object_get(
                             turbo_runtime_data_bind_object_get(resumed_state, "profile"),
                             "settings"),
                         "theme")),
                 "dark");
    check_true(turbo_runtime_data_bind_value_kind(
                   turbo_runtime_data_bind_object_get(
                       turbo_runtime_data_bind_object_get(
                           turbo_runtime_data_bind_object_get(resumed_state, "profile"),
                           "settings"),
                       "notes")) == TURBO_RUNTIME_DATA_BIND_VALUE_NULL);

    turbo_runtime_data_bind_value_destroy(resumed_state);
    turbo_runtime_data_bind_value_destroy(patched_state);
    turbo_runtime_data_bind_value_destroy(patch);
    turbo_free_json(&resumed_summary);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should resume an interrupted thread through the thread-scoped replay wrapper") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *thread_state = NULL;
    turbo_runtime_data_bind_value_t *replay_override = NULL;
    turbo_runtime_data_bind_value_t *replay_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *replay_summary = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL,
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_int_eq(turbo_agent_runtime_get_thread_state_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"), &thread_state),
                 0);
    replay_override = create_override_from_result(thread_state, 1);
    check_not_null(replay_override);
    check_int_eq(turbo_agent_runtime_resume_thread_bind_graph(
                     runtime, graph, turbo_json_get_string(summary, "thread_id"), replay_override,
                     NULL, &replay_summary, &replay_state),
                 0);
    check_str_eq(turbo_json_get_string(replay_summary, "status"), "completed");
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(replay_state, "visited_end"), 0));

    turbo_free_json(&replay_summary);
    turbo_runtime_data_bind_value_destroy(replay_state);
    turbo_runtime_data_bind_value_destroy(replay_override);
    turbo_runtime_data_bind_value_destroy(thread_state);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should interrupt resume fork and replay history through checkpoint records") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(0);
    turbo_runtime_data_bind_value_t *override = NULL;
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *history = NULL;
    json_value_t *summary = NULL;
    json_value_t *summary2 = NULL;
    json_value_t *summary3 = NULL;
    json_value_t *run = NULL;
    json_value_t *latest_run = NULL;
    json_value_t *pending_run = NULL;
    json_value_t *checkpoint = NULL;
    json_value_t *latest_checkpoint = NULL;
    json_value_t *checkpoints = NULL;
    json_value_t *lineage = NULL;
    json_value_t *branch_tree = NULL;
    json_value_t *checkpoint_context = NULL;
    turbo_graph_run_options_t options = {0};
    const char *interrupt_before_review[] = {"review"};
    const char *interrupt_before_end[] = {"end"};
    const char *first_checkpoint_id;
    const char *second_checkpoint_id;
    const char *run_id;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL,
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_str_eq(turbo_json_get_string(summary, "interrupt_reason"), "review_required");
    check_str_eq(turbo_json_get_string(summary, "pending_node"), "review");
    check_str_eq(turbo_json_get_string(summary, "pending_action"), "review");
    check_size_eq(turbo_json_array_size(turbo_json_object_get(summary, "available_commands")), 6);
    check_size_eq(
        turbo_json_array_size(turbo_json_object_get(summary, "available_command_descriptors")), 6);
    {
      const json_value_t *approve_descriptor =
          runtime_find_command_descriptor(summary, "approve_review");
      const json_value_t *request_replan_descriptor =
          runtime_find_command_descriptor(summary, "request_replan");
      const json_value_t *append_feedback_descriptor =
          runtime_find_command_descriptor(summary, "append_feedback");
      const json_value_t *override_descriptor =
          runtime_find_command_descriptor(summary, "override_final_output");
      const json_value_t *approve_args_schema;
      const json_value_t *request_replan_accepted_keys;
      const json_value_t *request_replan_fallback_keys;
      const json_value_t *append_feedback_accepted_keys;
      const json_value_t *feedback_required;
      const json_value_t *feedback_example_payload;
      const json_value_t *override_example_payload;
      const json_value_t *override_accepted_keys;
      const json_value_t *override_properties;
      const char *const request_replan_expected_accepts[] = {"reason", "text", "message"};
      const char *const request_replan_expected_fallbacks[] = {"text", "message"};
      const char *const append_feedback_expected_accepts[] = {"text", "message"};
      const char *const override_expected_accepts[] = {
          "text", "output_text", "message", "output_json", "output"};

      check_not_null(approve_descriptor);
      check_not_null(request_replan_descriptor);
      check_not_null(append_feedback_descriptor);
      check_not_null(override_descriptor);
      check_str_eq(turbo_json_get_string(approve_descriptor, "label"), "Approve Review");
      check_str_eq(turbo_json_get_string(approve_descriptor, "category"), "review");
      check_str_eq(turbo_json_get_string(approve_descriptor, "input_mode"), "none");
      check_false(turbo_json_get_bool(approve_descriptor, "requires_input", true));
      check_str_eq(turbo_json_get_string(approve_descriptor, "suggested_title"),
                   "Approve Review");
      check_str_eq(turbo_json_get_string(approve_descriptor, "resume_mode"), "resume_or_fork");
      approve_args_schema = turbo_json_object_get(approve_descriptor, "args_schema");
      check_str_eq(turbo_json_get_string(approve_args_schema, "type"), "object");
      check_str_eq(turbo_json_get_string(request_replan_descriptor, "primary_key"), "reason");
      request_replan_accepted_keys =
          turbo_json_object_get(request_replan_descriptor, "accepted_keys");
      request_replan_fallback_keys =
          turbo_json_object_get(request_replan_descriptor, "fallback_keys");
      check_not_null(request_replan_accepted_keys);
      check_not_null(request_replan_fallback_keys);
      runtime_check_string_array_contains_all(request_replan_accepted_keys,
                                              request_replan_expected_accepts,
                                              sizeof(request_replan_expected_accepts) /
                                                  sizeof(request_replan_expected_accepts[0]));
      runtime_check_string_array_contains_all(request_replan_fallback_keys,
                                              request_replan_expected_fallbacks,
                                              sizeof(request_replan_expected_fallbacks) /
                                                  sizeof(request_replan_expected_fallbacks[0]));
      check_false(turbo_json_get_bool(request_replan_descriptor, "supports_json_value", true));
      runtime_check_command_descriptor_fixture(request_replan_descriptor,
                                               "command_descriptors.golden.json",
                                               "request_replan");
      check_str_eq(turbo_json_get_string(append_feedback_descriptor, "category"), "input");
      check_str_eq(turbo_json_get_string(append_feedback_descriptor, "input_mode"), "text");
      check_true(turbo_json_get_bool(append_feedback_descriptor, "requires_input", false));
      check_str_eq(turbo_json_get_string(append_feedback_descriptor, "primary_key"), "text");
      check_str_eq(turbo_json_get_string(append_feedback_descriptor, "suggested_title"),
                   "Append Feedback");
      check_str_eq(turbo_json_get_string(append_feedback_descriptor, "placeholder"),
                   "Add one short feedback message for the next run.");
      check_str_eq(turbo_json_get_string(append_feedback_descriptor, "success_state_hint"),
                   "Appends one user-visible feedback message to the canonical input.");
      append_feedback_accepted_keys =
          turbo_json_object_get(append_feedback_descriptor, "accepted_keys");
      check_not_null(append_feedback_accepted_keys);
      runtime_check_string_array_contains_all(append_feedback_accepted_keys,
                                              append_feedback_expected_accepts,
                                              sizeof(append_feedback_expected_accepts) /
                                                  sizeof(append_feedback_expected_accepts[0]));
      check_false(turbo_json_get_bool(append_feedback_descriptor, "supports_json_value", true));
      feedback_required =
          turbo_json_object_get(turbo_json_object_get(append_feedback_descriptor, "args_schema"),
                                "required");
      check_size_eq(turbo_json_array_size(feedback_required), 1);
      check_str_eq(turbo_json_string(turbo_json_array_get(feedback_required, 0)), "text");
      feedback_example_payload = turbo_json_object_get(append_feedback_descriptor, "example_payload");
      check_true(turbo_json_type(feedback_example_payload) == TURBO_JSON_OBJECT);
      check_str_eq(turbo_json_get_string(feedback_example_payload, "kind"), "append_feedback");
      check_str_eq(turbo_json_get_string(feedback_example_payload, "text"), "please add tests");
      check_str_eq(turbo_json_get_string(override_descriptor, "category"), "output");
      check_str_eq(turbo_json_get_string(override_descriptor, "input_mode"), "text_or_json");
      check_true(turbo_json_get_bool(override_descriptor, "requires_input", false));
      check_str_eq(turbo_json_get_string(override_descriptor, "primary_key"), "text");
      check_str_eq(turbo_json_get_string(override_descriptor, "suggested_title"),
                   "Override Final Output");
      check_str_eq(turbo_json_get_string(override_descriptor, "placeholder"),
                   "Replace the final output text or provide structured output_json.");
      check_str_eq(turbo_json_get_string(override_descriptor, "success_state_hint"),
                   "Overrides the current final answer with host-provided content.");
      override_accepted_keys = turbo_json_object_get(override_descriptor, "accepted_keys");
      check_not_null(override_accepted_keys);
      runtime_check_string_array_contains_all(override_accepted_keys, override_expected_accepts,
                                              sizeof(override_expected_accepts) /
                                                  sizeof(override_expected_accepts[0]));
      check_true(turbo_json_get_bool(override_descriptor, "supports_json_value", false));
      override_example_payload = turbo_json_object_get(override_descriptor, "example_payload");
      check_true(turbo_json_type(override_example_payload) == TURBO_JSON_OBJECT);
      check_str_eq(turbo_json_get_string(override_example_payload, "kind"),
                   "override_final_output");
      check_str_eq(turbo_json_get_string(override_example_payload, "text"), "Ship it.");
      override_properties =
          turbo_json_object_get(turbo_json_object_get(override_descriptor, "args_schema"),
                                "properties");
      check_str_eq(
          turbo_json_get_string(turbo_json_object_get(override_properties, "output_json"), "type"),
          "object");
      runtime_check_command_descriptor_fixture(append_feedback_descriptor,
                                               "command_descriptors.golden.json",
                                               "append_feedback");
      runtime_check_command_descriptor_fixture(override_descriptor,
                                               "command_descriptors.golden.json",
                                               "override_final_output");
    }
    first_checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
    run_id = turbo_json_get_string(summary, "run_id");
    check_not_null(first_checkpoint_id);
    check_int_eq(turbo_agent_runtime_get_checkpoint(runtime, first_checkpoint_id, &checkpoint), 0);
    check_not_null(turbo_json_object_get(checkpoint, "control_snapshot"));
    check_not_null(turbo_json_object_get(checkpoint, "workflow_snapshot"));
    check_not_null(turbo_json_object_get(checkpoint, "events"));
    turbo_free_json(&checkpoint);

    override = create_override_from_result(result_state, 1);
    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;

    options.interrupt_before_nodes = interrupt_before_end;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_resume_bind_graph(runtime, graph, first_checkpoint_id,
                                                       override, &options, &summary2,
                                                       &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary2, "status"), "interrupted");
    second_checkpoint_id = turbo_json_get_string(summary2, "checkpoint_id");
    check_not_null(second_checkpoint_id);
    check_int_eq(turbo_agent_runtime_get_checkpoint(runtime, second_checkpoint_id, &checkpoint), 0);
    check_str_eq(turbo_json_get_string(checkpoint, "parent_checkpoint_id"), first_checkpoint_id);
    check_int_eq((int)turbo_json_get_double(checkpoint, "seq", 0), 2);
    turbo_free_json(&checkpoint);

    check_int_eq(turbo_agent_runtime_load_history_events_bind(runtime, run_id, NULL, &history), 0);
    check_size_eq(turbo_runtime_data_bind_value_size(history), 6);
    turbo_runtime_data_bind_value_destroy(history);
    history = NULL;
    check_int_eq(
        turbo_agent_runtime_load_history_events_bind(runtime, NULL, second_checkpoint_id, &history),
        0);
    check_size_eq(turbo_runtime_data_bind_value_size(history), 6);
    turbo_runtime_data_bind_value_destroy(history);
    history = NULL;
    check_int_eq(turbo_agent_runtime_load_thread_history_events_bind(
                     runtime, turbo_json_get_string(summary, "thread_id"), &history),
                 0);
    check_size_eq(turbo_runtime_data_bind_value_size(history), 6);
    turbo_runtime_data_bind_value_destroy(history);
    history = NULL;

    check_int_eq(turbo_agent_runtime_get_checkpoint_context(runtime, second_checkpoint_id,
                                                            &checkpoint_context),
                 0);
    runtime_check_checkpoint_context(checkpoint_context, turbo_json_get_string(summary, "thread_id"),
                                     run_id, second_checkpoint_id, first_checkpoint_id, 1);
    turbo_free_json(&checkpoint_context);
    checkpoint_context = NULL;

    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;
    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_runtime_fork_bind_graph(runtime, graph, first_checkpoint_id, override,
                                                     &options, &summary3, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary3, "status"), "completed");
    check_true(turbo_json_is_null(turbo_json_object_get(summary3, "interrupt_reason")));
    check_true(turbo_json_is_null(turbo_json_object_get(summary3, "pending_action")));
    check_true(turbo_json_is_null(turbo_json_object_get(summary3, "pending_node")));
    check_size_eq(turbo_json_array_size(turbo_json_object_get(summary3, "available_commands")), 0);
    check_size_eq(
        turbo_json_array_size(turbo_json_object_get(summary3, "available_command_descriptors")), 0);
    check_true(strcmp(turbo_json_get_string(summary3, "run_id"), run_id) != 0);
    check_int_eq(turbo_agent_runtime_get_run(runtime, turbo_json_get_string(summary3, "run_id"),
                                             &run),
                 0);
    check_str_eq(turbo_json_get_string(run, "parent_run_id"), run_id);
    check_str_eq(turbo_json_get_string(run, "forked_from_checkpoint_id"), first_checkpoint_id);
    turbo_free_json(&run);

    check_int_eq(turbo_agent_runtime_get_run(runtime, run_id, &run), 0);
    check_str_eq(turbo_json_get_string(run, "latest_checkpoint_id"), second_checkpoint_id);
    turbo_free_json(&run);

    check_int_eq(turbo_agent_runtime_get_latest_run(
                     runtime, turbo_json_get_string(summary, "thread_id"), &latest_run),
                 0);
    check_str_eq(turbo_json_get_string(latest_run, "id"), turbo_json_get_string(summary3, "run_id"));
    turbo_free_json(&latest_run);

    check_int_eq(turbo_agent_runtime_get_pending_run(
                     runtime, turbo_json_get_string(summary, "thread_id"), &pending_run),
                 0);
    check_str_eq(turbo_json_get_string(pending_run, "id"), run_id);
    turbo_free_json(&pending_run);

    check_int_eq(turbo_agent_runtime_get_latest_checkpoint(runtime, run_id, &latest_checkpoint), 0);
    check_str_eq(turbo_json_get_string(latest_checkpoint, "id"), second_checkpoint_id);
    turbo_free_json(&latest_checkpoint);

    check_int_eq(turbo_agent_runtime_list_checkpoints(runtime, run_id, &checkpoints), 0);
    check_size_eq(turbo_json_array_size(checkpoints), 2);
    check_int_eq(turbo_agent_runtime_list_thread_lineage(runtime,
                                                         turbo_json_get_string(summary, "thread_id"),
                                                         &lineage),
                 0);
    runtime_check_thread_lineage_bind(lineage, turbo_json_get_string(summary, "thread_id"),
                                      turbo_json_get_string(summary3, "run_id"), run_id,
                                      first_checkpoint_id);
    check_true(turbo_json_array_size(turbo_json_object_get(lineage, "branches")) >= 2);
    runtime_check_lineage_branch(runtime_find_lineage_branch(
                                     turbo_json_object_get(lineage, "branches"), NULL),
                                 NULL, NULL, NULL, first_checkpoint_id);
    runtime_check_lineage_branch(runtime_find_lineage_branch(
                                     turbo_json_object_get(lineage, "branches"), run_id),
                                 run_id, first_checkpoint_id, first_checkpoint_id,
                                 first_checkpoint_id);
    check_int_eq(turbo_agent_runtime_get_branch_tree(runtime, turbo_json_get_string(summary, "thread_id"),
                                                     &branch_tree),
                 0);
    runtime_check_branch_tree(branch_tree, turbo_json_get_string(summary, "thread_id"),
                              turbo_json_get_string(summary3, "run_id"),
                              turbo_json_get_string(summary3, "run_id"), NULL, NULL, 2, 1);
    runtime_check_branch_tree_branch(
        runtime_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"), run_id),
        run_id, NULL, NULL, first_checkpoint_id);
    runtime_check_branch_tree_branch(
        runtime_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"),
                                        turbo_json_get_string(summary3, "run_id")),
        turbo_json_get_string(summary3, "run_id"), run_id, first_checkpoint_id,
        first_checkpoint_id);
    runtime_check_branch_tree_edge(
        runtime_find_branch_tree_edge(turbo_json_object_get(branch_tree, "edges"),
                                      first_checkpoint_id,
                                      turbo_json_get_string(summary3, "run_id")),
        first_checkpoint_id, turbo_json_get_string(summary3, "run_id"));

    turbo_free_json(&branch_tree);
    turbo_free_json(&lineage);
    turbo_free_json(&checkpoints);
    turbo_free_json(&summary3);
    turbo_free_json(&summary2);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(override);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should project supervisor handoff metadata onto runtime summaries") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_supervisor_review_state_bind();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    turbo_graph_run_options_t options = {0};
    const char *interrupt_before_review[] = {"review"};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL,
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_str_eq(turbo_json_get_string(summary, "active_agent"), "planner");
    check_str_eq(turbo_json_get_string(summary, "handoff_target_agent"), "executor");
    check_str_eq(turbo_json_get_string(summary, "handoff_reason"), "delegate execution");

    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should replay one checkpoint through the explicit checkpoint-scoped replay aliases") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(0);
    turbo_runtime_data_bind_value_t *override = NULL;
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *resume_summary = NULL;
    json_value_t *fork_summary = NULL;
    json_value_t *run = NULL;
    turbo_graph_run_options_t options = {0};
    const char *interrupt_before_review[] = {"review"};
    const char *checkpoint_id;
    const char *run_id;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL,
                                                      &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
    run_id = turbo_json_get_string(summary, "run_id");
    check_not_null(checkpoint_id);

    override = create_override_from_result(result_state, 1);
    check_not_null(override);
    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_runtime_resume_checkpoint_bind_graph(runtime, graph, checkpoint_id,
                                                                  override, &options,
                                                                  &resume_summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(resume_summary, "status"), "completed");
    check_str_eq(turbo_json_get_string(resume_summary, "run_id"), run_id);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(result_state, "visited_end"), 0));

    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;
    check_int_eq(turbo_agent_runtime_fork_checkpoint_bind_graph(runtime, graph, checkpoint_id,
                                                                override, &options,
                                                                &fork_summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(fork_summary, "status"), "completed");
    check_true(strcmp(turbo_json_get_string(fork_summary, "run_id"), run_id) != 0);
    check_int_eq(turbo_agent_runtime_get_run(runtime, turbo_json_get_string(fork_summary, "run_id"),
                                             &run),
                 0);
    check_str_eq(turbo_json_get_string(run, "forked_from_checkpoint_id"), checkpoint_id);

    turbo_free_json(&run);
    turbo_free_json(&fork_summary);
    turbo_free_json(&resume_summary);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(override);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should reopen one file-backed runtime and resume from stored checkpoint") {
    char *root_dir = create_runtime_temp_root();
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_file_create(root_dir);
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(0);
    turbo_runtime_data_bind_value_t *override = NULL;
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *summary2 = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    char *checkpoint_id = NULL;

    check_not_null(root_dir);
    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL,
                                                      &summary, &result_state),
                 0);
    checkpoint_id = runtime_test_strdup(turbo_json_get_string(summary, "checkpoint_id"));
    check_not_null(checkpoint_id);
    override = create_override_from_result(result_state, 1);

    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;
    turbo_free_json(&summary);
    turbo_agent_runtime_destroy(runtime);

    store = turbo_agent_runtime_store_file_create(root_dir);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_runtime_resume_bind_graph(runtime, graph, checkpoint_id, override,
                                                       &options, &summary2, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary2, "status"), "completed");
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(result_state, "visited_end"), 0));

    free(checkpoint_id);
    free(root_dir);
    turbo_free_json(&summary2);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(override);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should persist parent agent lineage and list child runs") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *run = NULL;
    json_value_t *checkpoint = NULL;
    json_value_t *runs = NULL;
    turbo_graph_run_options_t options = {0};
    const char *interrupt_before_review[] = {"review"};
    turbo_agent_runtime_parent_link_t parent_link = {"run_parent", "call_parent",
                                                     "delegate"};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph_linked(runtime, graph, state, &options,
                                                             NULL, &parent_link, &summary,
                                                             &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "parent_agent_run_id"), "run_parent");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_call_id"), "call_parent");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_name"), "delegate");
    check_int_eq(turbo_agent_runtime_get_run(runtime, turbo_json_get_string(summary, "run_id"),
                                             &run),
                 0);
    check_str_eq(turbo_json_get_string(run, "parent_agent_run_id"), "run_parent");
    check_str_eq(turbo_json_get_string(run, "parent_tool_call_id"), "call_parent");
    check_str_eq(turbo_json_get_string(run, "parent_tool_name"), "delegate");

    check_int_eq(turbo_agent_runtime_get_checkpoint(
                     runtime, turbo_json_get_string(summary, "checkpoint_id"), &checkpoint),
                 0);
    check_str_eq(turbo_json_get_string(checkpoint, "parent_agent_run_id"), "run_parent");
    check_str_eq(turbo_json_get_string(checkpoint, "parent_tool_call_id"), "call_parent");
    check_str_eq(turbo_json_get_string(checkpoint, "parent_tool_name"), "delegate");

    check_int_eq(turbo_agent_runtime_list_child_runs(runtime, "run_parent", &runs), 0);
    check_size_eq(turbo_json_array_size(runs), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs, 0), "id"),
                 turbo_json_get_string(summary, "run_id"));

    turbo_free_json(&runs);
    turbo_free_json(&checkpoint);
    turbo_free_json(&run);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should expose persisted state and apply checkpoint commands") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *thread_state = NULL;
    turbo_runtime_data_bind_value_t *run_state = NULL;
    turbo_runtime_data_bind_value_t *checkpoint_state = NULL;
    turbo_runtime_data_bind_value_t *approved_override = NULL;
    turbo_runtime_data_bind_value_t *replan_override = NULL;
    turbo_runtime_data_bind_value_t *rejected_override = NULL;
    turbo_runtime_data_bind_value_t *feedback_override = NULL;
    turbo_runtime_data_bind_value_t *final_output_override = NULL;
    turbo_runtime_data_bind_value_t *resumed_state = NULL;
    turbo_runtime_data_bind_value_t *fork_state = NULL;
    turbo_runtime_data_bind_value_t *command = NULL;
    turbo_runtime_data_bind_value_t *payload = NULL;
    json_value_t *summary = NULL;
    json_value_t *summary2 = NULL;
    json_value_t *fork_summary = NULL;
    json_value_t *override_json = NULL;
    const json_value_t *input_json;
    const json_value_t *message_json;
    const char *interrupt_before_review[] = {"review"};
    const char *checkpoint_id;
    const char *run_id;
    const char *thread_id;
    turbo_graph_run_options_t options = {0};

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL,
                                                      &summary, &result_state),
                 0);
    checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
    run_id = turbo_json_get_string(summary, "run_id");
    thread_id = turbo_json_get_string(summary, "thread_id");
    check_not_null(checkpoint_id);
    check_not_null(run_id);
    check_not_null(thread_id);

    check_int_eq(turbo_agent_runtime_get_thread_state_bind(runtime, thread_id, &thread_state), 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(thread_state, "visited_start"), 0));

    check_int_eq(turbo_agent_runtime_get_run_state_bind(runtime, run_id, &run_state), 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(run_state, "visited_start"), 0));

    check_int_eq(turbo_agent_runtime_get_checkpoint_state_bind(runtime, checkpoint_id,
                                                               &checkpoint_state),
                 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(checkpoint_state, "visited_start"), 0));

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("approve_review")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_agent_runtime_apply_thread_command_bind(runtime, thread_id, command, &approved_override),
        0);
    override_json = turbo_runtime_data_bind_value_to_json(approved_override);
    check_not_null(override_json);
    check_true(turbo_agent_state_review_approved(override_json));
    turbo_free_json(&override_json);
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("approve_review")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_runtime_resume_thread_command_bind(runtime, graph, thread_id, command,
                                                                &options, &summary2, &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(summary2, "status"), "completed");
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;
    turbo_runtime_data_bind_value_destroy(run_state);
    run_state = NULL;
    check_int_eq(turbo_agent_runtime_get_run_state_bind(runtime, run_id, &run_state), 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(run_state, "visited_end"), 0));

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("request_replan")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "reason",
                     turbo_runtime_data_bind_value_create_string("manual review requested")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_agent_runtime_apply_thread_command_bind(runtime, thread_id, command, &replan_override),
        0);
    override_json = turbo_runtime_data_bind_value_to_json(replan_override);
    check_not_null(override_json);
    check_true(turbo_agent_state_replan_requested(override_json));
    check_str_eq(turbo_agent_state_replan_reason(override_json), "manual review requested");
    turbo_free_json(&override_json);
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("reject_review")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "note",
                     turbo_runtime_data_bind_value_create_string("needs another pass")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_agent_runtime_apply_checkpoint_command_bind(runtime, checkpoint_id, command,
                                                                   &rejected_override),
                 0);
    override_json = turbo_runtime_data_bind_value_to_json(rejected_override);
    check_not_null(override_json);
    check_true(turbo_agent_state_review_required(override_json));
    check_false(turbo_agent_state_review_approved(override_json));
    check_str_eq(turbo_agent_state_review_note(override_json), "needs another pass");
    turbo_free_json(&override_json);
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("append_feedback")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "text",
                     turbo_runtime_data_bind_value_create_string("please add tests")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_agent_runtime_apply_thread_command_bind(runtime, thread_id, command, &feedback_override),
        0);
    override_json = turbo_runtime_data_bind_value_to_json(feedback_override);
    check_not_null(override_json);
    input_json = turbo_json_object_get(override_json, "input");
    check_not_null(input_json);
    check_true(turbo_json_array_size(input_json) >= 1);
    message_json = turbo_json_array_get(input_json, turbo_json_array_size(input_json) - 1);
    check_not_null(message_json);
    check_str_eq(turbo_json_get_string(message_json, "role"), "user");
    check_str_eq(turbo_json_get_string(message_json, "content"), "please add tests");
    turbo_free_json(&override_json);
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;

    command = turbo_runtime_data_bind_value_create_object();
    payload = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_not_null(payload);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     payload, "ok", turbo_runtime_data_bind_value_create_bool(1)),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     payload, "value", turbo_runtime_data_bind_value_create_int64(9)),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("override_final_output")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(command, "output_json", payload),
                 TURBO_RUNTIME_DATA_BIND_OK);
    payload = NULL;
    check_int_eq(turbo_agent_runtime_apply_command_bind(runtime, checkpoint_id, command,
                                                        &final_output_override),
                 0);
    override_json = turbo_runtime_data_bind_value_to_json(final_output_override);
    check_not_null(override_json);
    check_str_eq(turbo_agent_state_final_answer_text(override_json), "{\"ok\":true,\"value\":9}");

    turbo_free_json(&override_json);
    override_json = NULL;
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;
    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("approve_review")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_agent_runtime_fork_checkpoint_command_bind(runtime, graph, checkpoint_id,
                                                                  command, NULL, &fork_summary,
                                                                  &fork_state),
                 0);
    check_str_eq(turbo_json_get_string(fork_summary, "status"), "completed");
    check_false(strcmp(turbo_json_get_string(fork_summary, "run_id"), run_id) == 0);

    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;
    turbo_runtime_data_bind_value_destroy(payload);
    turbo_runtime_data_bind_value_destroy(fork_state);
    turbo_runtime_data_bind_value_destroy(final_output_override);
    turbo_runtime_data_bind_value_destroy(feedback_override);
    turbo_runtime_data_bind_value_destroy(rejected_override);
    turbo_runtime_data_bind_value_destroy(resumed_state);
    turbo_runtime_data_bind_value_destroy(replan_override);
    turbo_runtime_data_bind_value_destroy(approved_override);
    turbo_runtime_data_bind_value_destroy(checkpoint_state);
    turbo_runtime_data_bind_value_destroy(run_state);
    turbo_runtime_data_bind_value_destroy(thread_state);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_free_json(&fork_summary);
    turbo_free_json(&summary2);
    turbo_free_json(&summary);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should fail loudly when file-backed checkpoint records contain invalid json") {
    char *root_dir = create_runtime_temp_root();
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_file_create(root_dir);
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_runtime_review_graph();
    turbo_runtime_data_bind_value_t *state = create_review_state_bind(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *checkpoints = NULL;
    json_value_t *checkpoint = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    char *checkpoint_id = NULL;
    char *run_id = NULL;
    char *checkpoint_filename = NULL;

    check_not_null(root_dir);
    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_runtime_start_bind_graph(runtime, graph, state, &options, NULL,
                                                      &summary, &result_state),
                 0);
    checkpoint_id = runtime_test_strdup(turbo_json_get_string(summary, "checkpoint_id"));
    check_not_null(checkpoint_id);
    check_int_eq(turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint), 0);
    run_id = runtime_test_strdup(turbo_json_get_string(checkpoint, "run_id"));
    check_not_null(run_id);
    turbo_free_json(&checkpoint);
    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;
    turbo_free_json(&summary);
    turbo_agent_runtime_destroy(runtime);

    checkpoint_filename = runtime_test_record_file(root_dir, "checkpoints", checkpoint_id);
    runtime_test_write_text(checkpoint_filename, "{not valid json");
    free(checkpoint_filename);

    store = turbo_agent_runtime_store_file_create(root_dir);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    check_int_eq(turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint), -1);
    check_int_eq(turbo_agent_runtime_list_checkpoints(runtime, "missing-run", &checkpoints), -1);
    check_int_eq(turbo_agent_runtime_list_checkpoints(runtime, run_id, &checkpoints), -1);
    check_int_eq(
        turbo_agent_runtime_resume_bind_graph(runtime, graph, checkpoint_id, NULL, NULL, &summary,
                                             &result_state),
        -1);

    free(checkpoint_id);
    free(run_id);
    free(root_dir);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }
}
