#include "turbo_agent_policy.h"

#include <ctype.h>
#include <string.h>

static int turbo_policy_string_prefix_eq(const char *lhs, const char *rhs, size_t n) {
  size_t i;
  unsigned char lc;
  unsigned char rc;

  for (i = 0; i < n; ++i) {
    lc = (unsigned char)lhs[i];
    rc = (unsigned char)rhs[i];

#ifdef _WIN32
    if (lc == '\\') {
      lc = '/';
    }
    if (rc == '\\') {
      rc = '/';
    }

    if (tolower(lc) != tolower(rc)) {
#else
    if (lc != rc) {
#endif
      return 0;
    }
  }

  return 1;
}

static const char *turbo_policy_get_path_arg(const json_value_t *args) {
  if (!args || turbo_json_type(args) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  if (turbo_json_get_string(args, "path")) {
    return turbo_json_get_string(args, "path");
  }

  return turbo_json_get_string(args, "workdir");
}

turbo_agent_policy_t turbo_agent_policy_default(void) {
  turbo_agent_policy_t policy;

  memset(&policy, 0, sizeof(policy));
  policy.auto_approve_safe = 1;
  policy.allow_shell = 1;
  policy.allow_patch = 1;
  policy.max_action_retries = 1;
  policy.max_steps = 32;
  policy.max_replans = 3;
  return policy;
}

int turbo_agent_policy_validate_path(const turbo_agent_policy_t *policy, const char *path) {
  size_t root_len;

  if (!path || path[0] == '\0') {
    return -1;
  }

  if (!policy || !policy->workspace_root || policy->allow_outside_workspace) {
    return 0;
  }

  root_len = strlen(policy->workspace_root);
  if (root_len == 0) {
    return 0;
  }

  if (strlen(path) < root_len) {
    return -1;
  }

  if (!turbo_policy_string_prefix_eq(path, policy->workspace_root, root_len)) {
    return -1;
  }

  if (path[root_len] == '\0' || path[root_len] == '\\' || path[root_len] == '/') {
    return 0;
  }

  return -1;
}

int turbo_agent_policy_is_dangerous_command(const char *command) {
  static const char *patterns[] = {
      "rm -rf", "del /s", "format", "git reset --hard", "git clean -fd", NULL};
  size_t i;

  if (!command) {
    return 0;
  }

  for (i = 0; patterns[i]; ++i) {
    if (strstr(command, patterns[i]) != NULL) {
      return 1;
    }
  }

  return 0;
}

turbo_agent_policy_decision_t
turbo_agent_policy_check_action(const turbo_agent_policy_t *policy,
                                const turbo_action_tool_definition_t *definition,
                                const json_value_t *args, const char **out_reason) {
  const char *path;
  const char *command;

  if (out_reason) {
    *out_reason = NULL;
  }

  if (!definition) {
    if (out_reason) {
      *out_reason = "missing_definition";
    }
    return TURBO_AGENT_POLICY_DENY;
  }

  path = turbo_policy_get_path_arg(args);
  if (path && turbo_agent_policy_validate_path(policy, path) != 0) {
    if (out_reason) {
      *out_reason = "path_outside_workspace";
    }
    return TURBO_AGENT_POLICY_DENY;
  }

  if (definition->kind == TURBO_ACTION_MUTATE && policy && !policy->allow_patch) {
    if (out_reason) {
      *out_reason = "patch_disabled";
    }
    return TURBO_AGENT_POLICY_DENY;
  }

  if (definition->kind == TURBO_ACTION_DANGEROUS && policy && !policy->allow_shell) {
    if (out_reason) {
      *out_reason = "shell_disabled";
    }
    return TURBO_AGENT_POLICY_DENY;
  }

  command = args && turbo_json_type(args) == TURBO_JSON_OBJECT
                ? turbo_json_get_string(args, "command")
                : NULL;
  if (command && turbo_agent_policy_is_dangerous_command(command)) {
    if (out_reason) {
      *out_reason = "dangerous_command";
    }
    return TURBO_AGENT_POLICY_REQUIRE_APPROVAL;
  }

  if (definition->requires_approval) {
    if (out_reason) {
      *out_reason = "tool_requires_approval";
    }
    return TURBO_AGENT_POLICY_REQUIRE_APPROVAL;
  }

  if (policy && policy->auto_approve_safe && definition->kind == TURBO_ACTION_OBSERVE) {
    return TURBO_AGENT_POLICY_ALLOW;
  }

  if (definition->kind == TURBO_ACTION_DANGEROUS) {
    if (out_reason) {
      *out_reason = "dangerous_action";
    }
    return TURBO_AGENT_POLICY_REQUIRE_APPROVAL;
  }

  return TURBO_AGENT_POLICY_ALLOW;
}

int turbo_agent_policy_requires_approval(const turbo_agent_policy_t *policy,
                                         const turbo_action_tool_definition_t *definition,
                                         const json_value_t *args) {
  return turbo_agent_policy_check_action(policy, definition, args, NULL) ==
                 TURBO_AGENT_POLICY_REQUIRE_APPROVAL
             ? 1
             : 0;
}
