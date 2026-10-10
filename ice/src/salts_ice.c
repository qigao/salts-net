/**
 * salts_ice.c - ICE Agent Implementation (RFC 8445)
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#define NORPC
#define NOSERVICE
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "ice/salts_ice.h"
#include "ice/salts_stun.h"
#include "ice/salts_turn.h"
#include "ice_cnet_datagram.h"
#include "salts_ice_internal.h"
#include "stun_binding_transaction.h"
#include <platform.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include "tstr.h"
#include "tlog.h"
#include <fmt.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <iphlpapi.h>
  #include <process.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "iphlpapi.lib")
#else
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <netdb.h>
  #include <net/if.h>
  #include <netinet/in.h>
  #include <strings.h>
  #include <unistd.h>
  #include <pthread.h>
#endif

#define ICE_CONSENT_MIN_BASE_INTERVAL_MS 5000
#define ICE_CONSENT_MAX_BASE_INTERVAL_MS 20000
#define ICE_CONSENT_EXPIRY_MS 30000

static void ice_tracef(const char *fmt, ...) {
  const char *path = getenv("SALTSNET_ICE_TRACE");
  FILE *fp;
  va_list args;

  if (!path || path[0] == '\0') {
    return;
  }

  fp = fopen(path, "a");
  if (!fp) {
    return;
  }

  va_start(args, fmt);
  vfprintf(fp, fmt, args);
  va_end(args);
  fputc('\n', fp);
  fclose(fp);
}

static int generate_random_u64(uint64_t *value) {
  if (!value)
    return -1;
  return cmeta_secure_random(value, sizeof(*value));
}

static int resolve_mdns_hostname(ice_candidate_t *candidate) {
  struct addrinfo hints;
  struct addrinfo *results = NULL;
  struct addrinfo *it = NULL;
  int rc;

  if (!candidate || candidate->mdns_name[0] == '\0' || candidate->ip[0] != '\0') {
    return 0;
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = candidate->family == AF_INET6 ? AF_INET6 : AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  rc = getaddrinfo(candidate->mdns_name, NULL, &hints, &results);
  if (rc != 0 || !results) {
#ifdef _WIN32
    TLOG_WARNF("Failed to resolve mDNS candidate {}: {}", candidate->mdns_name, rc);
#else
    TLOG_WARNF("Failed to resolve mDNS candidate {}: {}", candidate->mdns_name,
              gai_strerror(rc));
#endif
    if (results) {
      freeaddrinfo(results);
    }
    return -1;
  }

  for (it = results; it; it = it->ai_next) {
    void *addr_ptr = NULL;
    int family = it->ai_family;

    if (family == AF_INET) {
      addr_ptr = &((struct sockaddr_in *)it->ai_addr)->sin_addr;
    } else if (family == AF_INET6) {
      addr_ptr = &((struct sockaddr_in6 *)it->ai_addr)->sin6_addr;
    } else {
      continue;
    }

    if (inet_ntop(family, addr_ptr, candidate->ip, sizeof(candidate->ip))) {
      candidate->family = family;
      break;
    }
  }

  freeaddrinfo(results);

  if (candidate->ip[0] == '\0') {
    TLOG_WARNF("mDNS candidate {} resolved without a usable IP address", candidate->mdns_name);
    return -1;
  }

  TLOG_INFOF("Resolved mDNS candidate {} -> {}", candidate->mdns_name, candidate->ip);
  return 0;
}

static inline uint16_t read_u16_be(const uint8_t *ptr) {
  return (uint16_t)((ptr[0] << 8) | ptr[1]);
}

/* ============================================================================
 * Internal Structures
 * ============================================================================ */


typedef int (*ice_wait_wake_fn)(void *target);

typedef struct {
  atomic_int close_requested;
  cmeta_mutex_t mutex;
  int owner_active;
  unsigned int owner_depth;
#if defined(_WIN32)
  DWORD owner_thread;
#else
  pthread_t owner_thread;
#endif
  void *wait_target;
  ice_wait_wake_fn wait_wake;
} ice_progress_owner_t;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static int generate_random_string(char *buf, size_t len) {
  static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const size_t charset_len = sizeof(charset) - 1;
  const uint8_t uniform_limit = (uint8_t)(256u - (256u % charset_len));
  size_t i = 0;

  if (!buf || len == 0)
    return -1;

  while (i + 1 < len) {
    uint8_t random_byte;
    if (cmeta_secure_random(&random_byte, sizeof(random_byte)) != 0)
      return -1;
    if (random_byte >= uniform_limit)
      continue;
    buf[i++] = charset[random_byte % charset_len];
  }
  buf[len - 1] = '\0';
  return 0;
}

static int initialize_candidate_identity(salts_ice_agent_t *agent, ice_candidate_t *candidate) {
  if (!agent || !candidate)
    return -1;

  snprintf(candidate->foundation, sizeof(candidate->foundation), "%d",
           ++agent->foundation_counter);
  return 0;
}

static void rebuild_candidate_pairs(salts_ice_agent_t *agent);
static int ice_candidates_equivalent(const ice_candidate_t *a, const ice_candidate_t *b);
static int ice_agent_quiesce_transports(salts_ice_agent_t *agent, uint32_t timeout_ms);
static int ice_agent_release(salts_ice_agent_t *agent, uint32_t timeout_ms);

static void ice_agent_finish_close(salts_ice_agent_t *agent);

static ice_candidate_runtime_t *ice_candidate_runtime(
    salts_ice_agent_t *agent, const ice_candidate_t *candidate) {
  if (!agent || !candidate) return NULL;
  for (int i = 0; i < ICE_MAX_CANDIDATES; ++i) {
    if (candidate == &agent->local_candidates[i]) {
      return &agent->local_candidate_runtime[i];
    }
  }
  return NULL;
}

static ice_progress_owner_t *ice_agent_progress(const salts_ice_agent_t *agent) {
  return agent ? (ice_progress_owner_t *)agent->progress_owner_reserved : NULL;
}

static int ice_progress_current_thread_is_owner(const ice_progress_owner_t *progress) {
  if (!progress || !progress->owner_active) return 0;
#if defined(_WIN32)
  return progress->owner_thread == GetCurrentThreadId();
#else
  return pthread_equal(progress->owner_thread, pthread_self()) != 0;
#endif
}

static void ice_progress_set_current_thread_owner(ice_progress_owner_t *progress) {
  progress->owner_active = 1;
  progress->owner_depth = 1u;
#if defined(_WIN32)
  progress->owner_thread = GetCurrentThreadId();
#else
  progress->owner_thread = pthread_self();
#endif
}

static void ice_agent_owner_enter(salts_ice_agent_t *agent) {
  ice_progress_owner_t *progress = ice_agent_progress(agent);
  if (!progress) return;
  for (;;) {
    cmeta_mutex_lock(&progress->mutex);
    if (!progress->owner_active) {
      ice_progress_set_current_thread_owner(progress);
      cmeta_mutex_unlock(&progress->mutex);
      return;
    }
    if (ice_progress_current_thread_is_owner(progress)) {
      progress->owner_depth++;
      cmeta_mutex_unlock(&progress->mutex);
      return;
    }
    cmeta_mutex_unlock(&progress->mutex);
    cmeta_sleep_ms(1u);
  }
}

static void ice_agent_owner_leave(salts_ice_agent_t *agent) {
  ice_progress_owner_t *progress = ice_agent_progress(agent);
  int finish_close;
  if (!progress) return;
  cmeta_mutex_lock(&progress->mutex);
  if (!ice_progress_current_thread_is_owner(progress)) {
    cmeta_mutex_unlock(&progress->mutex);
    return;
  }
  if (progress->owner_depth > 1u) {
    progress->owner_depth--;
    cmeta_mutex_unlock(&progress->mutex);
    return;
  }
  progress->wait_target = NULL;
  progress->wait_wake = NULL;
  finish_close = atomic_load_explicit(&progress->close_requested,
                                      memory_order_acquire);
  if (!finish_close) {
    progress->owner_active = 0;
    progress->owner_depth = 0u;
  }
  cmeta_mutex_unlock(&progress->mutex);
  if (finish_close) {
    ice_agent_finish_close(agent);
    cmeta_mutex_lock(&progress->mutex);
    progress->owner_active = 0;
    progress->owner_depth = 0u;
    cmeta_mutex_unlock(&progress->mutex);
  }
}

static int ice_agent_wait_begin(salts_ice_agent_t *agent, void *target,
                                ice_wait_wake_fn wake) {
  ice_progress_owner_t *progress = ice_agent_progress(agent);
  if (!progress || !target || !wake) return -1;
  cmeta_mutex_lock(&progress->mutex);
  if (atomic_load_explicit(&progress->close_requested, memory_order_acquire)) {
    cmeta_mutex_unlock(&progress->mutex);
    return -1;
  }
  progress->wait_target = target;
  progress->wait_wake = wake;
  cmeta_mutex_unlock(&progress->mutex);
  return 0;
}

static void ice_agent_wait_end(salts_ice_agent_t *agent, void *target) {
  ice_progress_owner_t *progress = ice_agent_progress(agent);
  if (!progress) return;
  cmeta_mutex_lock(&progress->mutex);
  if (progress->wait_target == target) {
    progress->wait_target = NULL;
    progress->wait_wake = NULL;
  }
  cmeta_mutex_unlock(&progress->mutex);
}

static int ice_wake_datagram(void *target) {
  return ice_cnet_datagram_wake((ice_cnet_datagram_t *)target);
}

static int ice_wake_turn(void *target) {
  return turn_client_wake((salts_turn_client_t *)target);
}

static int ice_agent_is_closed(const salts_ice_agent_t *agent) {
  ice_progress_owner_t *progress = ice_agent_progress(agent);
  if (!agent) return 0;
  if (agent->state == ICE_STATE_CLOSED) return 1;
  if (!progress ||
      !atomic_load_explicit(&progress->close_requested, memory_order_acquire)) {
    return 0;
  }
  cmeta_mutex_lock(&progress->mutex);
  const int is_owner = ice_progress_current_thread_is_owner(progress);
  cmeta_mutex_unlock(&progress->mutex);
  if (is_owner) ice_agent_finish_close((salts_ice_agent_t *)agent);
  return 1;
}

static void set_state(salts_ice_agent_t *agent, ice_state_t new_state) {
  if (!agent || (agent->state == ICE_STATE_CLOSED && new_state != ICE_STATE_CLOSED))
    return;
  if (agent->state != new_state) {
    ice_state_t old_state = agent->state;
    agent->state = new_state;

    if ((new_state == ICE_STATE_CONNECTED || new_state == ICE_STATE_COMPLETED) &&
        old_state != ICE_STATE_CONNECTED && old_state != ICE_STATE_COMPLETED) {
      uint64_t now = cmeta_monotonic_ms();
      memset(agent->consent_txn_ids, 0, sizeof(agent->consent_txn_ids));
      memset(agent->consent_txn_sent_ms, 0, sizeof(agent->consent_txn_sent_ms));
      agent->consent_txn_next = 0;
      agent->last_consent_response_ms = now;
      agent->next_consent_check_ms = 0;
    }

    if (agent->callbacks.on_state_change) {
      TLOG_INFOF("State change: {} -> {}", ice_state_name(old_state), ice_state_name(new_state));
      agent->callbacks.on_state_change(agent, old_state, new_state, agent->callbacks.user_data);
    }
  }
}

static void ice_agent_finish_close(salts_ice_agent_t *agent) {
  if (!agent || agent->state == ICE_STATE_CLOSED) return;
  agent->checks_in_progress = 0;
  agent->current_check_pair = -1;
  set_state(agent, ICE_STATE_CLOSED);
}

static void set_gathering_state(salts_ice_agent_t *agent, ice_gathering_state_t new_state) {
  if (!agent || ice_agent_is_closed(agent))
    return;
  if (agent->gathering_state != new_state) {
    agent->gathering_state = new_state;
    if (agent->callbacks.on_gathering_change) {
      agent->callbacks.on_gathering_change(agent, new_state, agent->callbacks.user_data);
    }
  }
}

/* ============================================================================
 * Candidate Priority Calculation (RFC 8445 Section 5.1.2)
 * ============================================================================ */

uint32_t ice_calculate_priority(ice_candidate_type_t type, int local_pref, int component_id) {
  int type_pref;

  switch (type) {
  case ICE_CANDIDATE_TYPE_HOST:
    type_pref = 126;
    break;
  case ICE_CANDIDATE_TYPE_PRFLX:
    type_pref = 110;
    break;
  case ICE_CANDIDATE_TYPE_SRFLX:
    type_pref = 100;
    break;
  case ICE_CANDIDATE_TYPE_RELAY:
    type_pref = 0;
    break;
  default:
    type_pref = 0;
  }

  /* priority = (2^24) * type_preference + (2^8) * local_preference + (256 - component_id) */
  return (uint32_t)((type_pref << 24) + (local_pref << 8) + (256 - component_id));
}

static uint64_t calculate_pair_priority(uint32_t controlling_prio, uint32_t controlled_prio,
                                        int is_controlling) {
  uint64_t g = is_controlling ? controlling_prio : controlled_prio;
  uint64_t d = is_controlling ? controlled_prio : controlling_prio;

  /* pair_priority = 2^32 * MIN(G, D) + 2 * MAX(G, D) + (G > D ? 1 : 0) */
  uint64_t min_val = (g < d) ? g : d;
  uint64_t max_val = (g > d) ? g : d;
  return (min_val << 32) + 2 * max_val + (g > d ? 1 : 0);
}

