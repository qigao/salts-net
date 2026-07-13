/**
 * turbo_ice.c - ICE Agent Implementation (RFC 8445)
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
#include "ice/turbo_ice.h"
#include "ice/turbo_stun.h"
#include "ice/turbo_turn.h"
#include <platform.h>
#include "CoroNet/turbo_coro_socket.h"

#include "turbo_dns.h"
#include "turbo_str.h"
#include "tlog.h"
#include "turbo_mdns.h"
#include <fmt.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
  #include <iphlpapi.h>
  #include <process.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "iphlpapi.lib")
  #define tstr_ncasecmp _strnicmp
#else
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <netdb.h>
  #include <net/if.h>
  #include <netinet/in.h>
  #include <strings.h>
  #include <unistd.h>
#endif

/* Thread-safe random seeding using atomic CAS */
#include <stdatomic.h>

static atomic_int g_random_seeded = 0;

static void ice_tracef(const char *fmt, ...) {
  const char *path = getenv("TURBO_ICE_TRACE");
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

static void ensure_random_seeded(void) {
  if (atomic_load_explicit(&g_random_seeded, memory_order_acquire) == 0) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_random_seeded, &expected, 1)) {
      srand((unsigned int)time(NULL) ^ (unsigned int)turbo_getpid());
    }
  }
}

/* Generate 64-bit random value with better entropy.
 * Combines multiple rand() calls with time-based entropy to improve randomness.
 * Note: For cryptographic use, replace with OS-specific secure random. */
static uint64_t generate_random_u64(void) {
  ensure_random_seeded();
  uint64_t r = 0;
  /* Use 4 rand() calls to fill 64 bits (rand() typically gives 15-31 bits) */
  for (int i = 0; i < 4; i++) {
    r = (r << 16) ^ (uint64_t)rand();
  }
  /* Mix in additional entropy from high-resolution time */
  r ^= (uint64_t)time(NULL);
#ifdef _WIN32
  LARGE_INTEGER perf;
  QueryPerformanceCounter(&perf);
  r ^= (uint64_t)perf.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  r ^= (uint64_t)ts.tv_nsec;
#endif
  return r;
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
    TLOG_WARN("Failed to resolve mDNS candidate {}: {}", candidate->mdns_name, rc);
#else
    TLOG_WARN("Failed to resolve mDNS candidate {}: {}", candidate->mdns_name,
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
    TLOG_WARN("mDNS candidate {} resolved without a usable IP address", candidate->mdns_name);
    return -1;
  }

  TLOG_INFO("Resolved mDNS candidate {} -> {}", candidate->mdns_name, candidate->ip);
  return 0;
}

/* Generate UUID-style mDNS hostname for privacy (e.g.,
 * "a1b2c3d4-e5f6-7890-abcd-ef1234567890.local") */
static void generate_mdns_hostname(char *buf, size_t buf_len) {
  static const char hex[] = "0123456789abcdef";
  char uuid[37]; /* 36 chars + null */

  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      uuid[i] = '-';
    } else {
      uuid[i] = hex[rand() % 16];
    }
  }
  uuid[36] = '\0';

  fmt(buf, buf_len, "{}.local", uuid);
}

static inline uint16_t read_u16_be(const uint8_t *ptr) {
  return (uint16_t)((ptr[0] << 8) | ptr[1]);
}

