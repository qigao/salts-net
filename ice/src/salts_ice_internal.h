#ifndef SALTSNET_ICE_INTERNAL_H
#define SALTSNET_ICE_INTERNAL_H

#include "ice/salts_ice.h"
#include "ice/salts_stun.h"
#include "ice/salts_turn.h"

#define ICE_CONSENT_TRANSACTION_CAPACITY 10

typedef struct {
  void *socket;
  salts_turn_client_t *turn_client;
  int io_active;
} ice_candidate_runtime_t;

typedef struct {
  ice_candidate_t *local;
  ice_candidate_t *remote;
  int received_nomination;
} ice_triggered_check_t;

struct salts_ice_agent_s {
  ice_config_t config;
  void *progress_owner_reserved;
  /* One unpublished gathering socket, owned until commit or successful cleanup.
   * A failed cleanup prevents another allocation; candidate slots own committed
   * sockets. Neither handle is discarded by a failed destroy. */
  void *gathering_socket;

  ice_state_t state;
  ice_gathering_state_t gathering_state;
  ice_role_t role;
  uint64_t tie_breaker;

  char local_ufrag[32];
  char local_pwd[64];
  char remote_ufrag[32];
  char remote_pwd[64];

  ice_candidate_t local_candidates[ICE_MAX_CANDIDATES];
  ice_candidate_runtime_t local_candidate_runtime[ICE_MAX_CANDIDATES];
  int local_candidate_count;
  ice_candidate_t remote_candidates[ICE_MAX_CANDIDATES];
  int remote_candidate_count;

  ice_candidate_pair_t pairs[ICE_MAX_CANDIDATE_PAIRS];
  int pair_count;
  ice_candidate_pair_t *selected_pair;

  int current_check_pair;
  stun_transaction_id_t current_txn_id;
  int checks_in_progress;
  int valid_pairs_count;
  uint64_t check_start_time;

  int pending_stun_requests;
  int pending_turn_requests;
  salts_turn_client_t *turn_clients[ICE_MAX_TURN_SERVERS];

  int foundation_counter;
  ice_callbacks_t callbacks;

  int remote_credentials_set;
  int remote_candidates_complete;
  int nomination_started;
  int selected_pair_io_running;
  int current_check_nominating;
  int current_check_select_on_success;
  stun_transaction_id_t consent_txn_ids[ICE_CONSENT_TRANSACTION_CAPACITY];
  uint64_t consent_txn_sent_ms[ICE_CONSENT_TRANSACTION_CAPACITY];
  size_t consent_txn_next;
  uint64_t last_consent_response_ms;
  uint64_t next_consent_check_ms;
  ice_triggered_check_t triggered_checks[ICE_MAX_CANDIDATE_PAIRS];
  int triggered_check_head;
  int triggered_check_count;
  uint64_t last_keepalive_ms;
};

#endif