static int ice_ip_is_loopback(const char *ip) {
  struct in_addr addr4;
#if !defined(_WIN32)
  struct in6_addr addr6;
#endif

  if (!ip || ip[0] == '\0') {
    return 0;
  }

  if (inet_pton(AF_INET, ip, &addr4) == 1) {
    uint32_t host = ntohl(addr4.s_addr);
    return (host >> 24) == 127u;
  }

#if !defined(_WIN32)
  if (inet_pton(AF_INET6, ip, &addr6) == 1) {
    return IN6_IS_ADDR_LOOPBACK(&addr6) ? 1 : 0;
  }
#endif

  return 0;
}

/* Forward declarations for connectivity check functions */
static int send_connectivity_check(salts_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                   int nominate);
static void handle_stun_request(salts_ice_agent_t *agent, const uint8_t *data, size_t len,
                                const struct sockaddr *from, const char *peer_ip_override,
                                uint16_t peer_port_override, ice_candidate_t *local_cand);
static void handle_stun_response(salts_ice_agent_t *agent, const uint8_t *data, size_t len,
                                 const struct sockaddr *from, const char *peer_ip_override,
                                 uint16_t peer_port_override);
static ice_candidate_pair_t *find_pair_by_addresses(salts_ice_agent_t *agent, const char *local_ip,
                                                     uint16_t local_port, const char *remote_ip,
                                                     uint16_t remote_port);
static int find_pair_index_by_candidates(salts_ice_agent_t *agent, ice_candidate_t *local,
                                         ice_candidate_t *remote);
static void service_udp_candidate_socket(salts_ice_agent_t *agent, ice_candidate_t *local_cand,
                                         uint64_t timeout_ms, int allow_data);
static void service_turn_candidate_socket(salts_ice_agent_t *agent, ice_candidate_t *local_cand,
                                          uint64_t timeout_ms, int allow_data);
static ice_candidate_t *service_owner_candidate(salts_ice_agent_t *agent, ice_candidate_t *candidate);

/* ============================================================================
 * UDP Socket for Candidates (caller-driven CNet)
 * ============================================================================ */

enum { ICE_DATAGRAM_SEND_TIMEOUT_MS = 3000 };

static int destroy_candidate_transport_budget(void **socket, uint32_t timeout_ms) {
  ice_cnet_datagram_t *transport = (ice_cnet_datagram_t *)*socket;
  int status;
  if (!transport) return SALTS_OK;
  status = ice_cnet_datagram_destroy_budget(transport, timeout_ms);
  if (status != SALTS_OK) return status;
  free(transport);
  *socket = NULL;
  return SALTS_OK;
}

static int destroy_candidate_transport(void **socket) {
  return destroy_candidate_transport_budget(socket, ICE_CNET_STOP_TIMEOUT_MS);
}

static int create_candidate_socket(salts_ice_agent_t *agent, ice_candidate_t *candidate) {
  ice_candidate_runtime_t *runtime = ice_candidate_runtime(agent, candidate);
  ice_cnet_datagram_t *transport;
  uint16_t port = 0u;
  if (!candidate || !runtime || candidate->family != AF_INET || candidate->ip[0] == '\0') return -1;
  if (agent->gathering_socket) return SALTS_EBUSY;
  transport = (ice_cnet_datagram_t *)calloc(1u, sizeof(*transport));
  if (!transport) return -1;
  agent->gathering_socket = transport;
  if (ice_cnet_datagram_init(transport, candidate->ip, 0u, CNET_DATAGRAM_MAX_PAYLOAD_BYTES) !=
          SALTS_OK ||
      ice_cnet_datagram_port(transport, &port) != SALTS_OK) {
    (void)destroy_candidate_transport(&agent->gathering_socket);
    return -1;
  }
  runtime->socket = transport;
  agent->gathering_socket = NULL;
  candidate->port = port;
  return 0;
}

static int send_udp_to_remote(ice_cnet_datagram_t *transport, const char *ip, uint16_t port,
                              const void *data, size_t len) {
  cnet_datagram_peer peer;
  if (!transport || !ip || !data || len == 0u) return -1;
  if (ice_cnet_datagram_peer_from_text(ip, port, &peer) != SALTS_OK) return -1;
  return ice_cnet_datagram_send(transport, &peer, data, len, ICE_DATAGRAM_SEND_TIMEOUT_MS);
}

static int create_srflx_socket(salts_ice_agent_t *agent, const ice_candidate_t *base,
                               const char *server_host,
                               uint16_t server_port, int timeout_ms, int retries,
                               stun_mapped_address_t *mapped, ice_cnet_datagram_t **socket_out,
                               char *related_ip, size_t related_ip_len, uint16_t *related_port) {
  ice_cnet_datagram_t *transport;
  cnet_datagram_peer server_peer;
  stun_binding_transaction transaction = {0};
  uint16_t local_port = 0u;
  int result;
  if (!base || !server_host || !mapped || !socket_out || base->ip[0] == '\0') return -1;
  if (timeout_ms <= 0) timeout_ms = 3000;
  if (retries <= 0) retries = 3;
  if (agent->gathering_socket) return SALTS_EBUSY;
  transport = (ice_cnet_datagram_t *)calloc(1u, sizeof(*transport));
  if (!transport) return -2;
  agent->gathering_socket = transport;
  if (ice_cnet_datagram_init(transport, base->ip, 0u, STUN_MAX_MESSAGE_SIZE) != SALTS_OK ||
      ice_cnet_datagram_port(transport, &local_port) != SALTS_OK ||
      ice_cnet_datagram_resolve(server_host, server_port, &server_peer) != SALTS_OK) {
    (void)destroy_candidate_transport(&agent->gathering_socket);
    return -4;
  }
  if (related_ip && related_ip_len > 0u) {
    (void)snprintf(related_ip, related_ip_len, "%s", base->ip);
  }
  if (related_port) *related_port = local_port;

  /* Publish the wake target before admission; close cannot race an unregistered
   * wait. The transaction owns matching/deadlines, the ICE Owner owns polling. */
  if (ice_agent_wait_begin(agent, transport, ice_wake_datagram) != 0) {
    result = SALTS_ECANCELED;
  } else {
    result = stun_binding_transaction_start(&transaction, transport, &server_peer,
        (uint32_t)timeout_ms, (unsigned int)retries, cmeta_monotonic_ms());
    while (result == SALTS_OK && transaction.phase != STUN_BINDING_DONE) {
      if (ice_agent_is_closed(agent)) {
        result = stun_binding_transaction_cancel(&transaction);
        break;
      }
      result = stun_binding_transaction_advance(&transaction, cmeta_monotonic_ms());
      if (transaction.phase == STUN_BINDING_DONE) break;
      result = ice_cnet_datagram_poll_until(transport, transaction.deadline_ms);
      if (result == SALTS_ETIMEDOUT) result = SALTS_OK;
    }
    ice_agent_wait_end(agent, transport);
  }
  if (result == SALTS_OK && !ice_agent_is_closed(agent)) {
    *mapped = transaction.mapped;
    *socket_out = transport; /* Borrowed until gather commits the candidate. */
    return SALTS_OK;
  }
  (void)destroy_candidate_transport(&agent->gathering_socket);
  return -6;
}


/* ============================================================================
 * Host Candidate Gathering
 * ============================================================================ */

#ifdef _WIN32
static int gather_host_candidates_win32(salts_ice_agent_t *agent) {
  ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
  ULONG family = AF_UNSPEC;
  PIP_ADAPTER_ADDRESSES addresses = NULL;
  ULONG size = 0;
  ULONG result;

  /* Get required buffer size */
  result = GetAdaptersAddresses(family, flags, NULL, NULL, &size);
  if (result != ERROR_BUFFER_OVERFLOW)
    return -1;

  addresses = (PIP_ADAPTER_ADDRESSES)malloc(size);
  if (!addresses)
    return -1;

  result = GetAdaptersAddresses(family, flags, NULL, addresses, &size);
  if (result != NO_ERROR) {
    free(addresses);
    return -1;
  }

  PIP_ADAPTER_ADDRESSES adapter = addresses;
  while (adapter && agent->local_candidate_count < ICE_MAX_CANDIDATES) {
    /* Skip loopback and non-operational interfaces */
    if (adapter->OperStatus != IfOperStatusUp) {
      adapter = adapter->Next;
      continue;
    }
    if (!agent->config.allow_loopback && adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      adapter = adapter->Next;
      continue;
    }

    PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
    while (unicast && agent->local_candidate_count < ICE_MAX_CANDIDATES) {
      struct sockaddr *addr = unicast->Address.lpSockaddr;

      if (addr->sa_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
        /* Skip 127.x.x.x (unless allowed) and link-local 169.254.x.x */
        uint32_t ip = ntohl(addr4->sin_addr.s_addr);
        if ((!agent->config.allow_loopback && (ip >> 24) == 127) || (ip >> 16) == 0xA9FE) {
          unicast = unicast->Next;
          continue;
        }

        ice_candidate_t *cand = &agent->local_candidates[agent->local_candidate_count];
        memset(cand, 0, sizeof(*cand));

        cand->type = ICE_CANDIDATE_TYPE_HOST;
        cand->transport = ICE_TRANSPORT_UDP;
        cand->component_id = 1;
        cand->family = AF_INET;
        inet_ntop(AF_INET, &addr4->sin_addr, cand->ip, sizeof(cand->ip));
        cand->port = 0; /* Will be assigned when socket is created */
        cand->priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 1);
        if (initialize_candidate_identity(agent, cand) != 0) {
          free(addresses);
          return -2;
        }

        /* Create socket for this candidate */
        if (create_candidate_socket(agent, cand) == 0) {
          agent->local_candidate_count++;
        }
      }

      unicast = unicast->Next;
    }
    adapter = adapter->Next;
  }

  free(addresses);
  return 0;
}
#else
static int gather_host_candidates_unix(salts_ice_agent_t *agent) {
  struct ifaddrs *ifaddr, *ifa;

  if (getifaddrs(&ifaddr) == -1)
    return -1;

  for (ifa = ifaddr; ifa != NULL && agent->local_candidate_count < ICE_MAX_CANDIDATES;
       ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr)
      continue;

    /* Skip loopback and non-running interfaces */
    if (!(ifa->ifa_flags & IFF_RUNNING))
      continue;
    if (!agent->config.allow_loopback && (ifa->ifa_flags & IFF_LOOPBACK))
      continue;

    if (ifa->ifa_addr->sa_family == AF_INET) {
      struct sockaddr_in *addr4 = (struct sockaddr_in *)ifa->ifa_addr;
      uint32_t ip = ntohl(addr4->sin_addr.s_addr);

      /* Skip 127.x.x.x (unless allowed) and link-local 169.254.x.x */
      if ((!agent->config.allow_loopback && (ip >> 24) == 127) || (ip >> 16) == 0xA9FE)
        continue;

      ice_candidate_t *cand = &agent->local_candidates[agent->local_candidate_count];
      memset(cand, 0, sizeof(*cand));

      cand->type = ICE_CANDIDATE_TYPE_HOST;
      cand->transport = ICE_TRANSPORT_UDP;
      cand->component_id = 1;
      cand->family = AF_INET;
      inet_ntop(AF_INET, &addr4->sin_addr, cand->ip, sizeof(cand->ip));
      cand->port = 0;
      cand->priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_HOST, 65535, 1);
      if (initialize_candidate_identity(agent, cand) != 0) {
        freeifaddrs(ifaddr);
        return -2;
      }

      /* Create socket for this candidate */
      if (create_candidate_socket(agent, cand) == 0) {
        agent->local_candidate_count++;
      }
    }
  }

  freeifaddrs(ifaddr);
  return 0;
}
#endif

static int gather_host_candidates(salts_ice_agent_t *agent) {
#ifdef _WIN32
  return gather_host_candidates_win32(agent);
#else
  return gather_host_candidates_unix(agent);
#endif
}

/* ============================================================================
 * STUN Gathering (caller-driven CNet)
 * ============================================================================ */