static inline uint32_t read_u32_be(const uint8_t *ptr) {
  return (uint32_t)((ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | ptr[3]);
}

/* ============================================================================
 * Internal Structures
 * ============================================================================ */


struct turbo_ice_agent_s {
  ice_config_t config;

  /* Coroutine context */
  coro_context_t *ctx;

  /* State */
  ice_state_t state;
  ice_gathering_state_t gathering_state;
  ice_role_t role;
  uint64_t tie_breaker;

  /* Credentials */
  char local_ufrag[32];
  char local_pwd[64];
  char remote_ufrag[32];
  char remote_pwd[64];

  /* Candidates */
  ice_candidate_t local_candidates[ICE_MAX_CANDIDATES];
  int local_candidate_count;
  ice_candidate_t remote_candidates[ICE_MAX_CANDIDATES];
  int remote_candidate_count;

  /* Candidate pairs */
  ice_candidate_pair_t pairs[ICE_MAX_CANDIDATE_PAIRS];
  int pair_count;
  ice_candidate_pair_t *selected_pair;

  /* Connectivity check state */
  int current_check_pair;
  stun_transaction_id_t current_txn_id;
  int checks_in_progress;
  int valid_pairs_count;
  uint64_t check_start_time;

  /* STUN/TURN gathering (no longer client objects — use coro functions directly) */
  int pending_stun_requests;
  int pending_turn_requests;
  turbo_turn_client_t *turn_clients[ICE_MAX_TURN_SERVERS];

  /* mDNS context for privacy-preserving candidates */
  mdns_ctx_t *mdns_ctx;

  /* Foundation counter */
  int foundation_counter;

  /* Callbacks */
  ice_callbacks_t callbacks;

  /* Flags */
  int remote_credentials_set;
  int remote_candidates_complete;
  int nomination_started;
  int selected_pair_io_running;
};


/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static void generate_random_string(char *buf, size_t len) {
  static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  for (size_t i = 0; i < len - 1; i++) {
    buf[i] = charset[rand() % (sizeof(charset) - 1)];
  }
  buf[len - 1] = '\0';
}

static void generate_candidate_id(char *id) {
  generate_random_string(id, ICE_CANDIDATE_ID_LEN + 1);
}

static void rebuild_candidate_pairs(turbo_ice_agent_t *agent);
static int ice_candidates_equivalent(const ice_candidate_t *a, const ice_candidate_t *b);
static void selected_pair_io_task(coro_t *co, void *arg);
static void set_state(turbo_ice_agent_t *agent, ice_state_t new_state) {
  if (agent->state != new_state) {
    ice_state_t old_state = agent->state;
    agent->state = new_state;

    if ((new_state == ICE_STATE_CONNECTED || new_state == ICE_STATE_COMPLETED) &&
        agent->ctx && !agent->selected_pair_io_running) {
      agent->selected_pair_io_running = 1;
      if (coro_context_spawn(agent->ctx, selected_pair_io_task, agent) != 0) {
        agent->selected_pair_io_running = 0;
      }
    }

    if (agent->callbacks.on_state_change) {
      TLOG_INFO("State change: {} -> {}", ice_state_name(old_state), ice_state_name(new_state));
      agent->callbacks.on_state_change(agent, old_state, new_state, agent->callbacks.user_data);
    }
  }
}

static void set_gathering_state(turbo_ice_agent_t *agent, ice_gathering_state_t new_state) {
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

static int ice_pair_is_loopback(const ice_candidate_pair_t *pair) {
  return pair && pair->local && pair->remote &&
         ice_ip_is_loopback(pair->local->ip) &&
         ice_ip_is_loopback(pair->remote->ip);
}

/* Forward declarations for connectivity check functions */
static int send_connectivity_check(turbo_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                   int nominate);
static void handle_stun_request(turbo_ice_agent_t *agent, const uint8_t *data, size_t len,
                                const struct sockaddr *from, const char *peer_ip_override,
                                uint16_t peer_port_override, ice_candidate_t *local_cand);
static void handle_stun_response(turbo_ice_agent_t *agent, const uint8_t *data, size_t len,
                                 const struct sockaddr *from);
static ice_candidate_pair_t *find_pair_by_addresses(turbo_ice_agent_t *agent, const char *local_ip,
                                                    uint16_t local_port, const char *remote_ip,
                                                    uint16_t remote_port);
static void service_udp_candidate_socket(turbo_ice_agent_t *agent, ice_candidate_t *local_cand,
                                         uint64_t timeout_ms, int allow_data);
static void service_turn_candidate_socket(turbo_ice_agent_t *agent, ice_candidate_t *local_cand,
                                          uint64_t timeout_ms, int allow_data);
static ice_candidate_t *service_owner_candidate(turbo_ice_agent_t *agent, ice_candidate_t *candidate);

/* ============================================================================
 * UDP Socket for Candidates (coroutine-based)
 * ============================================================================ */

static int create_candidate_socket(turbo_ice_agent_t *agent, ice_candidate_t *candidate) {
  coro_socket_t *client = coro_socket_create(agent->ctx, CORO_SOCKET_UDP_V4);
  struct sockaddr_in bind_addr;
  struct sockaddr_storage addr;
  if (!client) return -1;

  candidate->socket = client;

  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(0);
  if (inet_pton(AF_INET, candidate->ip, &bind_addr.sin_addr) != 1) {
    coro_socket_destroy(client);
    candidate->socket = NULL;
    return -1;
  }
  if (coro_socket_bind(client, (const struct sockaddr *)&bind_addr) != 0) {
    coro_socket_destroy(client);
    candidate->socket = NULL;
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  if (coro_socket_get_local_address(client, &addr) == 0 && addr.ss_family == AF_INET) {
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
    candidate->port = ntohs(addr4->sin_port);
  }

  return 0;
}

static int send_udp_to_remote(coro_socket_t *socket, const char *ip, uint16_t port,
                              const void *data, size_t len) {
  struct sockaddr_in addr;

  if (!socket || !ip || !data || len == 0) {
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
    return -1;
  }

  return coro_socket_sendto(socket, (const char *)data, len, (const struct sockaddr *)&addr);
}

static int create_srflx_socket(coro_context_t *ctx, const ice_candidate_t *base,
                               const char *server_host, uint16_t server_port,
                               int timeout_ms, int retries,
                               stun_mapped_address_t *mapped,
                               coro_socket_t **socket_out,
                               char *related_ip, size_t related_ip_len,
                               uint16_t *related_port) {
  coro_socket_t *socket;
  struct sockaddr_storage local_addr;

  if (!ctx || !base || !server_host || !mapped || !socket_out) {
    return -1;
  }
  (void)base;

  socket = coro_socket_create(ctx, CORO_SOCKET_UDP_V4);
  if (!socket) {
    return -2;
  }

  if (timeout_ms <= 0) timeout_ms = 3000;
  if (retries <= 0) retries = 3;

  if (coro_socket_connect(socket, server_host, server_port) != 0) {
    coro_socket_destroy(socket);
    return -4;
  }

  memset(&local_addr, 0, sizeof(local_addr));
  if (coro_socket_get_local_address(socket, &local_addr) == 0 &&
      local_addr.ss_family == AF_INET) {
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&local_addr;
    if (related_ip && related_ip_len > 0) {
      inet_ntop(AF_INET, &addr4->sin_addr, related_ip, related_ip_len);
    }
    if (related_port) {
      *related_port = ntohs(addr4->sin_port);
    }
  }

  for (int attempt = 0; attempt < retries; attempt++) {
    stun_transaction_id_t txn_id;
    uint8_t buffer[STUN_HEADER_SIZE];
    size_t len;
    char *data = NULL;
    size_t data_len = 0;
    struct sockaddr_storage from;
    int rc;

    stun_generate_transaction_id(&txn_id);
    len = stun_build_binding_request(buffer, &txn_id);

    rc = coro_socket_send(socket, (const char *)buffer, len);
    if (rc != 0) {
      coro_socket_destroy(socket);
      return -5;
    }

    memset(&from, 0, sizeof(from));
    coro_socket_set_timeout(socket, (uint64_t)timeout_ms);
    rc = coro_socket_recvfrom(socket, &data, &data_len, &from);
    if (rc == 0 && data && data_len > 0 &&
        stun_is_stun_message((const uint8_t *)data, data_len)) {
      int result = stun_parse_binding_response((const uint8_t *)data, data_len,
                                               &txn_id, mapped);
      coro_socket_free_recv(data);
      if (result == 0) {
        *socket_out = socket;
        return 0;
      }
    } else if (data) {
      coro_socket_free_recv(data);
    }

    if (attempt + 1 < retries && ctx) {
      coro_sleep(ctx, (uint64_t)timeout_ms);
    }
  }

  coro_socket_destroy(socket);
  return -6;
}


/* ============================================================================
 * Host Candidate Gathering
 * ============================================================================ */

#ifdef _WIN32
static int gather_host_candidates_win32(turbo_ice_agent_t *agent) {
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
        cand->is_local = 1;
        snprintf(cand->foundation, sizeof(cand->foundation), "%d", ++agent->foundation_counter);
        generate_candidate_id(cand->id);

        /* Generate mDNS hostname for privacy if enabled */
        if (agent->config.use_mdns_candidates) {
          generate_mdns_hostname(cand->mdns_name, sizeof(cand->mdns_name));
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
static int gather_host_candidates_unix(turbo_ice_agent_t *agent) {
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
      cand->is_local = 1;
      snprintf(cand->foundation, sizeof(cand->foundation), "%d", ++agent->foundation_counter);
      generate_candidate_id(cand->id);

      /* Generate mDNS hostname for privacy if enabled */
      if (agent->config.use_mdns_candidates) {
        generate_mdns_hostname(cand->mdns_name, sizeof(cand->mdns_name));
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

static int gather_host_candidates(turbo_ice_agent_t *agent) {
#ifdef _WIN32
  return gather_host_candidates_win32(agent);
#else
  return gather_host_candidates_unix(agent);
#endif
}

/* ============================================================================
 * STUN Gathering (coroutine-based)
 * ============================================================================ */

static void gather_srflx_candidates(turbo_ice_agent_t *agent) {
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
      if (base->type != ICE_CANDIDATE_TYPE_HOST || !base->socket || base->family != AF_INET) {
        continue;
      }

      stun_mapped_address_t mapped;
      coro_socket_t *srflx_socket = NULL;
      char related_ip[64] = {0};
      uint16_t related_port = 0;
      int result = create_srflx_socket(agent->ctx, base, host, port, 1000, 1, &mapped,
                                       &srflx_socket, related_ip, sizeof(related_ip),
                                       &related_port);
      if (result != 0 || mapped.family != STUN_ADDR_FAMILY_IPV4) {
        if (srflx_socket) {
          coro_socket_destroy(srflx_socket);
        }
        continue;
      }
      if (strcmp(mapped.ip_str, related_ip[0] ? related_ip : base->ip) == 0 &&
          mapped.port == (related_port ? related_port : base->port)) {
        if (srflx_socket) {
          coro_socket_destroy(srflx_socket);
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
      cand->is_local = 1;
      snprintf(cand->foundation, sizeof(cand->foundation), "%d", ++agent->foundation_counter);
      generate_candidate_id(cand->id);

      strncpy(cand->related_ip, related_ip[0] ? related_ip : base->ip,
              sizeof(cand->related_ip) - 1);
      cand->related_port = related_port ? related_port : base->port;
      cand->socket = srflx_socket;

      agent->local_candidate_count++;

      if (agent->callbacks.on_candidate) {
        agent->callbacks.on_candidate(agent, cand, agent->callbacks.user_data);
      }
    }
  }
}

/* ============================================================================
 * TURN Gathering (coroutine-based)
 * ============================================================================ */

static void gather_relay_candidates(turbo_ice_agent_t *agent) {
  for (int i = 0; i < agent->config.turn_server_count && i < ICE_MAX_TURN_SERVERS; i++) {
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

    turbo_turn_client_t *turn = turn_client_create(agent->ctx, &turn_config);
    if (!turn) continue;

    agent->turn_clients[i] = turn;

    turn_allocation_t alloc;
    int result = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
      result = turn_client_allocate(turn, &alloc);
      if (result == 0) {
        break;
      }
      TLOG_WARN("TURN allocate failed server={} port={} attempt={} rc={}", host, port,
                attempt + 1, result);
      if (agent->ctx && attempt + 1 < 3) {
        coro_sleep(agent->ctx, 200);
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
      cand->is_local = 1;
      snprintf(cand->foundation, sizeof(cand->foundation), "%d", ++agent->foundation_counter);
      generate_candidate_id(cand->id);

      strncpy(cand->related_ip, alloc.mapped_ip, sizeof(cand->related_ip) - 1);
      cand->related_port = alloc.mapped_port;
      cand->turn_client = turn;
      cand->socket = NULL;

      agent->local_candidate_count++;
      TLOG_INFO("Gathered relay candidate {}:{} via {}", cand->ip, cand->port, host);

      if (agent->callbacks.on_candidate) {
        agent->callbacks.on_candidate(agent, cand, agent->callbacks.user_data);
      }
    } else if (result != 0) {
      TLOG_WARN("TURN relay candidate unavailable server={} port={} rc={}", host, port, result);
    }
  }
}

/* Gathering timeout is no longer needed — gathering is synchronous in coro */


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

int ice_agent_set_role(turbo_ice_agent_t *agent, int is_controlling) {
  if (!agent)
    return -1;
  if (agent->state != ICE_STATE_NEW && agent->state != ICE_STATE_GATHERING)
    return -2;

  agent->config.is_controlling = is_controlling ? 1 : 0;
  agent->role = is_controlling ? ICE_ROLE_CONTROLLING : ICE_ROLE_CONTROLLED;
  return 0;
}


turbo_ice_agent_t *ice_agent_create(coro_context_t *ctx, const ice_config_t *config) {
  if (!config)
    return NULL;
  ensure_random_seeded();

  turbo_ice_agent_t *agent = calloc(1, sizeof(turbo_ice_agent_t));
  if (!agent)
    return NULL;

  agent->ctx = ctx;
  agent->config = *config;

  agent->state = ICE_STATE_NEW;
  agent->gathering_state = ICE_GATHERING_NEW;
  agent->role = config->is_controlling ? ICE_ROLE_CONTROLLING : ICE_ROLE_CONTROLLED;
  agent->tie_breaker = generate_random_u64();

  /* Generate local credentials */
  generate_random_string(agent->local_ufrag, 8);
  generate_random_string(agent->local_pwd, 24);

  /* Initialize mDNS context if privacy mode enabled */
  if (config->use_mdns_candidates) {
    agent->mdns_ctx = mdns_create(NULL);
  }

  return agent;
}




static void ice_agent_quiesce_transports(turbo_ice_agent_t *agent) {
  if (!agent) {
    return;
  }

  for (int i = 0; i < ICE_MAX_TURN_SERVERS; i++) {
    if (agent->turn_clients[i]) {
      turn_client_destroy(agent->turn_clients[i]);
      agent->turn_clients[i] = NULL;
    }
  }

  for (int i = 0; i < agent->local_candidate_count; i++) {
    ice_candidate_t *cand = &agent->local_candidates[i];
    if (cand->socket) {
      int already_destroyed = 0;
      for (int j = 0; j < i; j++) {
        if (agent->local_candidates[j].socket == cand->socket) {
          already_destroyed = 1;
          break;
        }
      }
      if (!already_destroyed) {
        coro_socket_destroy((coro_socket_t *)cand->socket);
      }
      cand->socket = NULL;
    }
  }
}

static void ice_agent_drain_context(coro_context_t *ctx, uint64_t drain_ms) {
  uint64_t deadline_ms;

  if (!ctx || drain_ms == 0) {
    return;
  }

  deadline_ms = turbo_monotonic_ms() + drain_ms;
  do {
    coro_context_run(ctx, TURBO_RUN_ONCE);
  } while (turbo_monotonic_ms() < deadline_ms);
}

void ice_agent_destroy(turbo_ice_agent_t *agent) {
  coro_context_t *ctx;

  if (!agent) return;

  ctx = agent->ctx;
  ice_agent_quiesce_transports(agent);
  ice_agent_drain_context(ctx, 50);

  if (agent->mdns_ctx) {
    mdns_destroy(agent->mdns_ctx);
    agent->mdns_ctx = NULL;
  }

  free(agent);
}


void ice_agent_set_callbacks(turbo_ice_agent_t *agent, const ice_callbacks_t *callbacks) {
  if (agent && callbacks) {
    agent->callbacks = *callbacks;
  }
}

void ice_agent_get_local_credentials(turbo_ice_agent_t *agent, char *ufrag, size_t ufrag_len,
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

int ice_agent_set_remote_credentials(turbo_ice_agent_t *agent, const char *ufrag, const char *pwd) {
  if (!agent || !ufrag || !pwd)
    return -1;

  strncpy(agent->remote_ufrag, ufrag, sizeof(agent->remote_ufrag) - 1);
  strncpy(agent->remote_pwd, pwd, sizeof(agent->remote_pwd) - 1);
  agent->remote_credentials_set = 1;

  return 0;
}

int ice_agent_gather_candidates(turbo_ice_agent_t *agent) {
  if (!agent)
    return -1;

  if (agent->state != ICE_STATE_NEW)
    return -2;

  set_state(agent, ICE_STATE_GATHERING);
  set_gathering_state(agent, ICE_GATHERING_GATHERING);

  /* 1. Gather host candidates */
  gather_host_candidates(agent);

  /* Notify host candidates */
  for (int i = 0; i < agent->local_candidate_count; i++) {
    if (agent->callbacks.on_candidate) {
      agent->callbacks.on_candidate(agent, &agent->local_candidates[i], agent->callbacks.user_data);
    }
  }

  /* 2. Gather server-reflexive candidates via STUN */
  if (agent->config.stun_server_count > 0) {
    gather_srflx_candidates(agent);
  }

  /* 3. Gather relay candidates via TURN */
  if (agent->config.turn_server_count > 0) {
    gather_relay_candidates(agent);
  }

  /* Gathering is synchronous in coro — complete immediately */
  set_gathering_state(agent, ICE_GATHERING_COMPLETE);

  return 0;
}

int ice_agent_add_remote_candidate(turbo_ice_agent_t *agent, const char *candidate_str) {
  ice_candidate_t parsed;

  if (!agent || !candidate_str)
    return -1;

  if (agent->remote_candidate_count >= ICE_MAX_CANDIDATES)
    return -2;

  memset(&parsed, 0, sizeof(parsed));

  if (ice_candidate_parse(candidate_str, &parsed) != 0) {
    ice_tracef("ice_agent_add_remote_candidate parse_failed current_remote=%d candidate=%s",
               agent->remote_candidate_count, candidate_str);
    return -3;
  }

  if (parsed.mdns_name[0] != '\0' && resolve_mdns_hostname(&parsed) != 0) {
    ice_tracef("ice_agent_add_remote_candidate mdns_resolve_failed current_remote=%d candidate=%s",
               agent->remote_candidate_count, candidate_str);
    return -4;
  }

  parsed.is_local = 0;

  for (int i = 0; i < agent->remote_candidate_count; i++) {
    if (ice_candidates_equivalent(&agent->remote_candidates[i], &parsed)) {
      ice_tracef("ice_agent_add_remote_candidate duplicate_ignored current_remote=%d candidate=%s",
                 agent->remote_candidate_count, candidate_str);
      if (agent->state == ICE_STATE_CONNECTING) {
        rebuild_candidate_pairs(agent);
      }
      return 0;
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

  return 0;
}

void ice_agent_end_of_candidates(turbo_ice_agent_t *agent) {
  if (agent) {
    agent->remote_candidates_complete = 1;
    ice_tracef("ice_agent_end_of_candidates remote=%d state=%d",
               agent->remote_candidate_count, (int)agent->state);
  }
}

/* ============================================================================
 * Connectivity Check Implementation
 * ============================================================================ */

static ice_candidate_pair_t *find_pair_by_addresses(turbo_ice_agent_t *agent, const char *local_ip,
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

static int send_connectivity_check_internal(turbo_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                            int nominate, int track_transaction) {
  if (!agent || !pair || !pair->local || !pair->remote)
    return -1;

  stun_transaction_id_t txn_id;
  stun_transaction_id_t *active_txn = &txn_id;

  if (track_transaction) {
    active_txn = &agent->current_txn_id;
  }

  /* Generate transaction ID */
  stun_generate_transaction_id(active_txn);

  /* Build ICE STUN request */
  uint8_t stun_buf[STUN_MAX_MESSAGE_SIZE];
  int len = stun_build_ice_request(stun_buf, active_txn, agent->local_ufrag, agent->remote_ufrag,
                                   agent->remote_pwd, (uint32_t)pair->priority,
                                   agent->role == ICE_ROLE_CONTROLLING, agent->tie_breaker,
                                   nominate);

  if (len < 0) {
    TLOG_DEBUG("%s", "Failed to build STUN request");
    return -1;
  }

  TLOG_DEBUG("Outgoing BINDING REQUEST to {}:{} (txn: {})", pair->remote->ip,
            pair->remote->port, STUN_TRANSACTION_ID_LEN, active_txn->id);

  /* Send via suitable transport */
  int rc = -1;
  if (pair->local->type == ICE_CANDIDATE_TYPE_RELAY) {
    if (pair->local->turn_client) {
      ice_tracef("send_connectivity_check relay_send begin local=%s:%u remote=%s:%u nominate=%d",
                 pair->local->ip, (unsigned int)pair->local->port, pair->remote->ip,
                 (unsigned int)pair->remote->port, nominate);
      rc = turn_client_send((turbo_turn_client_t *)pair->local->turn_client, pair->remote->ip,
                            pair->remote->port, stun_buf, len);
      ice_tracef("send_connectivity_check relay_send rc=%d local=%s:%u remote=%s:%u", rc,
                 pair->local->ip, (unsigned int)pair->local->port, pair->remote->ip,
                 (unsigned int)pair->remote->port);
    }
  } else if (pair->local->socket) {
    coro_socket_t *client = (coro_socket_t *)pair->local->socket;
    rc = send_udp_to_remote(client, pair->remote->ip, pair->remote->port, stun_buf, (size_t)len);
  }

  if (rc == 0) {
    if (track_transaction) {
      pair->state = ICE_PAIR_STATE_IN_PROGRESS;
      pair->check_count++;
      pair->last_check_time = turbo_monotonic_ms();
    }
    TLOG_INFO("ICE check sent {}:{} -> {}:{} nominate={} pair_state={}",
              pair->local->ip, pair->local->port, pair->remote->ip, pair->remote->port,
              nominate, (int)pair->state);
  } else {
    TLOG_WARN("ICE check send failed {}:{} -> {}:{} rc={}",
              pair->local->ip, pair->local->port, pair->remote->ip, pair->remote->port, rc);
    if (track_transaction) {
      pair->state = ICE_PAIR_STATE_FAILED;
    }
  }

  return rc;
}

static int send_connectivity_check(turbo_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                   int nominate) {
  return send_connectivity_check_internal(agent, pair, nominate, 1);
}

static int send_connectivity_check_untracked(turbo_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                             int nominate) {
  return send_connectivity_check_internal(agent, pair, nominate, 0);
}


static void handle_stun_request(turbo_ice_agent_t *agent, const uint8_t *data, size_t len,
                                const struct sockaddr *from, const char *peer_ip_override,
                                uint16_t peer_port_override, ice_candidate_t *local_cand) {
  if (!agent || !data || !local_cand)
    return;

  /* Parse the request */
  char username[256] = {0};
  uint32_t priority = 0;
  int use_candidate = 0;
  stun_transaction_id_t rx_txn_id;
  memcpy(rx_txn_id.id, data + 8, STUN_TRANSACTION_ID_LEN);

  if (stun_parse_ice_request(data, len, username, &priority, &use_candidate) != 0) {
    TLOG_WARN("%s", "Failed to parse incoming STUN request");
    ice_tracef("handle_stun_request parse_failed len=%zu local=%s:%u", len, local_cand->ip,
               (unsigned int)local_cand->port);
    return;
  }

  /* Validate username: should be "local_ufrag:remote_ufrag" */
  char expected_username[256];
  fmt(expected_username, sizeof(expected_username), "{}:{}", agent->local_ufrag, agent->remote_ufrag);
  if (strcmp(username, expected_username) != 0) {
    TLOG_WARN("STUN username mismatch got='{}' expected='{}'", username, expected_username);
    ice_tracef("handle_stun_request username_mismatch got=%s expected=%s local=%s:%u", username,
               expected_username, local_cand->ip, (unsigned int)local_cand->port);
    return; /* Username mismatch */
  }

  /* Validate MESSAGE-INTEGRITY */
  if (stun_validate_message_integrity(data, len, agent->local_pwd) != 0) {
    TLOG_WARN("%s", "STUN request integrity validation failed");
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
  TLOG_DEBUG("Incoming BINDING REQUEST from {}:{} (txn: {})", remote_ip, remote_port,
            STUN_TRANSACTION_ID_LEN, rx_txn_id.id);
  TLOG_INFO("Incoming STUN request from {}:{} to {}:{} use_candidate={}",
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
    int send_rc = -1;
    if (local_cand->type == ICE_CANDIDATE_TYPE_RELAY && local_cand->turn_client) {
      send_rc = turn_client_send((turbo_turn_client_t *)local_cand->turn_client, remote_ip,
                                 remote_port, resp_buf, resp_len);
    } else if (local_cand->socket) {
      coro_socket_t *client = (coro_socket_t *)local_cand->socket;
      send_rc = send_udp_to_remote(client, remote_ip, remote_port, resp_buf, (size_t)resp_len);
    }
    ice_tracef("handle_stun_request response rc=%d from=%s:%u to=%s:%u", send_rc, local_cand->ip,
               (unsigned int)local_cand->port, remote_ip, (unsigned int)remote_port);
  } else {
    TLOG_DEBUG("%s", "Failed to build STUN response");
    ice_tracef("handle_stun_request build_response_failed from=%s:%u to=%s:%u", local_cand->ip,
               (unsigned int)local_cand->port, remote_ip, (unsigned int)remote_port);
  }


  /* Find or create the pair for this check */
  ice_candidate_pair_t *pair =
      find_pair_by_addresses(agent, local_cand->ip, local_cand->port, remote_ip, remote_port);

  if (pair) {
    if (pair->state != ICE_PAIR_STATE_SUCCEEDED) {
      pair->state = ICE_PAIR_STATE_SUCCEEDED;
      agent->valid_pairs_count++;
      TLOG_INFO("Validated pair from inbound request {}:{} <-> {}:{}",
                pair->local->ip, pair->local->port, pair->remote->ip, pair->remote->port);
    }

    if (agent->checks_in_progress &&
        agent->current_check_pair >= 0 &&
        agent->current_check_pair < agent->pair_count &&
        &agent->pairs[agent->current_check_pair] == pair) {
      agent->checks_in_progress = 0;
    }

    if (agent->role == ICE_ROLE_CONTROLLING &&
        (agent->config.aggressive_nomination || !agent->nomination_started) &&
        !pair->nominated &&
        ice_pair_is_loopback(pair)) {
      TLOG_INFO("Triggering immediate nomination for {}:{} -> {}:{}",
                pair->local->ip, pair->local->port, pair->remote->ip, pair->remote->port);
      send_connectivity_check(agent, pair, 1);
      pair->nominated = 1;
      agent->nomination_started = 1;
      agent->selected_pair = pair;
      set_state(agent, ICE_STATE_COMPLETED);
    }

    /* If we received USE-CANDIDATE and we're controlled, mark as nominated */
    if (use_candidate && agent->role == ICE_ROLE_CONTROLLED) {
      pair->nominated = 1;
      agent->selected_pair = pair;
      set_state(agent, ICE_STATE_COMPLETED);
    } else if (agent->role == ICE_ROLE_CONTROLLED &&
               agent->config.allow_loopback &&
               !agent->selected_pair &&
               ice_pair_is_loopback(pair)) {
      pair->nominated = 1;
      agent->selected_pair = pair;
      set_state(agent, ICE_STATE_COMPLETED);
    }
  }

  /* Trigger a check back (triggered check) if we're not already checking this pair */
  if (pair && pair->state == ICE_PAIR_STATE_FROZEN) {
    pair->state = ICE_PAIR_STATE_WAITING;
  }
}

static void handle_stun_response(turbo_ice_agent_t *agent, const uint8_t *data, size_t len,
                                 const struct sockaddr *from) {
  if (!agent || !data)
    return;
  (void)from;

  /* Verify transaction ID matches our current check */
  if (memcmp(data + 8, agent->current_txn_id.id, STUN_TRANSACTION_ID_LEN) != 0) {
    if (agent->selected_pair || agent->state != ICE_STATE_CONNECTING) {
      ice_tracef("handle_stun_response ignored_untracked_response state=%d selected=%p",
                 (int)agent->state, (void *)agent->selected_pair);
    } else {
      TLOG_WARN("%s", "STUN txn mismatch while waiting for response");
    }
    return; /* Not our transaction */
  }

  /* Validate MESSAGE-INTEGRITY with the same peer password used to sign the
   * original connectivity check request. */
  if (stun_validate_message_integrity(data, len, agent->remote_pwd) != 0) {
    TLOG_WARN("%s", "STUN response integrity validation failed");
    agent->checks_in_progress = 0;
    return; /* Invalid authentication */
  }

  /* Check for error response */
  int error_code = stun_get_error_code(data, len);
  if (error_code == STUN_ERROR_ROLE_CONFLICT) {
    TLOG_INFO("%s", "Role conflict detected, switching roles");
    /* Role conflict - switch roles */
    if (agent->role == ICE_ROLE_CONTROLLING) {
      agent->role = ICE_ROLE_CONTROLLED;
    } else {
      agent->role = ICE_ROLE_CONTROLLING;
    }
    /* Regenerate tie-breaker and restart checks */
    agent->tie_breaker = generate_random_u64();
    agent->checks_in_progress = 0; /* Reset state so timer can restart */
    return;
  }

  if (error_code != 0) {
    TLOG_DEBUG("STUN error response: {}", error_code);
    /* Other error - mark current pair as failed */
    if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count) {
      agent->pairs[agent->current_check_pair].state = ICE_PAIR_STATE_FAILED;
    }
    agent->checks_in_progress = 0;
    return;
  }

  /* Success! Mark the pair as succeeded */
  if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count) {
    ice_candidate_pair_t *pair = &agent->pairs[agent->current_check_pair];

    if (pair->state != ICE_PAIR_STATE_SUCCEEDED) {
      TLOG_INFO("ICE check succeeded pair={} {}:{} <-> {}:{}",
                agent->current_check_pair, pair->local->ip, pair->local->port,
                pair->remote->ip, pair->remote->port);
      pair->state = ICE_PAIR_STATE_SUCCEEDED;
      agent->valid_pairs_count++;
    }

    /* If controlling and using aggressive nomination, nominate this pair if not already nominated
     */
    if (agent->role == ICE_ROLE_CONTROLLING) {
      if ((agent->config.aggressive_nomination || !agent->nomination_started) && !pair->nominated) {
        /* Send a check with USE-CANDIDATE */
        TLOG_DEBUG("Nominating pair {} (aggressive)", agent->current_check_pair);
        send_connectivity_check(agent, pair, 1);
        pair->nominated = 1;
        agent->nomination_started = 1;
        if (!agent->selected_pair && ice_pair_is_loopback(pair)) {
          agent->selected_pair = pair;
          set_state(agent, ICE_STATE_COMPLETED);
        }
      }
    }

    /* If this pair is nominated (either by us or by peer), select it */
    if (pair->nominated) {
      agent->selected_pair = pair;
      set_state(agent, ICE_STATE_COMPLETED);
    }
  }

  agent->checks_in_progress = 0;
}

static int ice_ip_is_private_v4(const char *ip) {
  unsigned int a, b, c, d;

  if (!ip || sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
      a > 255 || b > 255 || c > 255 || d > 255) {
    return 0;
  }

  return (a == 10) ||
         (a == 172 && b >= 16 && b <= 31) ||
         (a == 192 && b == 168) ||
         (a == 127) ||
         (a == 169 && b == 254);
}

static int find_pair_index_by_candidates(turbo_ice_agent_t *agent, ice_candidate_t *local,
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

static int ice_pair_allowed_for_checklist(turbo_ice_agent_t *agent, ice_candidate_t *local,
                                          ice_candidate_t *remote, int has_local_public,
                                          int has_remote_public) {
  int local_is_loopback;
  int remote_is_loopback;
  int local_is_private;
  int remote_is_private;

  if (!agent || !local || !remote) {
    return 0;
  }

  if (!local->socket && !local->turn_client) {
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
  local_is_private = strchr(local->ip, '.') && ice_ip_is_private_v4(local->ip);
  remote_is_private = strchr(remote->ip, '.') && ice_ip_is_private_v4(remote->ip);

  if (local->type == ICE_CANDIDATE_TYPE_HOST &&
      remote->type == ICE_CANDIDATE_TYPE_HOST &&
      local_is_loopback != remote_is_loopback) {
    return 0;
  }

  if (has_remote_public && remote_is_private &&
      !(agent->config.allow_loopback && local_is_loopback && remote_is_loopback)) {
    return 0;
  }

  if (has_local_public && has_remote_public &&
      local->type == ICE_CANDIDATE_TYPE_HOST &&
      local_is_private && !remote_is_private &&
      !(agent->config.allow_loopback && local_is_loopback && remote_is_loopback)) {
    /*
     * Once both peers already have public reachability, a private host ->
     * public pair only drags the checklist toward asymmetric fallback
     * paths. Prefer the public/srflx pair instead of racing RFC1918 host
     * candidates against a public remote.
     */
    return 0;
  }

  return 1;
}

static void rebuild_candidate_pairs(turbo_ice_agent_t *agent) {
  if (!agent)
    return;

  int new_pairs_added = 0;
  int has_local_public = 0;
  int has_local_relay = 0;
  int has_remote_public = 0;
  int has_remote_relay = 0;
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

  for (int i = 0; i < agent->local_candidate_count; i++) {
    if (agent->local_candidates[i].type == ICE_CANDIDATE_TYPE_RELAY) {
      has_local_relay = 1;
    }
    if (strchr(agent->local_candidates[i].ip, '.') &&
        !ice_ip_is_private_v4(agent->local_candidates[i].ip)) {
      has_local_public = 1;
    }
  }

  for (int j = 0; j < agent->remote_candidate_count; j++) {
    if (agent->remote_candidates[j].type == ICE_CANDIDATE_TYPE_RELAY) {
      has_remote_relay = 1;
    }
    if (strchr(agent->remote_candidates[j].ip, '.') &&
        !ice_ip_is_private_v4(agent->remote_candidates[j].ip)) {
      has_remote_public = 1;
    }
  }

  if (agent->pair_count > 0) {
    int write_index = 0;

    for (int i = 0; i < agent->pair_count; i++) {
      ice_candidate_pair_t pair = agent->pairs[i];

      if (!ice_pair_allowed_for_checklist(agent, pair.local, pair.remote,
                                          has_local_public, has_remote_public)) {
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
      int local_is_loopback = ice_ip_is_loopback(local->ip);
      int remote_is_loopback = ice_ip_is_loopback(remote->ip);

      if (!ice_pair_allowed_for_checklist(agent, local, remote,
                                          has_local_public, has_remote_public)) {
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
        if (has_local_relay && has_remote_relay &&
            local->type == ICE_CANDIDATE_TYPE_RELAY &&
            remote->type == ICE_CANDIDATE_TYPE_RELAY) {
          pair->priority |= (1ULL << 63);
        } else if (agent->config.allow_loopback &&
                   local->type == ICE_CANDIDATE_TYPE_HOST &&
                   remote->type == ICE_CANDIDATE_TYPE_HOST &&
                   local_is_loopback && remote_is_loopback) {
          /*
           * When both peers run on the same host, loopback host pairs are the
           * only direct path that is guaranteed to work. Prefer them over
           * public/private host pairs that depend on hairpin routing.
           */
          pair->priority |= (1ULL << 62);
        } else if (has_local_relay && has_remote_relay &&
                   (local->type == ICE_CANDIDATE_TYPE_RELAY ||
                    remote->type == ICE_CANDIDATE_TYPE_RELAY)) {
          pair->priority |= (1ULL << 61);
        } else if (local->type == ICE_CANDIDATE_TYPE_HOST &&
                   remote->type == ICE_CANDIDATE_TYPE_HOST &&
                   !local_is_loopback && !remote_is_loopback) {
          /* When both host and loopback host pairs exist on the same machine,
           * prefer routable/private interfaces first so both peers converge on
           * the same candidate family instead of racing host vs loopback. */
          pair->priority |= (1ULL << 60);
        }
        pair->nominated = 0;
        pair->check_count = 0;
        pair->last_check_time = 0;
        agent->pair_count++;
        new_pairs_added++;
      }
    }
  }

  if (new_pairs_added > 0) {
    TLOG_DEBUG("Added {} new candidate pairs (total: {})", new_pairs_added,
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

static ice_candidate_t *service_owner_candidate(turbo_ice_agent_t *agent, ice_candidate_t *candidate) {
  if (!agent || !candidate) {
    return candidate;
  }

  for (int i = 0; i < agent->local_candidate_count; ++i) {
    ice_candidate_t *local = &agent->local_candidates[i];

    if (candidate->socket && local->socket == candidate->socket) {
      return local;
    }
    if (candidate->turn_client && local->turn_client == candidate->turn_client) {
      return local;
    }
  }

  return candidate;
}

static void service_udp_candidate_socket(turbo_ice_agent_t *agent, ice_candidate_t *local_cand,
                                         uint64_t timeout_ms, int allow_data) {
  coro_socket_t *client;
  ice_candidate_t *owner;
  uint64_t recv_timeout_ms;
  int drained = 0;

  if (!agent || !local_cand || !local_cand->socket) {
    return;
  }
  owner = service_owner_candidate(agent, local_cand);
  if (owner && owner->io_active) {
    return;
  }
  if (owner) {
    owner->io_active = 1;
  }

  client = (coro_socket_t *)local_cand->socket;
  recv_timeout_ms = timeout_ms;

  for (;;) {
    char *data = NULL;
    size_t data_len = 0;
    struct sockaddr_storage from;
    int rc;

    memset(&from, 0, sizeof(from));
    coro_socket_set_timeout(client, recv_timeout_ms);
    rc = coro_socket_recvfrom(client, &data, &data_len, &from);
    if (rc != 0 || !data || data_len == 0) {
      ice_tracef("service_udp_candidate_socket recv rc=%d data_len=%zu local=%s:%u", rc, data_len,
                 local_cand->ip, (unsigned int)local_cand->port);
      if (data) {
        coro_socket_free_recv(data);
      }
      break;
    }

    {
      char from_ip[64] = {0};
      uint16_t from_port = 0;
      if (from.ss_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&from;
        inet_ntop(AF_INET, &addr4->sin_addr, from_ip, sizeof(from_ip));
        from_port = ntohs(addr4->sin_port);
      }
#if !defined(_WIN32)
      else if (from.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&from;
        inet_ntop(AF_INET6, &addr6->sin6_addr, from_ip, sizeof(from_ip));
        from_port = ntohs(addr6->sin6_port);
      }
#endif
      ice_tracef("service_udp_candidate_socket recv rc=0 data_len=%zu from=%s:%u local=%s:%u",
                 data_len, from_ip, (unsigned int)from_port, local_cand->ip,
                 (unsigned int)local_cand->port);
    }

    if (stun_is_stun_message((const uint8_t *)data, data_len)) {
      uint16_t msg_type = read_u16_be((const uint8_t *)data);
      if (msg_type == STUN_MSG_BINDING_RESPONSE || msg_type == STUN_MSG_BINDING_ERROR_RESPONSE) {
        handle_stun_response(agent, (const uint8_t *)data, data_len, (const struct sockaddr *)&from);
      } else if (msg_type == STUN_MSG_BINDING_REQUEST) {
        handle_stun_request(agent, (const uint8_t *)data, data_len, (const struct sockaddr *)&from,
                            NULL, 0, local_cand);
      }
    } else if (allow_data && agent->callbacks.on_data) {
      agent->callbacks.on_data(agent, data, data_len, agent->callbacks.user_data);
    }

    coro_socket_free_recv(data);

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

  if (owner) {
    owner->io_active = 0;
  }
}

static void service_turn_candidate_socket(turbo_ice_agent_t *agent, ice_candidate_t *local_cand,
                                          uint64_t timeout_ms, int allow_data) {
  ice_candidate_t *owner;
  turbo_turn_client_t *turn;
  char peer_ip[64] = {0};
  uint16_t peer_port = 0;
  void *buf = NULL;
  const uint8_t *payload = NULL;
  size_t payload_len = 0;
  int rc;

  if (!agent || !local_cand || !local_cand->turn_client) {
    return;
  }
  owner = service_owner_candidate(agent, local_cand);
  if (owner && owner->io_active) {
    return;
  }
  if (owner) {
    owner->io_active = 1;
  }

  turn = (turbo_turn_client_t *)local_cand->turn_client;
  coro_socket_set_timeout(turn->client, timeout_ms);
  rc = turn_client_recv(turn, peer_ip, &peer_port, &buf, &payload, &payload_len);
  if (rc != 0 || !payload || payload_len == 0) {
    ice_tracef("service_turn_candidate_socket recv rc=%d payload_len=%zu local=%s:%u", rc,
               payload_len, local_cand->ip, (unsigned int)local_cand->port);
    if (buf) {
      coro_socket_free_recv(buf);
    }
    if (owner) {
      owner->io_active = 0;
    }
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
      handle_stun_response(agent, payload, payload_len, NULL);
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

  coro_socket_free_recv(buf);

  if (!allow_data && agent->state != ICE_STATE_CONNECTING) {
    ice_tracef("service_turn_candidate_socket stop_after_state_change state=%d local=%s:%u",
               (int)agent->state, local_cand->ip, (unsigned int)local_cand->port);
    if (owner) {
      owner->io_active = 0;
    }
    return;
  }

  if (owner) {
    owner->io_active = 0;
  }
}

static void run_selected_pair_io(turbo_ice_agent_t *agent) {
  while (agent && (agent->state == ICE_STATE_CONNECTED || agent->state == ICE_STATE_COMPLETED)) {
    if (agent->selected_pair && agent->selected_pair->local) {
      if (agent->selected_pair->local->type == ICE_CANDIDATE_TYPE_RELAY) {
        service_turn_candidate_socket(agent, agent->selected_pair->local, 1, 1);
      } else {
        service_udp_candidate_socket(agent, agent->selected_pair->local, 1, 1);
      }
      coro_sleep(agent->ctx, 1);
    } else {
      coro_sleep(agent->ctx, 10);
    }
  }
}

static void selected_pair_io_task(coro_t *co, void *arg) {
  turbo_ice_agent_t *agent = (turbo_ice_agent_t *)arg;
  (void)co;

  run_selected_pair_io(agent);
  if (agent) {
    agent->selected_pair_io_running = 0;
  }
}

static void service_connectivity_check_io(turbo_ice_agent_t *agent, uint64_t timeout_ms) {
  if (!agent) {
    return;
  }

  for (int i = 0; i < agent->local_candidate_count; ++i) {
    ice_candidate_t *local = &agent->local_candidates[i];

    if (local->type == ICE_CANDIDATE_TYPE_RELAY) {
      if (local->turn_client) {
        service_turn_candidate_socket(agent, local, timeout_ms, 0);
        if (agent->state != ICE_STATE_CONNECTING) {
          return;
        }
      }
      continue;
    }

    if (local->socket) {
      service_udp_candidate_socket(agent, local, timeout_ms, 0);
      if (agent->state != ICE_STATE_CONNECTING) {
        return;
      }
    }
  }
}

static void run_connectivity_checks(turbo_ice_agent_t *agent) {
  if (agent->state != ICE_STATE_CONNECTING)
    return;

  while (agent->state == ICE_STATE_CONNECTING) {
    uint64_t now = turbo_monotonic_ms();
    uint64_t elapsed = now - agent->check_start_time;

    /* Connectivity checks need inbound STUN pumping before a pair can advance. */
    service_connectivity_check_io(agent, 1);
    if (agent->state != ICE_STATE_CONNECTING) {
      return;
    }

    /* Check for overall timeout */
    if (elapsed > (uint64_t)agent->config.connectivity_timeout_ms) {
      TLOG_INFO("%s", "Connectivity check timeout elapsed");
      if (agent->valid_pairs_count > 0) {
        if (!agent->selected_pair) {
          for (int i = 0; i < agent->pair_count; i++) {
            if (agent->pairs[i].state == ICE_PAIR_STATE_SUCCEEDED) {
              agent->selected_pair = &agent->pairs[i];
              TLOG_INFO("No pair nominated, picking pair %d as fallback", i);
              break;
            }
          }
        }
        set_state(agent, agent->selected_pair ? ICE_STATE_COMPLETED : ICE_STATE_FAILED);
      } else {
        set_state(agent, ICE_STATE_FAILED);
      }
      return;
    }

    /* If a check is in progress, wait for response or per-check timeout */
    if (agent->checks_in_progress) {
      if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count) {
        ice_candidate_pair_t *pair = &agent->pairs[agent->current_check_pair];

        /* Per-check timeout (3 seconds) */
        if (pair->last_check_time > 0 && now - pair->last_check_time > 3000) {
          if (pair->check_count < 3) {
            send_connectivity_check(agent, pair, 0);
          } else {
            pair->state = ICE_PAIR_STATE_FAILED;
            agent->checks_in_progress = 0;
          }
        }
      }
      coro_sleep(agent->ctx, ICE_DEFAULT_TA_INTERVAL);
      continue;
    }

    int found = 0;
    if (agent->role == ICE_ROLE_CONTROLLING && agent->valid_pairs_count > 0 &&
        !agent->selected_pair) {
      for (int i = 0; i < agent->pair_count; i++) {
        if (agent->pairs[i].state == ICE_PAIR_STATE_SUCCEEDED && !agent->pairs[i].nominated) {
          agent->current_check_pair = i;
          agent->checks_in_progress = 1;
          send_connectivity_check(agent, &agent->pairs[i], 1);
          agent->pairs[i].nominated = 1;
          agent->nomination_started = 1;
          found = 1;
          break;
        }
      }
      if (found) {
        coro_sleep(agent->ctx, ICE_DEFAULT_TA_INTERVAL);
        continue;
      }
    }

    /* Find next pair to check */
    found = 0;
    if (agent->config.allow_loopback) {
      for (int i = 0; i < agent->pair_count; i++) {
        ice_candidate_pair_t *pair = &agent->pairs[i];
        if (pair->state != ICE_PAIR_STATE_WAITING) {
          continue;
        }
        if (ice_pair_is_loopback(pair)) {
          agent->current_check_pair = i;
          agent->checks_in_progress = 1;
          send_connectivity_check(agent, pair, 0);
          found = 1;
          break;
        }
      }
    }
    for (int i = 0; i < agent->pair_count; i++) {
      if (found) {
        break;
      }
      ice_candidate_pair_t *pair = &agent->pairs[i];
      if (pair->state == ICE_PAIR_STATE_WAITING) {
        agent->current_check_pair = i;
        agent->checks_in_progress = 1;
        send_connectivity_check(agent, pair, 0);
        found = 1;
        break;
      }
    }
    if (!found) {
      /* All pairs checked — determine final state */
      if (agent->selected_pair) {
        set_state(agent, ICE_STATE_COMPLETED);
        return;
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
          TLOG_INFO("All %d built pairs failed", agent->pair_count);
          set_state(agent, ICE_STATE_FAILED);
          return;
        }
      }
    }

    coro_sleep(agent->ctx, ICE_DEFAULT_TA_INTERVAL);
  }
}


int ice_agent_start_checks(turbo_ice_agent_t *agent) {
  if (!agent)
    return -1;

  if (agent->gathering_state != ICE_GATHERING_COMPLETE)
    return -2;

  if (!agent->remote_credentials_set) {
    TLOG_INFO("%s", "Cannot start checks: remote credentials not set");
    return -3;
  }

  if (agent->state == ICE_STATE_CONNECTING || agent->state == ICE_STATE_CONNECTED ||
      agent->state == ICE_STATE_COMPLETED) {
    /* Already started, just rebuild pairs to catch any newly added trickle candidates */
    ice_tracef("ice_agent_start_checks rebuild_existing state=%d local=%d remote=%d",
               (int)agent->state, agent->local_candidate_count, agent->remote_candidate_count);
    rebuild_candidate_pairs(agent);
    return 0;
  }

  TLOG_INFO("Starting checks with {} local and {} remote candidates",
           agent->local_candidate_count, agent->remote_candidate_count);
  ice_tracef("ice_agent_start_checks begin local=%d remote=%d", agent->local_candidate_count,
             agent->remote_candidate_count);

  /* Build initial candidate pairs */
  agent->pair_count = 0;
  rebuild_candidate_pairs(agent);
  ice_tracef("ice_agent_start_checks built pair_count=%d", agent->pair_count);

  if (agent->pair_count == 0) {
    TLOG_INFO("%s", "Starting CONNECTIVITY CHECKS with 0 pairs (waiting for remote candidates)");
  } else {
    TLOG_INFO("Built {} candidate pairs", agent->pair_count);
  }

  /* Initialize check state */
  agent->current_check_pair = -1;
  agent->checks_in_progress = 0;
  agent->valid_pairs_count = 0;
  agent->nomination_started = 0;
  agent->check_start_time = turbo_monotonic_ms();

  set_state(agent, ICE_STATE_CONNECTING);
  ice_tracef("ice_agent_start_checks entering_connectivity_loop");

  /* Run connectivity checks synchronously (coro-based) */
  run_connectivity_checks(agent);
  ice_tracef("ice_agent_start_checks connectivity_loop_done state=%d selected=%p", (int)agent->state,
             (void *)agent->selected_pair);

  return 0;
}

void ice_agent_poll_selected_pair(turbo_ice_agent_t *agent, uint64_t timeout_ms) {
  if (!agent) {
    return;
  }

  if (agent->state != ICE_STATE_CONNECTED && agent->state != ICE_STATE_COMPLETED) {
    return;
  }

  if (!agent->selected_pair || !agent->selected_pair->local) {
    return;
  }

  if (timeout_ms == 0) {
    timeout_ms = 1;
  }

  if (agent->selected_pair->local->type == ICE_CANDIDATE_TYPE_RELAY) {
    service_turn_candidate_socket(agent, agent->selected_pair->local, timeout_ms, 1);
  } else {
    service_udp_candidate_socket(agent, agent->selected_pair->local, timeout_ms, 1);
  }
}


int ice_agent_send(turbo_ice_agent_t *agent, const void *data, size_t len) {
  if (!agent || !data || len == 0)
    return -1;

  if (agent->state != ICE_STATE_CONNECTED && agent->state != ICE_STATE_COMPLETED)
    return -2;

  if (!agent->selected_pair)
    return -3;

  ice_candidate_t *local = agent->selected_pair->local;
  ice_candidate_t *remote = agent->selected_pair->remote;

  if (local->type == ICE_CANDIDATE_TYPE_HOST || local->type == ICE_CANDIDATE_TYPE_SRFLX) {
    if (!local->socket)
      return -4;
    coro_socket_t *client = (coro_socket_t *)local->socket;
    return send_udp_to_remote(client, remote->ip, remote->port, data, len);
  } else if (local->type == ICE_CANDIDATE_TYPE_RELAY) {
    if (!local->turn_client) return -5;
    return turn_client_send((turbo_turn_client_t *)local->turn_client, remote->ip, remote->port, data, len);
  }

  return -6;
}


ice_state_t ice_agent_get_state(turbo_ice_agent_t *agent) {
  if (!agent) return ICE_STATE_CLOSED;
  return agent->state;
}

coro_context_t *ice_agent_get_context(turbo_ice_agent_t *agent) {
  if (!agent) return NULL;
  return agent->ctx;
}

ice_gathering_state_t ice_agent_get_gathering_state(turbo_ice_agent_t *agent) {
  if (!agent) return ICE_GATHERING_NEW;
  return agent->gathering_state;
}

int ice_agent_get_selected_pair(turbo_ice_agent_t *agent, ice_candidate_t *local_out,
                                ice_candidate_t *remote_out) {
  if (!agent || !agent->selected_pair)
    return -1;

  if (local_out)
    *local_out = *agent->selected_pair->local;
  if (remote_out)
    *remote_out = *agent->selected_pair->remote;

  return 0;
}

int ice_agent_get_local_candidate_count(turbo_ice_agent_t *agent) {
  if (!agent) return 0;
  return agent->local_candidate_count;
}

int ice_agent_get_local_candidate(turbo_ice_agent_t *agent, int index, ice_candidate_t *out) {
  if (!agent || !out)
    return -1;
  if (index < 0 || index >= agent->local_candidate_count)
    return -2;

  *out = agent->local_candidates[index];
  return 0;
}

void ice_agent_set_allow_loopback(turbo_ice_agent_t *agent, int allow) {
  if (agent) {
    agent->config.allow_loopback = allow;
  }
}

void ice_agent_close(turbo_ice_agent_t *agent) {
  if (agent && agent->state != ICE_STATE_CLOSED) {
    agent->checks_in_progress = 0;
    agent->current_check_pair = -1;
    set_state(agent, ICE_STATE_CLOSED);
    ice_agent_quiesce_transports(agent);
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
  switch (state) {
    case ICE_STATE_NEW:          return "NEW";
    case ICE_STATE_GATHERING:    return "GATHERING";
    case ICE_STATE_CONNECTING:   return "CONNECTING";
    case ICE_STATE_CONNECTED:    return "CONNECTED";
    case ICE_STATE_COMPLETED:    return "COMPLETED";
    case ICE_STATE_FAILED:       return "FAILED";
    case ICE_STATE_DISCONNECTED: return "DISCONNECTED";
    case ICE_STATE_CLOSED:       return "CLOSED";
    default:                     return "UNKNOWN";
  }
}

const char *ice_gathering_state_name(ice_gathering_state_t state) {
  switch (state) {
    case ICE_GATHERING_NEW:       return "NEW";
    case ICE_GATHERING_GATHERING: return "GATHERING";
    case ICE_GATHERING_COMPLETE:  return "COMPLETE";
    default:                      return "UNKNOWN";
  }
}

const char *ice_candidate_type_name(ice_candidate_type_t type) {
  switch (type) {
    case ICE_CANDIDATE_TYPE_HOST:  return "host";
    case ICE_CANDIDATE_TYPE_SRFLX: return "srflx";
    case ICE_CANDIDATE_TYPE_PRFLX: return "prflx";
    case ICE_CANDIDATE_TYPE_RELAY: return "relay";
    default:                       return "unknown";
  }
}
