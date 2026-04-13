#include "tinytest.h"
#include "turbo_agent_policy.h"

spec("turbo agent policy") {

  it("should allow read action inside workspace") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "read_file",
        .description = "Read",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "C:\\workspace\\src\\main.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, NULL),
                 TURBO_AGENT_POLICY_ALLOW);

    turbo_free_json(&args);
  }

  it("should allow slash-normalized paths inside workspace") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "read_file",
        .description = "Read",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "C:/workspace/src/main.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, NULL),
                 TURBO_AGENT_POLICY_ALLOW);

    turbo_free_json(&args);
  }

  it("should reject patch outside workspace") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "apply_patch",
        .description = "Patch",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_MUTATE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    policy.workspace_root = "C:\\workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "D:\\other\\file.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_DENY);
    check_str_eq(reason, "path_outside_workspace");

    turbo_free_json(&args);
  }

#ifndef _WIN32
  it("should treat workspace paths as case-sensitive on non-Windows hosts") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "read_file",
        .description = "Read",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_OBSERVE,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    policy.workspace_root = "/workspace";
    check_not_null(args);
    turbo_json_object_set_string(args, "path", "/Workspace/src/main.c");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_DENY);
    check_str_eq(reason, "path_outside_workspace");

    turbo_free_json(&args);
  }
#endif

  it("should require approval for dangerous shell command") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "run_command",
        .description = "Run",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_DANGEROUS,
        .handler = (turbo_action_tool_handler_fn)1,
    };
    json_value_t *args = turbo_json_create_object();
    const char *reason = NULL;

    check_not_null(args);
    turbo_json_object_set_string(args, "command", "git reset --hard HEAD");
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, args, &reason),
                 TURBO_AGENT_POLICY_REQUIRE_APPROVAL);
    check_str_eq(reason, "dangerous_command");
    check_true(turbo_agent_policy_requires_approval(&policy, &definition, args));

    turbo_free_json(&args);
  }

  it("should deny shell actions when disabled") {
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_action_tool_definition_t definition = {
        .name = "run_command",
        .description = "Run",
        .parameters_json = "{\"type\":\"object\"}",
        .kind = TURBO_ACTION_DANGEROUS,
        .handler = (turbo_action_tool_handler_fn)1,
    };

    policy.allow_shell = 0;
    check_int_eq(turbo_agent_policy_check_action(&policy, &definition, NULL, NULL),
                 TURBO_AGENT_POLICY_DENY);
  }
}