static void gather_srflx_candidates(salts_ice_agent_t *agent) {
  for (int i = 0; i < agent->config.stun_server_count && i < ICE_MAX_STUN_SERVERS; i++) {
    const char *url = agent->config.stun_servers[i].url;

    char host[256] = {0};
    uint16_t port = STUN_DEFAULT_PORT;

    const char *p = url;
    if (strncmp(p, "stun:", 5) == 0)
      p += 5;
    if (strncmp(p, "//", 2) == 0)
      p += 2;

    const char *colon = strchr(p, ':');
    if (colon) {
      size_t host_len = colon - p;
      if (host_len < sizeof(host)) {
        memcpy(host, p, host_len);
        port = (uint16_t)atoi(colon + 1);
      }
    } else {
      strncpy(host, p, sizeof(host) - 1);
    }

    if (host[0] == '\0')
      continue;

    int base_count = agent->local_candidate_count;
    for (int j = 0; j < base_count && agent->local_candidate_count < ICE_MAX_CANDIDATES; j++) {
      ice_candidate_t *base = &agent->local_candidates[j];
      ice_candidate_runtime_t *base_runtime = ice_candidate_runtime(agent, base);
      if (base->type != ICE_CANDIDATE_TYPE_HOST || !base_runtime || !base_runtime->socket ||
          base->family != AF_INET) {
        continue;
      }

      stun_mapped_address_t mapped;
      ice_cnet_datagram_t *srflx_socket = NULL;
      char related_ip[64] = {0};
      uint16_t related_port = 0;
      int result = create_srflx_socket(agent, base, host, port, 1000, 1, &mapped,
                                       &srflx_socket, related_ip, sizeof(related_ip),
                                       &related_port);
      if (result != 0 || mapped.family != STUN_ADDR_FAMILY_IPV4) {
        if (srflx_socket) {
          (void)destroy_candidate_transport(&agent->gathering_socket);
        }
        continue;
      }
      if (strcmp(mapped.ip_str, related_ip[0] ? related_ip : base->ip) == 0 &&
          mapped.port == (related_port ? related_port : base->port)) {
        if (srflx_socket) {
          (void)destroy_candidate_transport(&agent->gathering_socket);
        }
        continue;
      }

      ice_candidate_t *cand = &agent->local_candidates[agent->local_candidate_count];
      memset(cand, 0, sizeof(*cand));

      cand->type = ICE_CANDIDATE_TYPE_SRFLX;
      cand->transport = ICE_TRANSPORT_UDP;
      cand->component_id = base->component_id;
      cand->family = AF_INET;
      strncpy(cand->ip, mapped.ip_str, sizeof(cand->ip) - 1);
      cand->port = mapped.port;
      cand->priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_SRFLX, 65534, 1);
      if (initialize_candidate_identity(agent, cand) != 0) {
        (void)destroy_candidate_transport(&agent->gathering_socket);
        return;
      }

      strncpy(cand->related_ip, related_ip[0] ? related_ip : base->ip,
              sizeof(cand->related_ip) - 1);
      cand->related_port = related_port ? related_port : base->port;
      ice_candidate_runtime(agent, cand)->socket = srflx_socket;
      agent->gathering_socket = NULL;

      agent->local_candidate_count++;

      if (agent->callbacks.on_candidate) {
        agent->callbacks.on_candidate(agent, cand, agent->callbacks.user_data);
      }
    }
  }
}

/* ============================================================================
 * TURN Gathering (caller-driven CNet)
 * ============================================================================ */

static void gather_relay_candidates(salts_ice_agent_t *agent) {
  for (int i = 0; i < agent->config.turn_server_count && i < ICE_MAX_TURN_SERVERS; i++) {
    if (ice_agent_is_closed(agent))
      return;

    const char *url = agent->config.turn_servers[i].url;
    const char *username = agent->config.turn_servers[i].username;
    const char *password = agent->config.turn_servers[i].credential;

    char host[256] = {0};
    uint16_t port = TURN_DEFAULT_PORT;

    const char *p = url;
    if (strncmp(p, "turn:", 5) == 0)
      p += 5;
    if (strncmp(p, "turns:", 6) == 0)
      p += 6;
    if (strncmp(p, "//", 2) == 0)
      p += 2;

    const char *colon = strchr(p, ':');
    if (colon) {
      size_t host_len = colon - p;
      if (host_len < sizeof(host)) {
        memcpy(host, p, host_len);
        port = (uint16_t)atoi(colon + 1);
      }
    } else {
      strncpy(host, p, sizeof(host) - 1);
    }

    if (host[0] == '\0' || !username || !password)
      continue;

    turn_client_config_t turn_config = {
        .server_host = host,
        .server_port = port,
        .username = username,
        .password = password,
        .timeout_ms = 5000,
        .lifetime = TURN_DEFAULT_LIFETIME};

    salts_turn_client_t *turn = turn_client_create(&turn_config);
    if (!turn) continue;

    agent->turn_clients[i] = turn;

    turn_allocation_t alloc;
    int result = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
      if (ice_agent_is_closed(agent))
        return;

      if (ice_agent_wait_begin(agent, turn, ice_wake_turn) != 0) return;
      result = turn_client_allocate(turn, &alloc);
      ice_agent_wait_end(agent, turn);
      if (ice_agent_is_closed(agent))
        return;
      if (result == 0) {
        break;
      }
      TLOG_WARNF("TURN allocate failed server={} port={} attempt={} rc={}", host, port,
                attempt + 1, result);
      if (attempt + 1 < 3) {
        cmeta_sleep_ms(200u);
        if (ice_agent_is_closed(agent))
          return;
      }
    }

    if (result == 0 && agent->local_candidate_count < ICE_MAX_CANDIDATES) {
      ice_candidate_t *cand = &agent->local_candidates[agent->local_candidate_count];
      memset(cand, 0, sizeof(*cand));

      cand->type = ICE_CANDIDATE_TYPE_RELAY;
      cand->transport = ICE_TRANSPORT_UDP;
      cand->component_id = 1;
      cand->family = AF_INET;
      strncpy(cand->ip, alloc.relayed_ip, sizeof(cand->ip) - 1);
      cand->port = alloc.relayed_port;
      cand->priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_RELAY, 65533, 1);
      if (initialize_candidate_identity(agent, cand) != 0) {
        return;
      }

      strncpy(cand->related_ip, alloc.mapped_ip, sizeof(cand->related_ip) - 1);
      cand->related_port = alloc.mapped_port;
      ice_candidate_runtime(agent, cand)->turn_client = turn;

      agent->local_candidate_count++;
      TLOG_INFOF("Gathered relay candidate {}:{} via {}", cand->ip, cand->port, host);

      if (agent->callbacks.on_candidate) {
        agent->callbacks.on_candidate(agent, cand, agent->callbacks.user_data);
      }
    } else if (result != 0) {
      TLOG_WARNF("TURN relay candidate unavailable server={} port={} rc={}", host, port, result);
    }
  }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ice_config_t ice_default_config(void) {
  ice_config_t config;
  memset(&config, 0, sizeof(config));

  config.gathering_timeout_ms = ICE_DEFAULT_GATHERING_TIMEOUT;
  config.connectivity_timeout_ms = ICE_DEFAULT_CONNECTIVITY_TIMEOUT;
  config.keepalive_interval_ms = ICE_DEFAULT_KEEPALIVE_INTERVAL;
  config.is_controlling = 1;
  config.aggressive_nomination = 0;
  config.lite_mode = 0;
  config.allow_loopback = 0;

  return config;
}

int ice_agent_set_role(salts_ice_agent_t *agent, int is_controlling) {
  int rc = 0;

  if (!agent)
    return -1;
  ice_agent_owner_enter(agent);
  if (ice_agent_is_closed(agent)) {
    rc = ICE_AGENT_ERROR_CLOSED;
    goto done;
  }
  if (agent->state != ICE_STATE_NEW && agent->state != ICE_STATE_GATHERING) {
    rc = -2;
    goto done;
  }

  agent->config.is_controlling = is_controlling ? 1 : 0;
  agent->role = is_controlling ? ICE_ROLE_CONTROLLING : ICE_ROLE_CONTROLLED;

done:
  ice_agent_owner_leave(agent);
  return rc;
}


salts_ice_agent_t *ice_agent_create(const ice_config_t *config) {
  ice_progress_owner_t *progress;
  if (!config)
    return NULL;
  if (config->keepalive_interval_ms < ICE_CONSENT_MIN_BASE_INTERVAL_MS ||
      config->keepalive_interval_ms > ICE_CONSENT_MAX_BASE_INTERVAL_MS)
    return NULL;
  if (config->use_mdns_candidates) return NULL;

  salts_ice_agent_t *agent = calloc(1, sizeof(salts_ice_agent_t));
  if (!agent)
    return NULL;

  progress = (ice_progress_owner_t *)calloc(1u, sizeof(*progress));
  if (!progress) {
    free(agent);
    return NULL;
  }
  atomic_init(&progress->close_requested, 0);
  cmeta_mutex_init(&progress->mutex);
  agent->progress_owner_reserved = progress;

  agent->config = *config;

  agent->state = ICE_STATE_NEW;
  agent->gathering_state = ICE_GATHERING_NEW;
  agent->role = config->is_controlling ? ICE_ROLE_CONTROLLING : ICE_ROLE_CONTROLLED;
  if (generate_random_u64(&agent->tie_breaker) != 0 ||
      generate_random_string(agent->local_ufrag, 8) != 0 ||
      generate_random_string(agent->local_pwd, 24) != 0) {
    cmeta_mutex_destroy(&progress->mutex);
    free(progress);
    free(agent);
    return NULL;
  }

  return agent;
}




static uint32_t ice_cleanup_remaining(uint64_t started, uint32_t budget) {
  uint64_t elapsed = cmeta_monotonic_ms() - started;
  return elapsed < budget ? budget - (uint32_t)elapsed : 0u;
}

static int ice_agent_quiesce_transports(salts_ice_agent_t *agent, uint32_t timeout_ms) {
  const uint64_t started = cmeta_monotonic_ms();
  int first, status;
  bool visited[ICE_MAX_CANDIDATES] = {false};
  if (!agent) return SALTS_OK;
  first = destroy_candidate_transport_budget(&agent->gathering_socket,
      ice_cleanup_remaining(started, timeout_ms));

  for (int i = 0; i < ICE_MAX_TURN_SERVERS; i++) {
    if (agent->turn_clients[i]) {
      bool aliases[ICE_MAX_CANDIDATES];
      for (int j = 0; j < ICE_MAX_CANDIDATES; ++j)
        aliases[j] = agent->local_candidate_runtime[j].turn_client == agent->turn_clients[i];
      status = turn_client_destroy_checked(&agent->turn_clients[i],
          ice_cleanup_remaining(started, timeout_ms));
      if (status == SALTS_OK) {
        agent->turn_clients[i] = NULL;
        for (int j = 0; j < ICE_MAX_CANDIDATES; ++j)
          if (aliases[j]) agent->local_candidate_runtime[j].turn_client = NULL;
      } else if (first == SALTS_OK) first = status;
    }
  }

  for (int i = 0; i < ICE_MAX_CANDIDATES; i++) {
    ice_candidate_runtime_t *runtime = &agent->local_candidate_runtime[i];
    runtime->io_active = 0;
    if (runtime->socket && !visited[i]) {
      bool aliases[ICE_MAX_CANDIDATES];
      for (int j = 0; j < ICE_MAX_CANDIDATES; ++j) {
        aliases[j] = agent->local_candidate_runtime[j].socket == runtime->socket;
        if (aliases[j]) visited[j] = true;
      }
      status = destroy_candidate_transport_budget(&runtime->socket,
          ice_cleanup_remaining(started, timeout_ms));
      if (status == SALTS_OK) {
        for (int j = 0; j < ICE_MAX_CANDIDATES; ++j)
          if (aliases[j]) agent->local_candidate_runtime[j].socket = NULL;
      } else if (first == SALTS_OK) first = status;
    }
  }
  return first;
}

static int ice_agent_release(salts_ice_agent_t *agent, uint32_t timeout_ms) {
  ice_progress_owner_t *progress;
  int status;
  if (!agent)
    return SALTS_OK;
  /* Keep the enclosing Owner and every failed handle available for another
   * destroy attempt. Successful siblings have already been cleared exactly once. */
  status = ice_agent_quiesce_transports(agent, timeout_ms);
  if (status != SALTS_OK) return status;
  progress = ice_agent_progress(agent);
  if (progress) {
    cmeta_mutex_destroy(&progress->mutex);
    free(progress);
    agent->progress_owner_reserved = NULL;
  }
  free(agent);
  return SALTS_OK;
}

int ice_agent_destroy_checked(salts_ice_agent_t **owner, uint32_t timeout_ms) {
  salts_ice_agent_t *agent;
  ice_progress_owner_t *progress;
  int status;
  if (!owner) return SALTS_EINVAL;
  agent = *owner;
  if (!agent) return SALTS_OK;
  progress = ice_agent_progress(agent);
  if (progress) {
    cmeta_mutex_lock(&progress->mutex);
    if (progress->owner_active) {
      cmeta_mutex_unlock(&progress->mutex);
      return SALTS_EBUSY;
    }
    ice_progress_set_current_thread_owner(progress);
    cmeta_mutex_unlock(&progress->mutex);
  }
  ice_agent_close(agent);
  ice_agent_owner_leave(agent);
  status = ice_agent_release(agent, timeout_ms);
  if (status == SALTS_OK) *owner = NULL;
  return status;
}

void ice_agent_destroy(salts_ice_agent_t *agent) {
  (void)ice_agent_destroy_checked(&agent, ICE_CNET_STOP_TIMEOUT_MS);
}


void ice_agent_set_callbacks(salts_ice_agent_t *agent, const ice_callbacks_t *callbacks) {
  if (agent && callbacks) {
    ice_agent_owner_enter(agent);
    agent->callbacks = *callbacks;
    ice_agent_owner_leave(agent);
  }
}

void ice_agent_get_local_credentials(salts_ice_agent_t *agent, char *ufrag, size_t ufrag_len,
                                     char *pwd, size_t pwd_len) {
  if (!agent)
    return;
  if (ufrag && ufrag_len > 0) {
    strncpy(ufrag, agent->local_ufrag, ufrag_len - 1);
    ufrag[ufrag_len - 1] = '\0';
  }
  if (pwd && pwd_len > 0) {
    strncpy(pwd, agent->local_pwd, pwd_len - 1);
    pwd[pwd_len - 1] = '\0';
  }
}

