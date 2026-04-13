#include "tinytest.h"
#include "turbo_openai_agent_workflow.h"

spec("turbo openai agent workflow api") {

  it("should expose workflow installers through the workflow header") {
    void *planner_loop = (void *)turbo_openai_agent_install_planner_loop;
    void *basic_loop = (void *)turbo_openai_agent_install_loop;
    void *review_loop = (void *)turbo_openai_agent_install_review_loop;
    void *replan_loop = (void *)turbo_openai_agent_install_replan_loop;
    void *review_replan_loop = (void *)turbo_openai_agent_install_review_replan_loop;
    void *engineering_loop = (void *)turbo_openai_agent_install_engineering_loop;

    check_not_null(planner_loop);
    check_not_null(basic_loop);
    check_not_null(review_loop);
    check_not_null(replan_loop);
    check_not_null(review_replan_loop);
    check_not_null(engineering_loop);
  }
}
