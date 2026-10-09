#include <cnet/client_pool.h>
#include <cnet/destination_policy.h>
#include <cnet/managed_dial.h>
#include <cnet/owner_placement.h>
#include <cnet/recovery_policy.h>
#include <cnet/sg_host.h>
#include <salts/error_codes.h>

#include <stdint.h>
#include <stdio.h>

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
      fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expr);      \
      return 1;                                                              \
    }                                                                        \
  } while (0)

int main(void) {
  cnet_destination_hint endpoints[] = {
      {.endpoint_id = 101u, .weight = 1u, .eligible = true},
      {.endpoint_id = 202u, .weight = 2u, .eligible = true}};
  cnet_destination_selection selection = {
      .size = sizeof(selection),
      .version = CNET_DESTINATION_POLICY_VERSION,
      .kind = CNET_DESTINATION_EXPLICIT,
      .endpoints = endpoints,
      .endpoint_count = 2u,
      .snapshot_generation = 1u,
      .expires_at_ms = UINT64_MAX,
      .now_ms = 5u,
      .explicit_endpoint_id = 202u};
  cnet_destination_result selected = {0};

  CHECK(cnet_destination_validate(endpoints, 2u) == SALTS_OK);
  CHECK(cnet_destination_choose(&selection, &selected) == SALTS_OK);
  CHECK(selected.endpoint_id == 202u);
  endpoints[1].eligible = false;
  CHECK(cnet_destination_choose(&selection, &selected) != SALTS_OK);
  endpoints[1].eligible = true;

  cnet_owner_placement_hint owners[] = {
      {.eligible = true, .pressure = 4u},
      {.eligible = true, .pressure = 1u}};
  cnet_owner_placement_input placement = {
      .size = sizeof(placement),
      .version = CNET_OWNER_PLACEMENT_VERSION,
      .kind = CNET_OWNER_PLACE_EXPLICIT,
      .owners = owners,
      .owner_count = 2u,
      .explicit_owner = 1u};
  size_t owner = SIZE_MAX;
  CHECK(cnet_owner_placement_choose(&placement, &owner) == SALTS_OK);
  CHECK(owner == 1u);
  owners[1].eligible = false;
  CHECK(cnet_owner_placement_choose(&placement, &owner) != SALTS_OK);

  cnet_retry_input retry = {
      .size = sizeof(retry),
      .version = CNET_RECOVERY_POLICY_VERSION,
      .attempts_used = 1u,
      .max_attempts = 2u,
      .now_ms = 10u,
      .deadline_ms = 100u,
      .remaining_retry_byte_budget = 1024u,
      .owned_replayable_body = true,
      .application_declares_idempotent = true};
  cnet_retry_result decision = {0};
  CHECK(cnet_retry_evaluate(&retry, &decision) == SALTS_OK);
  CHECK(!decision.allowed && decision.reason == CNET_RETRY_DISABLED);
  retry.explicitly_enabled = true;
  retry.one_attempt_contract = true;
  CHECK(cnet_retry_evaluate(&retry, &decision) == SALTS_OK);
  CHECK(!decision.allowed && decision.reason == CNET_RETRY_ONE_ATTEMPT);
  retry.one_attempt_contract = false;
  CHECK(cnet_retry_evaluate(&retry, &decision) == SALTS_OK);
  CHECK(decision.allowed && decision.reason == CNET_RETRY_ALLOWED);
  retry.cancelled = true;
  CHECK(cnet_retry_evaluate(&retry, &decision) == SALTS_OK);
  CHECK(!decision.allowed && decision.reason == CNET_RETRY_CANCELLED);
  return 0;
}