int ice_agent_set_remote_credentials(salts_ice_agent_t *agent, const char *ufrag, const char *pwd) {
  int rc = 0;
  size_t ufrag_len;
  size_t pwd_len;

  if (!agent || !ufrag || !pwd)
    return -1;
  ufrag_len = strlen(ufrag);
  pwd_len = strlen(pwd);
  ice_agent_owner_enter(agent);
  if (ice_agent_is_closed(agent)) {
    rc = ICE_AGENT_ERROR_CLOSED;
    goto done;
  }
  if (ufrag_len == 0 || ufrag_len >= sizeof(agent->remote_ufrag) ||
      pwd_len == 0 || pwd_len >= sizeof(agent->remote_pwd)) {
    rc = -2;
    goto done;
  }

  memcpy(agent->remote_ufrag, ufrag, ufrag_len + 1);
  memcpy(agent->remote_pwd, pwd, pwd_len + 1);
  agent->remote_credentials_set = 1;

done:
  ice_agent_owner_leave(agent);
  return rc;
}

ice_restart_options_t ice_restart_options_default(void) {
  ice_restart_options_t options;

  memset(&options, 0, sizeof(options));
  options.version = ICE_RESTART_OPTIONS_VERSION_1;
  options.struct_size = (uint32_t)sizeof(options);
  return options;
}

int ice_agent_restart(salts_ice_agent_t *agent,
                      const ice_restart_options_t *options) {
  int rc = 0;
  char next_ufrag[sizeof(agent->local_ufrag)];
  char next_pwd[sizeof(agent->local_pwd)];
  uint64_t next_tie_breaker;

  if (!agent || !options)
    return -1;
  ice_agent_owner_enter(agent);
  if (ice_agent_is_closed(agent)) {
    rc = ICE_AGENT_ERROR_CLOSED;
    goto done;
  }
  if (options->version != ICE_RESTART_OPTIONS_VERSION_1 ||
      options->struct_size < sizeof(*options) ||
      options->flags != 0 || options->reserved != 0) {
    rc = ICE_AGENT_ERROR_INVALID_OPTIONS;
    goto done;
  }
  if ((agent->state == ICE_STATE_GATHERING &&
       agent->gathering_state != ICE_GATHERING_COMPLETE) ||
      agent->state == ICE_STATE_CONNECTING ||
      agent->checks_in_progress) {
    rc = ICE_AGENT_ERROR_BUSY;
    goto done;
  }
  if (generate_random_u64(&next_tie_breaker) != 0 ||
      generate_random_string(next_ufrag, sizeof(next_ufrag)) != 0 ||
      generate_random_string(next_pwd, sizeof(next_pwd)) != 0) {
    rc = -2;
    goto done;
  }

  memcpy(agent->local_ufrag, next_ufrag, sizeof(next_ufrag));
  memcpy(agent->local_pwd, next_pwd, sizeof(next_pwd));
  agent->tie_breaker = next_tie_breaker;

  memset(agent->remote_ufrag, 0, sizeof(agent->remote_ufrag));
  memset(agent->remote_pwd, 0, sizeof(agent->remote_pwd));
  memset(agent->remote_candidates, 0, sizeof(agent->remote_candidates));
  memset(agent->pairs, 0, sizeof(agent->pairs));
  memset(&agent->current_txn_id, 0, sizeof(agent->current_txn_id));
  memset(agent->triggered_checks, 0, sizeof(agent->triggered_checks));

  agent->remote_candidate_count = 0;
  agent->pair_count = 0;
  agent->selected_pair = NULL;
  agent->current_check_pair = -1;
  agent->checks_in_progress = 0;
  agent->valid_pairs_count = 0;
  agent->check_start_time = 0;
  agent->pending_stun_requests = 0;
  agent->pending_turn_requests = 0;
  agent->remote_credentials_set = 0;
  agent->remote_candidates_complete = 0;
  agent->nomination_started = 0;
  agent->current_check_nominating = 0;
  agent->current_check_select_on_success = 0;
  agent->triggered_check_head = 0;
  agent->triggered_check_count = 0;
  agent->last_keepalive_ms = 0;
  memset(agent->consent_txn_ids, 0, sizeof(agent->consent_txn_ids));
  memset(agent->consent_txn_sent_ms, 0, sizeof(agent->consent_txn_sent_ms));
  agent->consent_txn_next = 0;
  agent->last_consent_response_ms = 0;
  agent->next_consent_check_ms = 0;

  set_state(agent, ICE_STATE_NEW);
  if (ice_agent_is_closed(agent)) rc = ICE_AGENT_ERROR_CLOSED;

done:
  ice_agent_owner_leave(agent);
  return rc;
}

int ice_agent_gather_candidates(salts_ice_agent_t *agent) {
  int rc;

  if (!agent)
    return -1;
  ice_agent_owner_enter(agent);
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }

  if (agent->state != ICE_STATE_NEW) {
    ice_agent_owner_leave(agent);
    return -2;
  }

  set_state(agent, ICE_STATE_GATHERING);
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }
  set_gathering_state(agent, ICE_GATHERING_GATHERING);
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }

  /* 1. Gather host candidates */
  rc = gather_host_candidates(agent);
  if (agent->gathering_socket) rc = SALTS_EBUSY;
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }
  if (rc != 0) {
    set_gathering_state(agent, ICE_GATHERING_COMPLETE);
    set_state(agent, ICE_STATE_FAILED);
    ice_agent_owner_leave(agent);
    return -3;
  }

  /* Notify host candidates */
  for (int i = 0; i < agent->local_candidate_count; i++) {
    if (agent->callbacks.on_candidate) {
      agent->callbacks.on_candidate(agent, &agent->local_candidates[i], agent->callbacks.user_data);
      if (ice_agent_is_closed(agent)) {
        ice_agent_owner_leave(agent);
        return ICE_AGENT_ERROR_CLOSED;
      }
    }
  }

  /* 2. Gather server-reflexive candidates via STUN */
  if (agent->config.stun_server_count > 0) {
    gather_srflx_candidates(agent);
    if (ice_agent_is_closed(agent)) {
      ice_agent_owner_leave(agent);
      return ICE_AGENT_ERROR_CLOSED;
    }
    if (agent->gathering_socket) {
      set_gathering_state(agent, ICE_GATHERING_COMPLETE);
      set_state(agent, ICE_STATE_FAILED);
      ice_agent_owner_leave(agent);
      return SALTS_EBUSY;
    }
  }

  /* 3. Gather relay candidates via TURN */
  if (agent->config.turn_server_count > 0) {
    gather_relay_candidates(agent);
    if (ice_agent_is_closed(agent)) {
      ice_agent_owner_leave(agent);
      return ICE_AGENT_ERROR_CLOSED;
    }
  }

  /* Gathering is synchronous and complete when this call returns. */
  set_gathering_state(agent, ICE_GATHERING_COMPLETE);
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }

  ice_agent_owner_leave(agent);
  return 0;
}

int ice_agent_add_remote_candidate(salts_ice_agent_t *agent, const char *candidate_str) {
  ice_candidate_t parsed;
  int rc = 0;

  if (!agent || !candidate_str)
    return -1;
  ice_agent_owner_enter(agent);
  if (ice_agent_is_closed(agent)) {
    rc = ICE_AGENT_ERROR_CLOSED;
    goto done;
  }

  if (agent->remote_candidate_count >= ICE_MAX_CANDIDATES) {
    rc = -2;
    goto done;
  }

  memset(&parsed, 0, sizeof(parsed));

  if (ice_candidate_parse(candidate_str, &parsed) != 0) {
    ice_tracef("ice_agent_add_remote_candidate parse_failed current_remote=%d candidate=%s",
               agent->remote_candidate_count, candidate_str);
    rc = -3;
    goto done;
  }

  rc = parsed.mdns_name[0] != '\0' ? resolve_mdns_hostname(&parsed) : 0;
  if (ice_agent_is_closed(agent)) {
    rc = ICE_AGENT_ERROR_CLOSED;
    goto done;
  }
  if (rc != 0) {
    ice_tracef("ice_agent_add_remote_candidate mdns_resolve_failed current_remote=%d candidate=%s",
               agent->remote_candidate_count, candidate_str);
    rc = -4;
    goto done;
  }

  for (int i = 0; i < agent->remote_candidate_count; i++) {
    if (ice_candidates_equivalent(&agent->remote_candidates[i], &parsed)) {
      ice_tracef("ice_agent_add_remote_candidate duplicate_ignored current_remote=%d candidate=%s",
                 agent->remote_candidate_count, candidate_str);
      if (agent->state == ICE_STATE_CONNECTING) {
        rebuild_candidate_pairs(agent);
      }
      goto done;
    }
  }

  ice_candidate_t *cand = &agent->remote_candidates[agent->remote_candidate_count];
  *cand = parsed;
  agent->remote_candidate_count++;
  ice_tracef("ice_agent_add_remote_candidate added remote=%d candidate=%s",
             agent->remote_candidate_count, candidate_str);

  /* If already connecting, rebuild pairs to include this new candidate */
  if (agent->state == ICE_STATE_CONNECTING) {
    rebuild_candidate_pairs(agent);
  }

done:
  ice_agent_owner_leave(agent);
  return rc;
}

void ice_agent_end_of_candidates(salts_ice_agent_t *agent) {
  if (agent) {
    ice_agent_owner_enter(agent);
    if (ice_agent_is_closed(agent)) {
      ice_agent_owner_leave(agent);
      return;
    }
    agent->remote_candidates_complete = 1;
    ice_tracef("ice_agent_end_of_candidates remote=%d state=%d",
               agent->remote_candidate_count, (int)agent->state);
    ice_agent_owner_leave(agent);
  }
}

/* ============================================================================
 * Connectivity Check Implementation
 * ============================================================================ */

static ice_candidate_pair_t *find_pair_by_addresses(salts_ice_agent_t *agent, const char *local_ip,
                                                    uint16_t local_port, const char *remote_ip,
                                                    uint16_t remote_port) {
  for (int i = 0; i < agent->pair_count; i++) {
    ice_candidate_pair_t *pair = &agent->pairs[i];
    if (strcmp(pair->local->ip, local_ip) == 0 && pair->local->port == local_port &&
        strcmp(pair->remote->ip, remote_ip) == 0 && pair->remote->port == remote_port) {
      return pair;
    }
  }
  return NULL;
}

static int enqueue_triggered_check(salts_ice_agent_t *agent, ice_candidate_t *local,
                                   ice_candidate_t *remote, int received_nomination) {
  int tail;

  if (!agent || !local || !remote)
    return -1;
  for (int i = 0; i < agent->triggered_check_count; ++i) {
    int index = (agent->triggered_check_head + i) % ICE_MAX_CANDIDATE_PAIRS;
    ice_triggered_check_t *entry = &agent->triggered_checks[index];
    if (entry->local == local && entry->remote == remote) {
      entry->received_nomination |= received_nomination;
      return 0;
    }
  }
  if (agent->triggered_check_count >= ICE_MAX_CANDIDATE_PAIRS)
    return -1;

  tail = (agent->triggered_check_head + agent->triggered_check_count) % ICE_MAX_CANDIDATE_PAIRS;
  agent->triggered_checks[tail].local = local;
  agent->triggered_checks[tail].remote = remote;
  agent->triggered_checks[tail].received_nomination = received_nomination;
  agent->triggered_check_count++;
  return 0;
}

static int dequeue_triggered_check(salts_ice_agent_t *agent, int *pair_index,
                                   int *received_nomination) {
  while (agent && agent->triggered_check_count > 0) {
    ice_triggered_check_t entry = agent->triggered_checks[agent->triggered_check_head];
    agent->triggered_check_head = (agent->triggered_check_head + 1) % ICE_MAX_CANDIDATE_PAIRS;
    agent->triggered_check_count--;
    *pair_index = find_pair_index_by_candidates(agent, entry.local, entry.remote);
    if (*pair_index >= 0) {
      *received_nomination = entry.received_nomination;
      return 1;
    }
  }
  return 0;
}

