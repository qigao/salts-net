#include "tinytest.h"
#include "turbo_openai_agent_graph.h"

spec("turbo openai agent graph api") {

  it("should expose workflow helpers through the graph-specific header") {
    turbo_graph_node_fn model_node = turbo_openai_agent_model_node;
    turbo_graph_node_fn tool_node = turbo_openai_agent_tool_node;
    turbo_graph_edge_predicate_fn has_tools = turbo_openai_agent_has_pending_tool_calls;
    turbo_graph_edge_predicate_fn review_ok = turbo_openai_agent_review_approved_predicate;

    check_not_null((void *)model_node);
    check_not_null((void *)tool_node);
    check_not_null((void *)has_tools);
    check_not_null((void *)review_ok);
  }
}
