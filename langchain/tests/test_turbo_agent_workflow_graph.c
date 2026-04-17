#include "tinytest.h"
#include "turbo_agent.h"
#include "turbo_agent_workflow.h"

static int dummy_transport(const char *request_json, char **out_response_json, void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static int dummy_detect_replan(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)ctx;
  (void)user_data;
  return 0;
}

static turbo_agent_t *create_test_agent(const char *model) {
  turbo_agent_config_t config = {0};

  config.model = model;
  config.transport_fn = dummy_transport;
  return turbo_agent_create(&config);
}

spec("turbo agent workflow graph") {

  it("should install planner loop graph structure") {
    turbo_graph_t *graph = turbo_graph_create("planner-loop");
    turbo_agent_t *planner = create_test_agent("gpt-5.4");
    turbo_agent_t *executor = create_test_agent("gpt-5.4");

    check_not_null(graph);
    check_not_null(planner);
    check_not_null(executor);
    check_int_eq(
        turbo_agent_install_planner_loop(graph, planner, executor, "planner", "commit", "step",
                                         "executor", "tools", "advance", "end", 1),
        TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "planner");
    check_size_eq(turbo_graph_node_count(graph), 7);
    check_size_eq(turbo_graph_edge_count(graph), 8);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_agent_destroy(executor);
    turbo_agent_destroy(planner);
    turbo_graph_destroy(graph);
  }

  it("should install review loop graph structure") {
    turbo_graph_t *graph = turbo_graph_create("review-loop");
    turbo_agent_t *planner = create_test_agent("gpt-5.4");
    turbo_agent_t *executor = create_test_agent("gpt-5.4");

    check_not_null(graph);
    check_not_null(planner);
    check_not_null(executor);
    check_int_eq(
        turbo_agent_install_review_loop(graph, planner, executor, "planner", "commit", "step",
                                        "review", "executor", "tools", "advance", "end", 1),
        TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "planner");
    check_size_eq(turbo_graph_node_count(graph), 8);
    check_size_eq(turbo_graph_edge_count(graph), 10);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_agent_destroy(executor);
    turbo_agent_destroy(planner);
    turbo_graph_destroy(graph);
  }

  it("should install review replan loop graph structure") {
    turbo_graph_t *graph = turbo_graph_create("review-replan-loop");
    turbo_agent_t *planner = create_test_agent("gpt-5.4");
    turbo_agent_t *executor = create_test_agent("gpt-5.4");

    check_not_null(graph);
    check_not_null(planner);
    check_not_null(executor);
    check_int_eq(
        turbo_agent_install_review_replan_loop(
            graph, planner, executor, "planner", "commit", "step", "review", "executor",
            "tools", "detect", dummy_detect_replan, NULL, "route", "prepare", "advance", "end",
            1),
        TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "planner");
    check_size_eq(turbo_graph_node_count(graph), 11);
    check_size_eq(turbo_graph_edge_count(graph), 13);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_agent_destroy(executor);
    turbo_agent_destroy(planner);
    turbo_graph_destroy(graph);
  }

  it("should install supervisor loop graph structure") {
    turbo_graph_t *graph = turbo_graph_create("supervisor-loop");

    check_not_null(graph);
    check_int_eq(
        turbo_agent_install_supervisor_loop(graph, "supervisor", "handoff", "end", 1),
        TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "supervisor");
    check_size_eq(turbo_graph_node_count(graph), 3);
    check_size_eq(turbo_graph_edge_count(graph), 2);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_graph_destroy(graph);
  }
}