static int send_connectivity_check_internal(salts_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                            int nominate, int track_transaction) {
  if (!agent || !pair || !pair->local || !pair->remote)
    return -1;

  stun_transaction_id_t txn_id;
  stun_transaction_id_t *active_txn = &txn_id;

  if (track_transaction) {
    active_txn = &agent->current_txn_id;
  }

  /* Generate transaction ID */
  if (stun_generate_transaction_id(active_txn) != 0)
    return -1;

  /* Build ICE STUN request */
  uint8_t stun_buf[STUN_MAX_MESSAGE_SIZE];
  int local_preference = (int)((pair->local->priority >> 8) & 0xffffu);
  uint32_t peer_reflexive_priority = ice_calculate_priority(
      ICE_CANDIDATE_TYPE_PRFLX, local_preference, pair->local->component_id);
  int len = stun_build_ice_request(stun_buf, active_txn, agent->local_ufrag, agent->remote_ufrag,
                                   agent->remote_pwd, peer_reflexive_priority,
                                   agent->role == ICE_ROLE_CONTROLLING, agent->tie_breaker,
                                   nominate);

  if (len < 0) {
    TLOG_DEBUG("Failed to build STUN request");
    return -1;
  }

  TLOG_DEBUGF("Outgoing BINDING REQUEST to {}:{} (txn: {})", pair->remote->ip,
            pair->remote->port, STUN_TRANSACTION_ID_LEN, active_txn->id);

  /* Send via suitable transport */
  ice_candidate_runtime_t *local_runtime = ice_candidate_runtime(agent, pair->local);
  int rc = -1;
  if (pair->local->type == ICE_CANDIDATE_TYPE_RELAY) {
    if (local_runtime && local_runtime->turn_client) {
      ice_tracef("send_connectivity_check relay_send begin local=%s:%u remote=%s:%u nominate=%d",
                 pair->local->ip, (unsigned int)pair->local->port, pair->remote->ip,
                 (unsigned int)pair->remote->port, nominate);
      rc = turn_client_send(local_runtime->turn_client, pair->remote->ip,
                            pair->remote->port, stun_buf, len);
      ice_tracef("send_connectivity_check relay_send rc=%d local=%s:%u remote=%s:%u", rc,
                 pair->local->ip, (unsigned int)pair->local->port, pair->remote->ip,
                 (unsigned int)pair->remote->port);
    }
  } else if (local_runtime && local_runtime->socket) {
    ice_cnet_datagram_t *client = (ice_cnet_datagram_t *)local_runtime->socket;
    rc = send_udp_to_remote(client, pair->remote->ip, pair->remote->port, stun_buf, (size_t)len);
  }

  if (rc == 0) {
    if (track_transaction) {
      pair->state = ICE_PAIR_STATE_IN_PROGRESS;
      pair->check_count++;
      pair->last_check_time = cmeta_monotonic_ms();
      agent->current_check_nominating = nominate;
    }
    TLOG_DEBUGF("ICE check sent {}:{} -> {}:{} nominate={} pair_state={}",
               pair->local->ip, pair->local->port, pair->remote->ip, pair->remote->port,
               nominate, (int)pair->state);
  } else {
    TLOG_WARNF("ICE check send failed {}:{} -> {}:{} rc={}",
              pair->local->ip, pair->local->port, pair->remote->ip, pair->remote->port, rc);
    if (track_transaction) {
      pair->state = ICE_PAIR_STATE_FAILED;
    }
  }

  return rc;
}

static int send_connectivity_check(salts_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                   int nominate) {
  return send_connectivity_check_internal(agent, pair, nominate, 1);
}

static ice_candidate_pair_t *ensure_peer_reflexive_pair(salts_ice_agent_t *agent,
                                                        ice_candidate_t *local_cand,
                                                        const char *remote_ip,
                                                        uint16_t remote_port,
                                                        uint32_t priority) {
  ice_candidate_t *remote = NULL;
  ice_candidate_pair_t *pair;

  pair = find_pair_by_addresses(agent, local_cand->ip, local_cand->port,
                                remote_ip, remote_port);
  if (pair)
    return pair;

  for (int i = 0; i < agent->remote_candidate_count; ++i) {
    ice_candidate_t *candidate = &agent->remote_candidates[i];
    if (candidate->component_id == local_cand->component_id &&
        candidate->transport == local_cand->transport &&
        candidate->port == remote_port && strcmp(candidate->ip, remote_ip) == 0) {
      remote = candidate;
      break;
    }
  }

  if (!remote) {
    if (agent->remote_candidate_count >= ICE_MAX_CANDIDATES)
      return NULL;
    remote = &agent->remote_candidates[agent->remote_candidate_count];
    memset(remote, 0, sizeof(*remote));
    remote->type = ICE_CANDIDATE_TYPE_PRFLX;
    remote->transport = local_cand->transport;
    remote->component_id = local_cand->component_id;
    remote->family = local_cand->family;
    remote->priority = priority;
    remote->port = remote_port;
    strncpy(remote->ip, remote_ip, sizeof(remote->ip) - 1);
    if (initialize_candidate_identity(agent, remote) != 0) {
      memset(remote, 0, sizeof(*remote));
      return NULL;
    }
    agent->remote_candidate_count++;
  }

  if (agent->pair_count >= ICE_MAX_CANDIDATE_PAIRS)
    return NULL;
  pair = &agent->pairs[agent->pair_count++];
  memset(pair, 0, sizeof(*pair));
  pair->local = local_cand;
  pair->remote = remote;
  pair->state = ICE_PAIR_STATE_WAITING;
  pair->priority = calculate_pair_priority(local_cand->priority, remote->priority,
                                           agent->role == ICE_ROLE_CONTROLLING);
  return pair;
}


static void handle_stun_request(salts_ice_agent_t *agent, const uint8_t *data, size_t len,
                                const struct sockaddr *from, const char *peer_ip_override,
                                uint16_t peer_port_override, ice_candidate_t *local_cand) {
  if (!agent || !data || len < STUN_HEADER_SIZE || !local_cand)
    return;

  /* Parse the request */
  char username[256] = {0};
  uint32_t priority = 0;
  int use_candidate = 0;
  stun_transaction_id_t rx_txn_id;
  memcpy(rx_txn_id.id, data + 8, STUN_TRANSACTION_ID_LEN);

  if (stun_parse_ice_request(data, len, username, &priority, &use_candidate) != 0) {
    TLOG_WARN("Failed to parse incoming STUN request");
    ice_tracef("handle_stun_request parse_failed len=%zu local=%s:%u", len, local_cand->ip,
               (unsigned int)local_cand->port);
    return;
  }

  /* Validate username: should be "local_ufrag:remote_ufrag" */
  char expected_username[256];
  fmt(expected_username, sizeof(expected_username), "{}:{}", agent->local_ufrag, agent->remote_ufrag);
  if (strcmp(username, expected_username) != 0) {
    TLOG_WARNF("STUN username mismatch got='{}' expected='{}'", username, expected_username);
    ice_tracef("handle_stun_request username_mismatch got=%s expected=%s local=%s:%u", username,
               expected_username, local_cand->ip, (unsigned int)local_cand->port);
    return; /* Username mismatch */
  }

  /* Validate MESSAGE-INTEGRITY */
  if (stun_validate_message_integrity(data, len, agent->local_pwd) != 0) {
    TLOG_WARN("STUN request integrity validation failed");
    ice_tracef("handle_stun_request integrity_failed user=%s local=%s:%u", username,
               local_cand->ip, (unsigned int)local_cand->port);
    return; /* Invalid authentication */
  }

  /* Get sender's address */
  char remote_ip[64];
  uint16_t remote_port;
  if (from && from->sa_family == AF_INET) {
    struct sockaddr_in *addr4 = (struct sockaddr_in *)from;
    inet_ntop(AF_INET, &addr4->sin_addr, remote_ip, sizeof(remote_ip));
    remote_port = ntohs(addr4->sin_port);
  } else if (peer_ip_override && peer_ip_override[0] != '\0') {
    strncpy(remote_ip, peer_ip_override, sizeof(remote_ip) - 1);
    remote_ip[sizeof(remote_ip) - 1] = '\0';
    remote_port = peer_port_override;
  } else {
    return;
  }

  /* Build and send response using our local ICE password. The peer validates
   * the response against the credentials we advertised in signaling. */
  TLOG_DEBUGF("Incoming BINDING REQUEST from {}:{} (txn: {})", remote_ip, remote_port,
            STUN_TRANSACTION_ID_LEN, rx_txn_id.id);
  TLOG_DEBUGF("Incoming STUN request from {}:{} to {}:{} use_candidate={}",
             remote_ip, remote_port, local_cand->ip, local_cand->port, use_candidate);
  ice_tracef("handle_stun_request accepted from=%s:%u to=%s:%u use_candidate=%d", remote_ip,
             (unsigned int)remote_port, local_cand->ip, (unsigned int)local_cand->port,
             use_candidate);
  uint8_t resp_buf[STUN_MAX_MESSAGE_SIZE];
  stun_transaction_id_t txn_id;
  memcpy(txn_id.id, data + 8, STUN_TRANSACTION_ID_LEN);

  int resp_len =
      stun_build_ice_response(resp_buf, &txn_id, agent->local_pwd, remote_ip, remote_port);
  if (resp_len > 0) {
    ice_candidate_runtime_t *local_runtime = ice_candidate_runtime(agent, local_cand);
    int send_rc = -1;
    if (local_cand->type == ICE_CANDIDATE_TYPE_RELAY && local_runtime &&
        local_runtime->turn_client) {
      send_rc = turn_client_send(local_runtime->turn_client, remote_ip,
                                 remote_port, resp_buf, resp_len);
    } else if (local_runtime && local_runtime->socket) {
      ice_cnet_datagram_t *client = (ice_cnet_datagram_t *)local_runtime->socket;
      send_rc = send_udp_to_remote(client, remote_ip, remote_port, resp_buf, (size_t)resp_len);
    }
    ice_tracef("handle_stun_request response rc=%d from=%s:%u to=%s:%u", send_rc, local_cand->ip,
               (unsigned int)local_cand->port, remote_ip, (unsigned int)remote_port);
  } else {
    TLOG_DEBUG("Failed to build STUN response");
    ice_tracef("handle_stun_request build_response_failed from=%s:%u to=%s:%u", local_cand->ip,
               (unsigned int)local_cand->port, remote_ip, (unsigned int)remote_port);
  }


  /* An authenticated inbound request discovers a peer-reflexive candidate, but
   * the pair becomes valid only after our triggered check succeeds. */
  ice_candidate_pair_t *pair = ensure_peer_reflexive_pair(agent, local_cand, remote_ip,
                                                          remote_port, priority);
  if (!pair)
    return;

  if (pair->state == ICE_PAIR_STATE_SUCCEEDED) {
    if (use_candidate && agent->role == ICE_ROLE_CONTROLLED) {
      pair->nominated = 1;
      agent->selected_pair = pair;
      set_state(agent, ICE_STATE_COMPLETED);
    }
    return;
  }

  if (agent->checks_in_progress && agent->current_check_pair >= 0 &&
      agent->current_check_pair < agent->pair_count &&
      &agent->pairs[agent->current_check_pair] == pair) {
    if (use_candidate && agent->role == ICE_ROLE_CONTROLLED)
      agent->current_check_select_on_success = 1;
    return;
  }
  pair->state = ICE_PAIR_STATE_WAITING;
  if (enqueue_triggered_check(agent, pair->local, pair->remote,
                              use_candidate && agent->role == ICE_ROLE_CONTROLLED) != 0) {
    pair->state = ICE_PAIR_STATE_FAILED;
  }
}

static int stun_response_source_matches(const ice_candidate_pair_t *pair,
                                        const struct sockaddr *from,
                                        const char *peer_ip_override,
                                        uint16_t peer_port_override) {
  if (!pair || !pair->remote)
    return 0;

  if (from && from->sa_family == AF_INET) {
    const struct sockaddr_in *addr4 = (const struct sockaddr_in *)from;
    struct in_addr expected_addr4;
    if (ntohs(addr4->sin_port) != pair->remote->port ||
        inet_pton(AF_INET, pair->remote->ip, &expected_addr4) != 1)
      return 0;
    return memcmp(&addr4->sin_addr, &expected_addr4, sizeof(expected_addr4)) == 0;
  } else if (from && from->sa_family == AF_INET6) {
    const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)from;
    struct in6_addr expected_addr6;
    if (ntohs(addr6->sin6_port) != pair->remote->port ||
        inet_pton(AF_INET6, pair->remote->ip, &expected_addr6) != 1)
      return 0;
    return memcmp(&addr6->sin6_addr, &expected_addr6, sizeof(expected_addr6)) == 0;
  } else if (peer_ip_override && peer_ip_override[0] != '\0') {
    struct in_addr peer_addr4;
    struct in_addr expected_addr4;
    struct in6_addr peer_addr6;
    struct in6_addr expected_addr6;

    if (peer_port_override != pair->remote->port)
      return 0;
    if (inet_pton(AF_INET, peer_ip_override, &peer_addr4) == 1 &&
        inet_pton(AF_INET, pair->remote->ip, &expected_addr4) == 1)
      return memcmp(&peer_addr4, &expected_addr4, sizeof(expected_addr4)) == 0;
    if (inet_pton(AF_INET6, peer_ip_override, &peer_addr6) == 1 &&
        inet_pton(AF_INET6, pair->remote->ip, &expected_addr6) == 1)
      return memcmp(&peer_addr6, &expected_addr6, sizeof(expected_addr6)) == 0;
    return 0;
  } else {
    return 0;
  }
}

static int find_consent_transaction(const salts_ice_agent_t *agent, const uint8_t *data) {
  if (!agent || !data)
    return -1;

  for (size_t i = 0; i < ICE_CONSENT_TRANSACTION_CAPACITY; ++i) {
    if (agent->consent_txn_sent_ms[i] != 0 &&
        memcmp(data + 8, agent->consent_txn_ids[i].id, STUN_TRANSACTION_ID_LEN) == 0)
      return (int)i;
  }

  return -1;
}

static void handle_stun_response(salts_ice_agent_t *agent, const uint8_t *data, size_t len,
                                 const struct sockaddr *from, const char *peer_ip_override,
                                 uint16_t peer_port_override) {
  int consent_txn_index;

  if (!agent || !data || len < STUN_HEADER_SIZE || !stun_is_stun_message(data, len))
    return;

  consent_txn_index = find_consent_transaction(agent, data);
  if (consent_txn_index >= 0) {
    if (!stun_response_source_matches(agent->selected_pair, from, peer_ip_override,
                                      peer_port_override))
      return;
    if (stun_validate_message_integrity(data, len, agent->remote_pwd) != 0 ||
        stun_get_error_code(data, len) != 0)
      return;

    memset(&agent->consent_txn_ids[consent_txn_index], 0,
           sizeof(agent->consent_txn_ids[consent_txn_index]));
    agent->consent_txn_sent_ms[consent_txn_index] = 0;
    agent->last_consent_response_ms = cmeta_monotonic_ms();
    return;
  }

  /* Verify transaction ID matches our current check */
  if (memcmp(data + 8, agent->current_txn_id.id, STUN_TRANSACTION_ID_LEN) != 0) {
    if (agent->selected_pair || agent->state != ICE_STATE_CONNECTING) {
      ice_tracef("handle_stun_response ignored_untracked_response state=%d selected=%p",
                 (int)agent->state, (void *)agent->selected_pair);
    } else {
      TLOG_WARN("STUN txn mismatch while waiting for response");
    }
    return; /* Not our transaction */
  }

  if (agent->current_check_pair < 0 || agent->current_check_pair >= agent->pair_count ||
      !stun_response_source_matches(&agent->pairs[agent->current_check_pair], from,
                                    peer_ip_override, peer_port_override)) {
    return;
  }

  /* Validate MESSAGE-INTEGRITY with the same peer password used to sign the
   * original connectivity check request. */
  if (stun_validate_message_integrity(data, len, agent->remote_pwd) != 0) {
    TLOG_WARN("STUN response integrity validation failed");
    if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count)
      agent->pairs[agent->current_check_pair].state = ICE_PAIR_STATE_FAILED;
    agent->checks_in_progress = 0;
    agent->current_check_nominating = 0;
    agent->current_check_select_on_success = 0;
    return; /* Invalid authentication */
  }

  /* Check for error response */
  int error_code = stun_get_error_code(data, len);
  if (error_code == STUN_ERROR_ROLE_CONFLICT) {
    TLOG_INFO("Role conflict detected, switching roles");
    /* Role conflict - switch roles */
    if (agent->role == ICE_ROLE_CONTROLLING) {
      agent->role = ICE_ROLE_CONTROLLED;
    } else {
      agent->role = ICE_ROLE_CONTROLLING;
    }
    /* Regenerate tie-breaker and restart checks */
    if (generate_random_u64(&agent->tie_breaker) != 0) {
      set_state(agent, ICE_STATE_FAILED);
      return;
    }
    agent->checks_in_progress = 0; /* Reset state so timer can restart */
    agent->current_check_nominating = 0;
    agent->current_check_select_on_success = 0;
    return;
  }

  if (error_code != 0) {
    TLOG_DEBUGF("STUN error response: {}", error_code);
    /* Other error - mark current pair as failed */
    if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count) {
      agent->pairs[agent->current_check_pair].state = ICE_PAIR_STATE_FAILED;
    }
    agent->checks_in_progress = 0;
    agent->current_check_nominating = 0;
    agent->current_check_select_on_success = 0;
    return;
  }

  /* Success! Mark the pair as succeeded */
  if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count) {
    ice_candidate_pair_t *pair = &agent->pairs[agent->current_check_pair];

    if (pair->state != ICE_PAIR_STATE_SUCCEEDED) {
      TLOG_DEBUGF("ICE check succeeded pair={} {}:{} <-> {}:{}",
                 agent->current_check_pair, pair->local->ip, pair->local->port,
                 pair->remote->ip, pair->remote->port);
      pair->state = ICE_PAIR_STATE_SUCCEEDED;
      agent->valid_pairs_count++;
    }

    /* Nomination is committed only after the corresponding outbound check
     * succeeds; an inbound request alone is not a valid-pair proof. */
    if ((agent->role == ICE_ROLE_CONTROLLING && agent->current_check_nominating) ||
        (agent->role == ICE_ROLE_CONTROLLED && agent->current_check_select_on_success)) {
      pair->nominated = 1;
      agent->selected_pair = pair;
      set_state(agent, ICE_STATE_COMPLETED);
    }
  }

  agent->checks_in_progress = 0;
  agent->current_check_nominating = 0;
  agent->current_check_select_on_success = 0;
}

static int find_pair_index_by_candidates(salts_ice_agent_t *agent, ice_candidate_t *local,
                                         ice_candidate_t *remote) {
  if (!agent || !local || !remote) {
    return -1;
  }

  for (int i = 0; i < agent->pair_count; i++) {
    if (agent->pairs[i].local == local && agent->pairs[i].remote == remote) {
      return i;
    }
  }

  return -1;
}

static int ice_candidates_equivalent(const ice_candidate_t *a, const ice_candidate_t *b) {
  if (!a || !b) {
    return 0;
  }

  return a->type == b->type &&
         a->transport == b->transport &&
         a->component_id == b->component_id &&
         a->family == b->family &&
         a->port == b->port &&
         a->related_port == b->related_port &&
         strcmp(a->ip, b->ip) == 0 &&
         strcmp(a->related_ip, b->related_ip) == 0;
}

static int ice_pair_allowed_for_checklist(salts_ice_agent_t *agent, ice_candidate_t *local,
                                          ice_candidate_t *remote) {
  int local_is_loopback;
  int remote_is_loopback;

  if (!agent || !local || !remote) {
    return 0;
  }

  ice_candidate_runtime_t *local_runtime = ice_candidate_runtime(agent, local);
  if (!local_runtime || (!local_runtime->socket && !local_runtime->turn_client)) {
    return 0;
  }
  if (local->ip[0] == '\0' || remote->ip[0] == '\0') {
    return 0;
  }
  if (local->component_id != remote->component_id) {
    return 0;
  }
  if (local->transport != remote->transport) {
    return 0;
  }
  if (local->family != remote->family) {
    return 0;
  }

  local_is_loopback = ice_ip_is_loopback(local->ip);
  remote_is_loopback = ice_ip_is_loopback(remote->ip);

  if (local->type == ICE_CANDIDATE_TYPE_HOST &&
      remote->type == ICE_CANDIDATE_TYPE_HOST &&
      local_is_loopback != remote_is_loopback) {
    return 0;
  }

  return 1;
}

static void rebuild_candidate_pairs(salts_ice_agent_t *agent) {
  if (!agent)
    return;

  int new_pairs_added = 0;
  ice_candidate_t *selected_local = NULL;
  ice_candidate_t *selected_remote = NULL;
  ice_candidate_t *current_local = NULL;
  ice_candidate_t *current_remote = NULL;

  if (agent->selected_pair) {
    selected_local = agent->selected_pair->local;
    selected_remote = agent->selected_pair->remote;
  }
  if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count) {
    current_local = agent->pairs[agent->current_check_pair].local;
    current_remote = agent->pairs[agent->current_check_pair].remote;
  }

  if (agent->pair_count > 0) {
    int write_index = 0;

    for (int i = 0; i < agent->pair_count; i++) {
      ice_candidate_pair_t pair = agent->pairs[i];

      if (!ice_pair_allowed_for_checklist(agent, pair.local, pair.remote)) {
        continue;
      }

      if (write_index != i) {
        agent->pairs[write_index] = pair;
      }
      write_index++;
    }

    agent->pair_count = write_index;
  }

  for (int i = 0; i < agent->local_candidate_count; i++) {
    for (int j = 0; j < agent->remote_candidate_count; j++) {
      ice_candidate_t *local = &agent->local_candidates[i];
      ice_candidate_t *remote = &agent->remote_candidates[j];
      if (!ice_pair_allowed_for_checklist(agent, local, remote)) {
        continue;
      }
      /* Check if pair already exists */
      int exists = 0;
      for (int k = 0; k < agent->pair_count; k++) {
        if (ice_candidates_equivalent(agent->pairs[k].local, local) &&
            ice_candidates_equivalent(agent->pairs[k].remote, remote)) {
          exists = 1;
          break;
        }
      }

      if (!exists && agent->pair_count < ICE_MAX_CANDIDATE_PAIRS) {
        ice_candidate_pair_t *pair = &agent->pairs[agent->pair_count];
        pair->local = local;
        pair->remote = remote;
        pair->state = ICE_PAIR_STATE_WAITING; /* Unfreeze immediately for simplicity */
        pair->priority = calculate_pair_priority(local->priority, remote->priority,
                                                 agent->role == ICE_ROLE_CONTROLLING);
        pair->nominated = 0;
        pair->check_count = 0;
        pair->last_check_time = 0;
        agent->pair_count++;
        new_pairs_added++;
      }
    }
  }

  if (new_pairs_added > 0) {
    TLOG_DEBUGF("Added {} new candidate pairs (total: {})", new_pairs_added,
              agent->pair_count);
    /* Sort pairs by priority (descending) - simple bubble sort */
    for (int i = 0; i < agent->pair_count - 1; i++) {
      for (int j = 0; j < agent->pair_count - i - 1; j++) {
        if (agent->pairs[j].priority < agent->pairs[j + 1].priority) {
          ice_candidate_pair_t tmp = agent->pairs[j];
          agent->pairs[j] = agent->pairs[j + 1];
          agent->pairs[j + 1] = tmp;
        }
      }
    }

    for (int i = 0; i < agent->pair_count && i < 5; i++) {
      ice_candidate_pair_t *pair = &agent->pairs[i];
      ice_tracef("rebuild_candidate_pairs top[%d] local=%s:%u remote=%s:%u prio=%llu state=%d",
                 i, pair->local->ip, (unsigned int)pair->local->port,
                 pair->remote->ip, (unsigned int)pair->remote->port,
                 (unsigned long long)pair->priority, (int)pair->state);
    }

    if (selected_local && selected_remote) {
      int selected_index = find_pair_index_by_candidates(agent, selected_local, selected_remote);
      agent->selected_pair = selected_index >= 0 ? &agent->pairs[selected_index] : NULL;
    }
    if (current_local && current_remote) {
      int current_index = find_pair_index_by_candidates(agent, current_local, current_remote);
      if (current_index >= 0) {
        agent->current_check_pair = current_index;
      } else {
        agent->current_check_pair = -1;
        agent->checks_in_progress = 0;
      }
    }
  }
}

static void service_udp_candidate_socket(salts_ice_agent_t *agent, ice_candidate_t *local_cand,
                                         uint64_t timeout_ms, int allow_data) {
  ice_candidate_runtime_t *runtime;
  ice_cnet_datagram_t *client;
  uint64_t recv_timeout_ms;
  int drained = 0;

  if (!agent || !local_cand) {
    return;
  }
  runtime = ice_candidate_runtime(agent, local_cand);
  if (!runtime || !runtime->socket || runtime->io_active) {
    return;
  }
  runtime->io_active = 1;

  client = (ice_cnet_datagram_t *)runtime->socket;
  recv_timeout_ms = timeout_ms;

  for (;;) {
    uint8_t data[CNET_DATAGRAM_MAX_PAYLOAD_BYTES];
    size_t data_len = 0;
    cnet_datagram_peer from;
    char from_ip[64] = {0};
    uint16_t from_port = 0u;
    int rc;

    memset(&from, 0, sizeof(from));
    if (ice_agent_wait_begin(agent, client, ice_wake_datagram) != 0) {
      rc = SALTS_ECANCELED;
    } else {
      rc = ice_cnet_datagram_receive(client, &from, data, sizeof(data), &data_len,
                                     recv_timeout_ms > UINT32_MAX
                                         ? UINT32_MAX
                                         : (uint32_t)recv_timeout_ms);
      ice_agent_wait_end(agent, client);
    }
    if (rc != 0 || data_len == 0) {
      ice_tracef("service_udp_candidate_socket recv rc=%d data_len=%zu local=%s:%u", rc, data_len,
                 local_cand->ip, (unsigned int)local_cand->port);
      break;
    }

    if (ice_cnet_datagram_peer_to_text(&from, from_ip, sizeof(from_ip), &from_port) != SALTS_OK)
      break;
    ice_tracef("service_udp_candidate_socket recv rc=0 data_len=%zu from=%s:%u local=%s:%u",
               data_len, from_ip, (unsigned int)from_port, local_cand->ip,
               (unsigned int)local_cand->port);

    if (stun_is_stun_message((const uint8_t *)data, data_len)) {
      uint16_t msg_type = read_u16_be((const uint8_t *)data);
      if (msg_type == STUN_MSG_BINDING_RESPONSE || msg_type == STUN_MSG_BINDING_ERROR_RESPONSE) {
        handle_stun_response(agent, data, data_len, NULL, from_ip, from_port);
      } else if (msg_type == STUN_MSG_BINDING_REQUEST) {
        handle_stun_request(agent, data, data_len, NULL, from_ip, from_port, local_cand);
      }
    } else if (allow_data && agent->callbacks.on_data) {
      agent->callbacks.on_data(agent, data, data_len, agent->callbacks.user_data);
    }

    if (!allow_data && agent->state != ICE_STATE_CONNECTING) {
      ice_tracef("service_udp_candidate_socket stop_after_state_change state=%d local=%s:%u",
                 (int)agent->state, local_cand->ip, (unsigned int)local_cand->port);
      break;
    }

    drained++;
    if (drained >= 8) {
      break;
    }

    recv_timeout_ms = 1;
  }

  runtime->io_active = 0;
}

static void service_turn_candidate_socket(salts_ice_agent_t *agent, ice_candidate_t *local_cand,
                                          uint64_t timeout_ms, int allow_data) {
  ice_candidate_runtime_t *runtime;
  salts_turn_client_t *turn;
  char peer_ip[64] = {0};
  uint16_t peer_port = 0;
  void *buf = NULL;
  const uint8_t *payload = NULL;
  size_t payload_len = 0;
  int rc;

  if (!agent || !local_cand) {
    return;
  }
  runtime = ice_candidate_runtime(agent, local_cand);
  if (!runtime || !runtime->turn_client || runtime->io_active) {
    return;
  }
  runtime->io_active = 1;

  turn = runtime->turn_client;
  if (ice_agent_wait_begin(agent, turn, ice_wake_turn) != 0) {
    rc = SALTS_ECANCELED;
  } else {
    rc = turn_client_recv_timeout(
        turn, timeout_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)timeout_ms,
        peer_ip, &peer_port, &buf, &payload, &payload_len);
    ice_agent_wait_end(agent, turn);
  }
  if (rc != 0 || !payload || payload_len == 0) {
    ice_tracef("service_turn_candidate_socket recv rc=%d payload_len=%zu local=%s:%u", rc,
               payload_len, local_cand->ip, (unsigned int)local_cand->port);
    if (buf) {
      turn_client_free_recv(buf);
    }
    runtime->io_active = 0;
    return;
  }

  ice_tracef("service_turn_candidate_socket recv rc=0 payload_len=%zu peer=%s:%u local=%s:%u",
             payload_len, peer_ip, (unsigned int)peer_port, local_cand->ip,
             (unsigned int)local_cand->port);

  if (stun_is_stun_message(payload, payload_len)) {
    uint16_t msg_type = read_u16_be(payload);
    ice_tracef("service_turn_candidate_socket stun msg_type=0x%04x peer=%s:%u local=%s:%u",
               (unsigned int)msg_type, peer_ip, (unsigned int)peer_port, local_cand->ip,
               (unsigned int)local_cand->port);
    if (msg_type == STUN_MSG_BINDING_RESPONSE || msg_type == STUN_MSG_BINDING_ERROR_RESPONSE) {
      handle_stun_response(agent, payload, payload_len, NULL, peer_ip, peer_port);
    } else if (msg_type == STUN_MSG_BINDING_REQUEST) {
      handle_stun_request(agent, payload, payload_len, NULL, peer_ip, peer_port, local_cand);
    }
  } else if (allow_data && agent->callbacks.on_data) {
    ice_tracef("service_turn_candidate_socket app_data payload_len=%zu peer=%s:%u local=%s:%u",
               payload_len, peer_ip, (unsigned int)peer_port, local_cand->ip,
               (unsigned int)local_cand->port);
    agent->callbacks.on_data(agent, payload, payload_len, agent->callbacks.user_data);
  } else {
    ice_tracef("service_turn_candidate_socket non_stun payload_len=%zu peer=%s:%u local=%s:%u",
               payload_len, peer_ip, (unsigned int)peer_port, local_cand->ip,
               (unsigned int)local_cand->port);
  }

  turn_client_free_recv(buf);

  if (!allow_data && agent->state != ICE_STATE_CONNECTING) {
    ice_tracef("service_turn_candidate_socket stop_after_state_change state=%d local=%s:%u",
               (int)agent->state, local_cand->ip, (unsigned int)local_cand->port);
    runtime->io_active = 0;
    return;
  }

  runtime->io_active = 0;
}

static int schedule_next_consent_check(salts_ice_agent_t *agent, uint64_t now) {
  uint8_t random_byte;
  uint64_t base_interval;
  uint64_t minimum_interval;
  uint64_t jitter_span;

  if (!agent || cmeta_secure_random(&random_byte, sizeof(random_byte)) != 0)
    return -1;

  base_interval = (uint64_t)agent->config.keepalive_interval_ms;
  minimum_interval = (base_interval * 4u) / 5u;
  jitter_span = (base_interval * 2u) / 5u;
  agent->next_consent_check_ms =
      now + minimum_interval + ((jitter_span * random_byte) / UINT8_MAX);
  return 0;
}

static int send_consent_check(salts_ice_agent_t *agent, uint64_t now) {
  ice_candidate_pair_t *pair;
  stun_transaction_id_t txn_id;
  uint8_t stun_buf[STUN_MAX_MESSAGE_SIZE];
  uint32_t peer_reflexive_priority;
  int local_preference;
  int stun_len;
  int send_rc = -1;

  if (!agent || !(pair = agent->selected_pair) || !pair->local || !pair->remote)
    return -1;
  if (stun_generate_transaction_id(&txn_id) != 0)
    return -1;

  local_preference = (int)((pair->local->priority >> 8) & 0xffffu);
  peer_reflexive_priority = ice_calculate_priority(
      ICE_CANDIDATE_TYPE_PRFLX, local_preference, pair->local->component_id);
  stun_len = stun_build_ice_request(stun_buf, &txn_id, agent->local_ufrag,
                                    agent->remote_ufrag, agent->remote_pwd,
                                    peer_reflexive_priority,
                                    agent->role == ICE_ROLE_CONTROLLING,
                                    agent->tie_breaker, 0);
  if (stun_len < 0)
    return -1;

  ice_candidate_runtime_t *local_runtime = ice_candidate_runtime(agent, pair->local);
  if (pair->local->type == ICE_CANDIDATE_TYPE_RELAY && local_runtime &&
      local_runtime->turn_client) {
    send_rc = turn_client_send(local_runtime->turn_client, pair->remote->ip,
                               pair->remote->port, stun_buf, (size_t)stun_len);
  } else if (local_runtime && local_runtime->socket) {
    send_rc = send_udp_to_remote((ice_cnet_datagram_t *)local_runtime->socket,
                                 pair->remote->ip, pair->remote->port,
                                 stun_buf, (size_t)stun_len);
  }

  if (send_rc == 0) {
    size_t slot = agent->consent_txn_next;
    agent->consent_txn_ids[slot] = txn_id;
    agent->consent_txn_sent_ms[slot] = now;
    agent->consent_txn_next = (slot + 1u) % ICE_CONSENT_TRANSACTION_CAPACITY;
  }
  agent->last_keepalive_ms = now;
  return schedule_next_consent_check(agent, now);
}

static void service_selected_pair_once(salts_ice_agent_t *agent, uint64_t timeout_ms) {
  uint64_t now;
  if (!agent || (agent->state != ICE_STATE_CONNECTED && agent->state != ICE_STATE_COMPLETED) ||
      !agent->selected_pair || !agent->selected_pair->local) {
    return;
  }
  now = cmeta_monotonic_ms();
  if (agent->last_consent_response_ms == 0 ||
      now - agent->last_consent_response_ms >= ICE_CONSENT_EXPIRY_MS) {
    set_state(agent, ICE_STATE_DISCONNECTED);
    return;
  }
  if (agent->next_consent_check_ms == 0) {
    if (schedule_next_consent_check(agent, now) != 0) {
      set_state(agent, ICE_STATE_DISCONNECTED);
      return;
    }
  } else if (now >= agent->next_consent_check_ms && send_consent_check(agent, now) != 0) {
    set_state(agent, ICE_STATE_DISCONNECTED);
    return;
  }
  if (agent->selected_pair->local->type == ICE_CANDIDATE_TYPE_RELAY) {
    service_turn_candidate_socket(agent, agent->selected_pair->local, timeout_ms, 1);
  } else {
    service_udp_candidate_socket(agent, agent->selected_pair->local, timeout_ms, 1);
  }
}

static void service_connectivity_check_io(salts_ice_agent_t *agent, uint64_t timeout_ms) {
  if (!agent) {
    return;
  }

  for (int i = 0; i < agent->local_candidate_count; ++i) {
    ice_candidate_t *local = &agent->local_candidates[i];

    ice_candidate_runtime_t *runtime = ice_candidate_runtime(agent, local);
    if (local->type == ICE_CANDIDATE_TYPE_RELAY) {
      if (runtime && runtime->turn_client) {
        service_turn_candidate_socket(agent, local, timeout_ms, 0);
        if (agent->state != ICE_STATE_CONNECTING) {
          return;
        }
      }
      continue;
    }

    if (runtime && runtime->socket) {
      service_udp_candidate_socket(agent, local, timeout_ms, 0);
      if (agent->state != ICE_STATE_CONNECTING) {
        return;
      }
    }
  }
}

static void run_connectivity_checks(salts_ice_agent_t *agent) {
  if (agent->state != ICE_STATE_CONNECTING)
    return;

  while (agent->state == ICE_STATE_CONNECTING) {
    uint64_t now = cmeta_monotonic_ms();
    uint64_t elapsed = now - agent->check_start_time;

    /* Connectivity checks need inbound STUN pumping before a pair can advance. */
    service_connectivity_check_io(agent, 1);
    if (agent->state != ICE_STATE_CONNECTING) {
      break;
    }

    /* Check for overall timeout */
    if (elapsed > (uint64_t)agent->config.connectivity_timeout_ms) {
      TLOG_INFO("Connectivity check timeout elapsed");
      set_state(agent, agent->selected_pair ? ICE_STATE_COMPLETED : ICE_STATE_FAILED);
      break;
    }

    /* If a check is in progress, wait for response or per-check timeout */
    if (agent->checks_in_progress) {
      if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count) {
        ice_candidate_pair_t *pair = &agent->pairs[agent->current_check_pair];

        /* Per-check timeout (3 seconds) */
        if (pair->last_check_time > 0 && now - pair->last_check_time > 3000) {
          if (pair->check_count < 3) {
            if (send_connectivity_check(agent, pair, agent->current_check_nominating) != 0)
              agent->checks_in_progress = 0;
          } else {
            pair->state = ICE_PAIR_STATE_FAILED;
            agent->checks_in_progress = 0;
            agent->current_check_nominating = 0;
            agent->current_check_select_on_success = 0;
          }
        }
      }
      cmeta_sleep_ms(ICE_DEFAULT_TA_INTERVAL);
      continue;
    }

    int found = 0;
    if (agent->role == ICE_ROLE_CONTROLLING && agent->valid_pairs_count > 0 &&
        !agent->selected_pair) {
      for (int i = 0; i < agent->pair_count; i++) {
        if (agent->pairs[i].state == ICE_PAIR_STATE_SUCCEEDED && !agent->pairs[i].nominated) {
          agent->current_check_pair = i;
          agent->checks_in_progress = 1;
          agent->current_check_select_on_success = 0;
          if (send_connectivity_check(agent, &agent->pairs[i], 1) == 0) {
            agent->nomination_started = 1;
            found = 1;
          } else {
            agent->checks_in_progress = 0;
          }
          break;
        }
      }
      if (found) {
        cmeta_sleep_ms(ICE_DEFAULT_TA_INTERVAL);
        continue;
      }
    }

    {
      int triggered_pair_index = -1;
      int received_nomination = 0;
      if (dequeue_triggered_check(agent, &triggered_pair_index, &received_nomination)) {
        ice_candidate_pair_t *pair = &agent->pairs[triggered_pair_index];
        int nominate = agent->role == ICE_ROLE_CONTROLLING && agent->config.aggressive_nomination;
        agent->current_check_pair = triggered_pair_index;
        agent->checks_in_progress = 1;
        agent->current_check_select_on_success = 0;
        if (send_connectivity_check(agent, pair, nominate) == 0) {
          agent->current_check_select_on_success =
              agent->role == ICE_ROLE_CONTROLLED && received_nomination;
          found = 1;
        } else {
          agent->checks_in_progress = 0;
        }
      }
    }
    if (found) {
      cmeta_sleep_ms(ICE_DEFAULT_TA_INTERVAL);
      continue;
    }

    /* Find next pair to check */
    found = 0;
    for (int i = 0; i < agent->pair_count; i++) {
      if (found) {
        break;
      }
      ice_candidate_pair_t *pair = &agent->pairs[i];
      if (pair->state == ICE_PAIR_STATE_WAITING) {
        agent->current_check_pair = i;
        agent->checks_in_progress = 1;
        agent->current_check_select_on_success = 0;
        int nominate = agent->role == ICE_ROLE_CONTROLLING && agent->config.aggressive_nomination;
        if (send_connectivity_check(agent, pair, nominate) == 0) {
          found = 1;
        } else {
          agent->checks_in_progress = 0;
        }
        break;
      }
    }
    if (!found) {
      /* All pairs checked — determine final state */
      if (agent->selected_pair) {
        set_state(agent, ICE_STATE_COMPLETED);
        break;
      }

      /* Check if all pairs failed */
      if (agent->pair_count > 0) {
        int all_failed = 1;
        for (int i = 0; i < agent->pair_count; i++) {
          if (agent->pairs[i].state != ICE_PAIR_STATE_FAILED) {
            all_failed = 0;
            break;
          }
        }
        if (all_failed) {
          TLOG_INFOF("All {} built pairs failed", agent->pair_count);
          set_state(agent, ICE_STATE_FAILED);
          break;
        }
      }
    }

    cmeta_sleep_ms(ICE_DEFAULT_TA_INTERVAL);
  }
  agent->checks_in_progress = 0;
  agent->current_check_pair = -1;
  agent->current_check_nominating = 0;
  agent->current_check_select_on_success = 0;
}


int ice_agent_start_checks(salts_ice_agent_t *agent) {
  if (!agent)
    return -1;
  ice_agent_owner_enter(agent);
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }

  if (agent->gathering_state != ICE_GATHERING_COMPLETE) {
    ice_agent_owner_leave(agent);
    return -2;
  }

  if (!agent->remote_credentials_set) {
    TLOG_INFO("Cannot start checks: remote credentials not set");
    ice_agent_owner_leave(agent);
    return -3;
  }

  if (agent->state == ICE_STATE_CONNECTING || agent->state == ICE_STATE_CONNECTED ||
      agent->state == ICE_STATE_COMPLETED) {
    /* Already started, just rebuild pairs to catch any newly added trickle candidates */
    ice_tracef("ice_agent_start_checks rebuild_existing state=%d local=%d remote=%d",
               (int)agent->state, agent->local_candidate_count, agent->remote_candidate_count);
    rebuild_candidate_pairs(agent);
    ice_agent_owner_leave(agent);
    return 0;
  }

  TLOG_INFOF("Starting checks with {} local and {} remote candidates",
           agent->local_candidate_count, agent->remote_candidate_count);
  ice_tracef("ice_agent_start_checks begin local=%d remote=%d", agent->local_candidate_count,
             agent->remote_candidate_count);

  /* Build initial candidate pairs */
  agent->pair_count = 0;
  rebuild_candidate_pairs(agent);
  ice_tracef("ice_agent_start_checks built pair_count=%d", agent->pair_count);

  if (agent->pair_count == 0) {
    TLOG_INFO("Starting CONNECTIVITY CHECKS with 0 pairs (waiting for remote candidates)");
  } else {
    TLOG_INFOF("Built {} candidate pairs", agent->pair_count);
  }

  /* Initialize check state */
  agent->current_check_pair = -1;
  agent->checks_in_progress = 0;
  agent->valid_pairs_count = 0;
  agent->nomination_started = 0;
  agent->check_start_time = cmeta_monotonic_ms();

  set_state(agent, ICE_STATE_CONNECTING);
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }
  ice_tracef("ice_agent_start_checks entering_connectivity_loop");

  /* Run connectivity checks synchronously on the current owner thread. */
  run_connectivity_checks(agent);
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }
  ice_tracef("ice_agent_start_checks connectivity_loop_done state=%d selected=%p", (int)agent->state,
             (void *)agent->selected_pair);

  ice_agent_owner_leave(agent);
  return 0;
}

void ice_agent_poll_selected_pair(salts_ice_agent_t *agent, uint64_t timeout_ms) {
  if (!agent) {
    return;
  }

  ice_agent_owner_enter(agent);
  if (ice_agent_is_closed(agent) ||
      (agent->state != ICE_STATE_CONNECTED && agent->state != ICE_STATE_COMPLETED)) {
    ice_agent_owner_leave(agent);
    return;
  }

  if (!agent->selected_pair || !agent->selected_pair->local) {
    ice_agent_owner_leave(agent);
    return;
  }

  if (timeout_ms == 0) {
    timeout_ms = 1;
  }

  service_selected_pair_once(agent, timeout_ms);
  (void)ice_agent_is_closed(agent);
  ice_agent_owner_leave(agent);
}


int ice_agent_send(salts_ice_agent_t *agent, const void *data, size_t len) {
  int rc;

  if (!agent)
    return -1;
  ice_agent_owner_enter(agent);
  if (ice_agent_is_closed(agent)) {
    ice_agent_owner_leave(agent);
    return ICE_AGENT_ERROR_CLOSED;
  }
  if (!data || len == 0) {
    ice_agent_owner_leave(agent);
    return -1;
  }

  if (agent->state != ICE_STATE_CONNECTED && agent->state != ICE_STATE_COMPLETED) {
    ice_agent_owner_leave(agent);
    return -2;
  }

  if (!agent->selected_pair) {
    ice_agent_owner_leave(agent);
    return -3;
  }

  ice_candidate_t *local = agent->selected_pair->local;
  ice_candidate_t *remote = agent->selected_pair->remote;
  ice_candidate_runtime_t *runtime = ice_candidate_runtime(agent, local);

  if (local->type == ICE_CANDIDATE_TYPE_HOST || local->type == ICE_CANDIDATE_TYPE_SRFLX) {
    if (!runtime || !runtime->socket) {
      ice_agent_owner_leave(agent);
      return -4;
    }
    ice_cnet_datagram_t *client = (ice_cnet_datagram_t *)runtime->socket;
    rc = send_udp_to_remote(client, remote->ip, remote->port, data, len);
  } else if (local->type == ICE_CANDIDATE_TYPE_RELAY) {
    if (!runtime || !runtime->turn_client) {
      ice_agent_owner_leave(agent);
      return -5;
    }
    rc = turn_client_send(runtime->turn_client, remote->ip, remote->port, data, len);
  } else {
    ice_agent_owner_leave(agent);
    return -6;
  }

  rc = ice_agent_is_closed(agent) ? ICE_AGENT_ERROR_CLOSED : rc;
  ice_agent_owner_leave(agent);
  return rc;
}


ice_state_t ice_agent_get_state(salts_ice_agent_t *agent) {
  if (!agent) return ICE_STATE_CLOSED;
  return agent->state;
}

ice_gathering_state_t ice_agent_get_gathering_state(salts_ice_agent_t *agent) {
  if (!agent) return ICE_GATHERING_NEW;
  return agent->gathering_state;
}

int ice_agent_get_selected_pair(salts_ice_agent_t *agent, ice_candidate_t *local_out,
                                ice_candidate_t *remote_out) {
  if (!agent || !agent->selected_pair)
    return -1;

  if (local_out)
    *local_out = *agent->selected_pair->local;
  if (remote_out)
    *remote_out = *agent->selected_pair->remote;

  return 0;
}

int ice_agent_get_local_candidate_count(salts_ice_agent_t *agent) {
  if (!agent) return 0;
  return agent->local_candidate_count;
}

int ice_agent_get_local_candidate(salts_ice_agent_t *agent, int index, ice_candidate_t *out) {
  if (!agent || !out)
    return -1;
  if (index < 0 || index >= agent->local_candidate_count)
    return -2;

  *out = agent->local_candidates[index];
  return 0;
}

void ice_agent_set_allow_loopback(salts_ice_agent_t *agent, int allow) {
  if (agent) {
    ice_agent_owner_enter(agent);
    if (ice_agent_is_closed(agent)) {
      ice_agent_owner_leave(agent);
      return;
    }
    agent->config.allow_loopback = allow;
    ice_agent_owner_leave(agent);
  }
}

void ice_agent_close(salts_ice_agent_t *agent) {
  ice_progress_owner_t *progress;
  int owner_active;
  int current_is_owner;
  int finish_here = 0;
  if (!agent) return;
  progress = ice_agent_progress(agent);
  if (!progress) return;
  cmeta_mutex_lock(&progress->mutex);
  atomic_store_explicit(&progress->close_requested, 1, memory_order_release);
  owner_active = progress->owner_active;
  current_is_owner = ice_progress_current_thread_is_owner(progress);
  if (!owner_active) {
    ice_progress_set_current_thread_owner(progress);
    current_is_owner = 1;
    finish_here = 1;
  }
  if (owner_active && !current_is_owner && progress->wait_wake &&
      progress->wait_target) {
    (void)progress->wait_wake(progress->wait_target);
  }
  cmeta_mutex_unlock(&progress->mutex);
  if (current_is_owner) ice_agent_finish_close(agent);
  if (finish_here) {
    cmeta_mutex_lock(&progress->mutex);
    progress->owner_active = 0;
    progress->owner_depth = 0u;
    cmeta_mutex_unlock(&progress->mutex);
  }
}

/* ============================================================================
 * Candidate Parsing/Formatting
 * ============================================================================ */

int ice_candidate_parse(const char *sdp_str, ice_candidate_t *candidate) {
  if (!sdp_str || !candidate)
    return -1;

  memset(candidate, 0, sizeof(*candidate));

  /* Format: candidate:foundation component transport priority address port typ type [raddr rport]
   */
  /* Example: candidate:1 1 UDP 2130706431 192.168.1.1 54321 typ host */

  const char *p = sdp_str;

  /* Skip "candidate:" prefix if present */
  if (strncmp(p, "candidate:", 10) == 0)
    p += 10;
  else if (strncmp(p, "a=candidate:", 12) == 0)
    p += 12;

  /* Parse foundation */
  char foundation[ICE_CANDIDATE_FOUNDATION_LEN + 1] = {0};
  int n = 0;
  while (*p && !isspace(*p) && n < ICE_CANDIDATE_FOUNDATION_LEN) {
    foundation[n++] = *p++;
  }
  strncpy(candidate->foundation, foundation, sizeof(candidate->foundation) - 1);
  while (*p && isspace(*p))
    p++;

  /* Parse component */
  candidate->component_id = (uint8_t)atoi(p);
  while (*p && !isspace(*p))
    p++;
  while (*p && isspace(*p))
    p++;

  /* Parse transport */
  if (tstr_ncasecmp(p, "UDP", 3) == 0) {
    candidate->transport = ICE_TRANSPORT_UDP;
  } else if (tstr_ncasecmp(p, "TCP", 3) == 0) {
    candidate->transport = ICE_TRANSPORT_TCP;
  }
  while (*p && !isspace(*p))
    p++;
  while (*p && isspace(*p))
    p++;

  /* Parse priority */
  candidate->priority = (uint32_t)strtoul(p, NULL, 10);
  while (*p && !isspace(*p))
    p++;
  while (*p && isspace(*p))
    p++;

  /* Parse IP address or mDNS hostname */
  char address[64] = {0};
  n = 0;
  while (*p && !isspace(*p) && n < (int)sizeof(address) - 1) {
    address[n++] = *p++;
  }

  /* Check if this is an mDNS .local hostname */
  size_t addr_len = strlen(address);
  if (addr_len > 6 && strcmp(address + addr_len - 6, ".local") == 0) {
    /* Store mDNS hostname - IP will be resolved later */
    strncpy(candidate->mdns_name, address, sizeof(candidate->mdns_name) - 1);
    candidate->ip[0] = '\0';     /* IP unknown until resolved */
    candidate->family = AF_INET; /* Assume IPv4 for now */
  } else {
    strncpy(candidate->ip, address, sizeof(candidate->ip) - 1);
    candidate->family = strchr(candidate->ip, ':') ? AF_INET6 : AF_INET;
  }
  while (*p && isspace(*p))
    p++;

  /* Parse port */
  candidate->port = (uint16_t)atoi(p);
  while (*p && !isspace(*p))
    p++;
  while (*p && isspace(*p))
    p++;

  /* Parse "typ" keyword */
  if (strncmp(p, "typ", 3) == 0) {
    p += 3;
    while (*p && isspace(*p))
      p++;

    if (strncmp(p, "host", 4) == 0) {
      candidate->type = ICE_CANDIDATE_TYPE_HOST;
    } else if (strncmp(p, "srflx", 5) == 0) {
      candidate->type = ICE_CANDIDATE_TYPE_SRFLX;
    } else if (strncmp(p, "prflx", 5) == 0) {
      candidate->type = ICE_CANDIDATE_TYPE_PRFLX;
    } else if (strncmp(p, "relay", 5) == 0) {
      candidate->type = ICE_CANDIDATE_TYPE_RELAY;
    }
  }

  while (*p && !isspace(*p))
    p++;

  while (*p) {
    char key[16] = {0};
    char value[64] = {0};

    while (*p && isspace(*p))
      p++;
    if (!*p)
      break;

    n = 0;
    while (*p && !isspace(*p) && n < (int)sizeof(key) - 1) {
      key[n++] = *p++;
    }

    while (*p && isspace(*p))
      p++;

    n = 0;
    while (*p && !isspace(*p) && n < (int)sizeof(value) - 1) {
      value[n++] = *p++;
    }

    if (strcmp(key, "raddr") == 0) {
      strncpy(candidate->related_ip, value, sizeof(candidate->related_ip) - 1);
    } else if (strcmp(key, "rport") == 0) {
      candidate->related_port = (uint16_t)atoi(value);
    }
  }

  return 0;
}

int ice_candidate_to_sdp(const ice_candidate_t *candidate, char *buf, size_t buf_len) {
  if (!candidate || !buf || buf_len == 0)
    return -1;

  const char *type_str;
  switch (candidate->type) {
  case ICE_CANDIDATE_TYPE_HOST:
    type_str = "host";
    break;
  case ICE_CANDIDATE_TYPE_SRFLX:
    type_str = "srflx";
    break;
  case ICE_CANDIDATE_TYPE_PRFLX:
    type_str = "prflx";
    break;
  case ICE_CANDIDATE_TYPE_RELAY:
    type_str = "relay";
    break;
  default:
    type_str = "host";
  }

  const char *transport_str = candidate->transport == ICE_TRANSPORT_UDP ? "UDP" : "TCP";

  /* Use mDNS hostname for privacy if available (host candidates only) */
  const char *address = (candidate->mdns_name[0] != '\0') ? candidate->mdns_name : candidate->ip;

  int len = snprintf(buf, buf_len, "candidate:%s %u %s %u %s %u typ %s",
                     candidate->foundation, (unsigned int)candidate->component_id, transport_str,
                     candidate->priority, address, (unsigned int)candidate->port, type_str);

  /* Add raddr/rport if present */
  if (candidate->related_ip[0] != '\0' && (size_t)len < buf_len) {
    int extra = snprintf(buf + len, buf_len - (size_t)len, " raddr %s rport %u",
                         candidate->related_ip, (unsigned int)candidate->related_port);
    if (extra > 0) {
      len += extra;
    }
  }

  return len;
}

const char *ice_state_name(ice_state_t state) {
  const char *name = ice_state_t_to_string(state);
  return name ? name : "UNKNOWN";
}

const char *ice_gathering_state_name(ice_gathering_state_t state) {
  const char *name = ice_gathering_state_t_to_string(state);
  return name ? name : "UNKNOWN";
}

const char *ice_candidate_type_name(ice_candidate_type_t type) {
  const char *name = ice_candidate_type_t_to_string(type);
  return name ? name : "unknown";
}
