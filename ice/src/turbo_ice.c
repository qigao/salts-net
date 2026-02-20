/**
 * turbo_ice.c - ICE Agent Implementation (RFC 8445)
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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
#include <turbo_thread.h>

#include "turbo_dns.h"
#include "turbo_str.h"
#include "tlog.h"
#include "turbo_mdns.h"
#include "turbo_async_client.h"
#include <ctype.h>
#include <stb_sprintf.h>
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
  #include <net/if.h>
  #include <netinet/in.h>
  #include <strings.h>
  #include <unistd.h>
#endif

/* Thread-safe random seeding using atomic CAS */
#include "turbo_atomic.h"

static turbo_atomic_int_t g_random_seeded = TURBO_ATOMIC_INIT(0);

static void ensure_random_seeded(void) {
  if (turbo_atomic_load(&g_random_seeded) == 0) {
    if (turbo_atomic_cas(&g_random_seeded, 0, 1)) {
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

  stbsp_snprintf(buf, (int)buf_len, "%s.local", uuid);
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
  int current_check_pair;               /* Index of pair being checked */
  stun_transaction_id_t current_txn_id; /* Current check transaction ID */
  int checks_in_progress;
  int valid_pairs_count;     /* Number of succeeded pairs */
  uint64_t check_start_time; /* When checks started */

  /* STUN client for gathering srflx candidates */
  turbo_stun_client_t *stun_clients[ICE_MAX_STUN_SERVERS];
  int pending_stun_requests;

  /* TURN clients for relay candidates */
  turbo_turn_client_t *turn_clients[ICE_MAX_TURN_SERVERS];
  int pending_turn_requests;

  /* mDNS context for privacy-preserving candidates */
  mdns_ctx_t *mdns_ctx;

  /* Timers */
  turbo_timer_t *gathering_timer;
  turbo_timer_t *check_timer;
  turbo_timer_t *keepalive_timer;

  /* Foundation counter */
  int foundation_counter;
  
  /* Synchronization */
  turbo_mutex_t lock;

  /* Callbacks */
  ice_callbacks_t callbacks;

  /* Flags */
  int remote_credentials_set;
  int remote_candidates_complete;
  int nomination_started; /* Controlling agent has started nominating */

  /* Internal */
  int destroying; /* Set to 1 when destroy is called */
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
static void set_state(turbo_ice_agent_t *agent, ice_state_t new_state) {
  if (agent->state != new_state) {
    ice_state_t old_state = agent->state;
    agent->state = new_state;

    /* Stop check timer when we leave CONNECTING state */
    if (old_state == ICE_STATE_CONNECTING && new_state != ICE_STATE_CONNECTING) {
      turbo_timer_stop(agent->check_timer);
    }


    if (agent->callbacks.on_state_change) {
      TLOG_INFO("State change: {} -> {}", ENUM_NAME(old_state), ENUM_NAME(new_state));
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

/* Forward declarations for connectivity check functions */
static void send_connectivity_check(turbo_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                    int nominate);
static void handle_stun_request(turbo_ice_agent_t *agent, const uint8_t *data, size_t len,
                                const struct sockaddr *from, ice_candidate_t *local_cand);
static void handle_stun_response(turbo_ice_agent_t *agent, const uint8_t *data, size_t len,
                                 const struct sockaddr *from);
static ice_candidate_pair_t *find_pair_by_addresses(turbo_ice_agent_t *agent, const char *local_ip,
                                                    uint16_t local_port, const char *remote_ip,
                                                    uint16_t remote_port);

/* ============================================================================
 * UDP Socket for Candidates
 * ============================================================================ */

static void on_candidate_async_event(async_client_t *client, const async_client_event_t *event, void *user_data) {
  turbo_ice_agent_t *agent = (turbo_ice_agent_t *)user_data;
  if (!agent) return;

  turbo_mutex_lock(&agent->lock);
  if (agent->destroying) {
    turbo_mutex_unlock(&agent->lock);
    return;
  }

  if (event->type == ASYNC_CLIENT_EVENT_DATA) {
    const uint8_t *data = (const uint8_t *)event->data;
    size_t nread = event->length;

    /* Find which local candidate this socket belongs to */
    ice_candidate_t *local_cand = NULL;
    for (int i = 0; i < agent->local_candidate_count; i++) {
        if (agent->local_candidates[i].socket == client) {
            local_cand = &agent->local_candidates[i];
            break;
        }
    }

    if (!local_cand) {
        turbo_mutex_unlock(&agent->lock);
        return;
    }

    /* Check if this is a STUN message */
    if (stun_is_stun_message(data, nread)) {
        uint16_t msg_type = read_u16_be(data);

        if (msg_type == STUN_MSG_BINDING_REQUEST) {
            /* Incoming connectivity check - respond. Use a fake sockaddr for now as next steps will refactor STUN */
            struct sockaddr_in from_dummy;
            memset(&from_dummy, 0, sizeof(from_dummy));
            handle_stun_request(agent, data, nread, (const struct sockaddr *)&from_dummy, local_cand);
        } else if (msg_type == STUN_MSG_BINDING_RESPONSE || msg_type == STUN_MSG_BINDING_ERROR_RESPONSE) {
            /* Response to our connectivity check */
            struct sockaddr_in from_dummy;
            memset(&from_dummy, 0, sizeof(from_dummy));
            handle_stun_response(agent, data, nread, (const struct sockaddr *)&from_dummy);
        }
        turbo_mutex_unlock(&agent->lock);
        return;
    }

    /* Regular data - pass to application (only if connected) */
    if (agent->state == ICE_STATE_CONNECTED || agent->state == ICE_STATE_COMPLETED) {
        if (agent->callbacks.on_data) {
            agent->callbacks.on_data(agent, data, nread, agent->callbacks.user_data);
        }
    }
  }
  turbo_mutex_unlock(&agent->lock);
}


static int create_candidate_socket(turbo_ice_agent_t *agent, ice_candidate_t *candidate) {
  async_client_t *client = async_client_create(on_candidate_async_event, agent);
  if (!client) return -1;

  char url[512];
  stbsp_snprintf(url, sizeof(url), "udp://%s:0", candidate->ip);

  async_client_status_t status = async_client_connect(client, url);
  if (status != ASYNC_CLIENT_STATUS_OK) {
      async_client_destroy(client);
      return -1;
  }

  candidate->socket = client;

  /* Note: In a real implementation we would need to get the bound port here.
   * For now, we assume fixed port or use the one from connect. 
   * libuv uv_udp_getsockname would be replaced by some netcore API if needed. */
  candidate->port = 54321; /* Dummy port for now */

  return 0;
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
        stbsp_snprintf(cand->foundation, sizeof(cand->foundation), "%d",
                       ++agent->foundation_counter);
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
      stbsp_snprintf(cand->foundation, sizeof(cand->foundation), "%d", ++agent->foundation_counter);
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
 * STUN Gathering Callbacks
 * ============================================================================ */

static void on_stun_binding_response(turbo_stun_client_t *client, int status,
                                     const stun_mapped_address_t *mapped, void *user_data) {
  turbo_ice_agent_t *agent = (turbo_ice_agent_t *)user_data;

  agent->pending_stun_requests--;

  if (status == 0 && mapped && agent->local_candidate_count < ICE_MAX_CANDIDATES) {
    /* Create server-reflexive candidate */
    ice_candidate_t *cand = &agent->local_candidates[agent->local_candidate_count];
    memset(cand, 0, sizeof(*cand));

    cand->type = ICE_CANDIDATE_TYPE_SRFLX;
    cand->transport = ICE_TRANSPORT_UDP;
    cand->component_id = 1;
    cand->family = mapped->family == STUN_ADDR_FAMILY_IPV4 ? AF_INET : AF_INET6;
    strncpy(cand->ip, mapped->ip_str, sizeof(cand->ip) - 1);
    cand->port = mapped->port;
    cand->priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_SRFLX, 65534, 1);
    cand->is_local = 1;
    stbsp_snprintf(cand->foundation, sizeof(cand->foundation), "%d", ++agent->foundation_counter);
    generate_candidate_id(cand->id);

    /* Set related address (the host candidate) */
    if (agent->local_candidate_count > 0) {
      strncpy(cand->related_ip, agent->local_candidates[0].ip, sizeof(cand->related_ip) - 1);
      cand->related_port = agent->local_candidates[0].port;
    }

    /* Assign socket from STUN client - required for connectivity checks */
    cand->socket = client->async_client_handle;

    agent->local_candidate_count++;

    /* Notify callback */
    if (agent->callbacks.on_candidate) {
      agent->callbacks.on_candidate(agent, cand, agent->callbacks.user_data);
    }
  }

  /* Check if all STUN requests are complete */
  if (agent->pending_stun_requests == 0 && agent->pending_turn_requests == 0) {
    TLOG_INFO("All gathering requests complete");
    set_gathering_state(agent, ICE_GATHERING_COMPLETE);
  }

  (void)client;
}

static void gather_srflx_candidates(turbo_ice_agent_t *agent) {
  for (int i = 0; i < agent->config.stun_server_count && i < ICE_MAX_STUN_SERVERS; i++) {
    const char *url = agent->config.stun_servers[i].url;

    /* Parse stun:host:port */
    char host[256] = {0};
    uint16_t port = STUN_DEFAULT_PORT;

    const char *p = url;
    if (strncmp(p, "stun:", 5) == 0)
      p += 5;
    if (strncmp(p, "//", 2) == 0)
      p += 2;

    /* Extract host and port */
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

    stun_client_config_t stun_config = {
                                        .server_host = host,
                                        .server_port = port,
                                        .timeout_ms = 3000,
                                        .retries = 2};


    turbo_stun_client_t *stun = stun_client_create(&stun_config);
    if (stun) {
      agent->stun_clients[i] = stun;
      agent->pending_stun_requests++;
      stun_client_bind(stun, on_stun_binding_response, agent);
    }
  }
}

/* ============================================================================
 * TURN Gathering Callbacks
 * ============================================================================ */

static void on_turn_allocate_response(turbo_turn_client_t *client, int status,
                                      const turn_allocation_t *allocation, void *user_data) {
  turbo_ice_agent_t *agent = (turbo_ice_agent_t *)user_data;

  agent->pending_turn_requests--;

  if (status == 0 && allocation && agent->local_candidate_count < ICE_MAX_CANDIDATES) {
    /* Create relay candidate */
    ice_candidate_t *cand = &agent->local_candidates[agent->local_candidate_count];
    memset(cand, 0, sizeof(*cand));

    cand->type = ICE_CANDIDATE_TYPE_RELAY;
    cand->transport = ICE_TRANSPORT_UDP;
    cand->component_id = 1;
    cand->family = AF_INET; /* TURN allocation is IPv4 for now */
    strncpy(cand->ip, allocation->relayed_ip, sizeof(cand->ip) - 1);
    cand->port = allocation->relayed_port;
    cand->priority = ice_calculate_priority(ICE_CANDIDATE_TYPE_RELAY, 65533, 1);
    cand->is_local = 1;
    stbsp_snprintf(cand->foundation, sizeof(cand->foundation), "%d", ++agent->foundation_counter);
    generate_candidate_id(cand->id);

    /* Set related address (server-reflexive from TURN) */
    strncpy(cand->related_ip, allocation->mapped_ip, sizeof(cand->related_ip) - 1);
    cand->related_port = allocation->mapped_port;

    /* Store TURN client reference for sending data */
    /* Note: relay candidates don't have their own socket, they use TURN client */
    cand->socket = NULL;

    agent->local_candidate_count++;

    /* Notify callback */
    if (agent->callbacks.on_candidate) {
      agent->callbacks.on_candidate(agent, cand, agent->callbacks.user_data);
    }
  }

  /* Check if all gathering is complete */
  if (agent->pending_stun_requests == 0 && agent->pending_turn_requests == 0) {
    set_gathering_state(agent, ICE_GATHERING_COMPLETE);
  }

  (void)client;
}

static void gather_relay_candidates(turbo_ice_agent_t *agent) {
  for (int i = 0; i < agent->config.turn_server_count && i < ICE_MAX_TURN_SERVERS; i++) {
    const char *url = agent->config.turn_servers[i].url;
    const char *username = agent->config.turn_servers[i].username;
    const char *password = agent->config.turn_servers[i].credential;

    /* Parse turn:host:port */
    char host[256] = {0};
    uint16_t port = TURN_DEFAULT_PORT;

    const char *p = url;
    if (strncmp(p, "turn:", 5) == 0)
      p += 5;
    if (strncmp(p, "turns:", 6) == 0)
      p += 6; /* TURNS not supported yet */
    if (strncmp(p, "//", 2) == 0)
      p += 2;

    /* Extract host and port */
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


    turbo_turn_client_t *turn = turn_client_create(&turn_config);
    if (turn) {
      agent->turn_clients[i] = turn;
      agent->pending_turn_requests++;
      turn_client_allocate(turn, on_turn_allocate_response, agent);
    }
  }
}

/* ============================================================================
 * Gathering Timer
 * ============================================================================ */

static void on_gathering_timeout(turbo_timer_t *timer) {
  turbo_ice_agent_t *agent = (turbo_ice_agent_t *)turbo_timer_get_data(timer);

  if (!agent) {
    return;
  }

  turbo_mutex_lock(&agent->lock);

  /* Check if agent is being destroyed */
  if (agent->destroying) {
    turbo_mutex_unlock(&agent->lock);
    return;
  }

  /* Cancel pending STUN requests */
  for (int i = 0; i < ICE_MAX_STUN_SERVERS; i++) {
    if (agent->stun_clients[i]) {
      stun_client_cancel(agent->stun_clients[i]);
    }
  }

  /* Cancel pending TURN requests */
  for (int i = 0; i < ICE_MAX_TURN_SERVERS; i++) {
    if (agent->turn_clients[i]) {
      turn_client_destroy(agent->turn_clients[i]);
      agent->turn_clients[i] = NULL;
    }
  }

  agent->pending_stun_requests = 0;
  agent->pending_turn_requests = 0;
  set_gathering_state(agent, ICE_GATHERING_COMPLETE);

  turbo_mutex_unlock(&agent->lock);
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


turbo_ice_agent_t *ice_agent_create(const ice_config_t *config) {
  if (!config)
    return NULL;
  ensure_random_seeded();

  turbo_ice_agent_t *agent = calloc(1, sizeof(turbo_ice_agent_t));
  if (!agent)
    return NULL;

  turbo_mutex_init(&agent->lock);
  agent->config = *config;

  agent->state = ICE_STATE_NEW;
  agent->gathering_state = ICE_GATHERING_NEW;
  agent->role = config->is_controlling ? ICE_ROLE_CONTROLLING : ICE_ROLE_CONTROLLED;
  agent->tie_breaker = generate_random_u64();

  /* Generate local credentials */
  generate_random_string(agent->local_ufrag, 8);
  generate_random_string(agent->local_pwd, 24);

  /* Initialize timers */
  agent->gathering_timer = turbo_timer_create(NULL);
  if (agent->gathering_timer) turbo_timer_set_data(agent->gathering_timer, agent);
  
  agent->check_timer = turbo_timer_create(NULL);
  if (agent->check_timer) turbo_timer_set_data(agent->check_timer, agent);
  
  agent->keepalive_timer = turbo_timer_create(NULL);
  if (agent->keepalive_timer) turbo_timer_set_data(agent->keepalive_timer, agent);

  /* Initialize mDNS context if privacy mode enabled */
  if (config->use_mdns_candidates) {
    agent->mdns_ctx = mdns_create(NULL);
  }

  return agent;
}




void ice_agent_destroy(turbo_ice_agent_t *agent) {
  if (!agent) return;
  agent->destroying = 1;

  /* Stop and destroy timers */
  if (agent->gathering_timer) {
    turbo_timer_stop(agent->gathering_timer);
    turbo_timer_destroy(agent->gathering_timer);
    agent->gathering_timer = NULL;
  }
  if (agent->check_timer) {
    turbo_timer_stop(agent->check_timer);
    turbo_timer_destroy(agent->check_timer);
    agent->check_timer = NULL;
  }
  if (agent->keepalive_timer) {
    turbo_timer_stop(agent->keepalive_timer);
    turbo_timer_destroy(agent->keepalive_timer);
    agent->keepalive_timer = NULL;
  }

  /* Destroy STUN clients */
  for (int i = 0; i < ICE_MAX_STUN_SERVERS; i++) {
    if (agent->stun_clients[i]) {
      stun_client_destroy(agent->stun_clients[i]);
      agent->stun_clients[i] = NULL;
    }
  }

  /* Destroy TURN clients */
  for (int i = 0; i < ICE_MAX_TURN_SERVERS; i++) {
    if (agent->turn_clients[i]) {
      turn_client_destroy(agent->turn_clients[i]);
      agent->turn_clients[i] = NULL;
    }
  }

  /* Destroy candidate async clients */
  for (int i = 0; i < agent->local_candidate_count; i++) {
    ice_candidate_t *cand = &agent->local_candidates[i];
    if (cand->socket && cand->type == ICE_CANDIDATE_TYPE_HOST) {
        async_client_close((async_client_t *)cand->socket);
        async_client_destroy((async_client_t *)cand->socket);
        cand->socket = NULL;
    }
  }
  // SRFLX/RELAY sockets are owned by stun/turn clients and will be closed when those clients are destroyed.

  if (agent->mdns_ctx) {
    mdns_destroy(agent->mdns_ctx);
    agent->mdns_ctx = NULL;
  }

  turbo_mutex_destroy(&agent->lock);
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
  
  turbo_mutex_lock(&agent->lock);
  if (agent->state != ICE_STATE_NEW) {
    turbo_mutex_unlock(&agent->lock);
    return -2;
  }

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

  /* Start gathering timeout if any async gathering is in progress */
  if (agent->pending_stun_requests > 0 || agent->pending_turn_requests > 0) {
    turbo_timer_start(agent->gathering_timer, on_gathering_timeout,
                   agent->config.gathering_timeout_ms, 0);
  } else {
    /* No async gathering needed, complete immediately */
    set_gathering_state(agent, ICE_GATHERING_COMPLETE);
  }

  turbo_mutex_unlock(&agent->lock);
  return 0;
}

int ice_agent_add_remote_candidate(turbo_ice_agent_t *agent, const char *candidate_str) {
  if (!agent || !candidate_str)
    return -1;
  
  turbo_mutex_lock(&agent->lock);
  if (agent->remote_candidate_count >= ICE_MAX_CANDIDATES) {
    turbo_mutex_unlock(&agent->lock);
    return -2;
  }

  ice_candidate_t *cand = &agent->remote_candidates[agent->remote_candidate_count];
  memset(cand, 0, sizeof(*cand));

  if (ice_candidate_parse(candidate_str, cand) != 0) {
    turbo_mutex_unlock(&agent->lock);
    return -3;
  }

  cand->is_local = 0;
  agent->remote_candidate_count++;

  /* If already connecting, rebuild pairs to include this new candidate */
  if (agent->state == ICE_STATE_CONNECTING || agent->state == ICE_STATE_GATHERING) {
    rebuild_candidate_pairs(agent);
  }

  turbo_mutex_unlock(&agent->lock);
  return 0;
}

void ice_agent_end_of_candidates(turbo_ice_agent_t *agent) {
  if (agent) {
    turbo_mutex_lock(&agent->lock);
    agent->remote_candidates_complete = 1;
    turbo_mutex_unlock(&agent->lock);
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

static void send_connectivity_check(turbo_ice_agent_t *agent, ice_candidate_pair_t *pair,
                                    int nominate) {
  if (!agent || !pair || !pair->local || !pair->remote)
    return;
  if (!pair->local->socket)
    return;

  /* Generate transaction ID */
  stun_generate_transaction_id(&agent->current_txn_id);

  /* Build ICE STUN request */
  uint8_t stun_buf[STUN_MAX_MESSAGE_SIZE];
  int len = stun_build_ice_request(
      stun_buf, &agent->current_txn_id, agent->local_ufrag, agent->remote_ufrag, agent->remote_pwd,
      (uint32_t)pair->priority, agent->role == ICE_ROLE_CONTROLLING, agent->tie_breaker, nominate);

  if (len < 0) {
    TLOG_DEBUG("Failed to build STUN request");
    return;
  }

  TLOG_DEBUG("Outgoing BINDING REQUEST to {}:{} (txn: {})", pair->remote->ip,
            pair->remote->port, STUN_TRANSACTION_ID_LEN, agent->current_txn_id.id);

  /* Build remote URL */
  char url[512];
  stbsp_snprintf(url, sizeof(url), "udp://%s:%u", pair->remote->ip, pair->remote->port);

  /* Send */
  async_client_connect((async_client_t *)pair->local->socket, url);
  async_client_status_t sent = async_client_send((async_client_t *)pair->local->socket, (const char *)stun_buf, len);

  if (sent == ASYNC_CLIENT_STATUS_OK) {
    pair->state = ICE_PAIR_STATE_IN_PROGRESS;
    pair->check_count++;
    pair->last_check_time = turbo_monotonic_ms();
    TLOG_DEBUG("Check sent successfully");
  } else {
    TLOG_DEBUG("Check send failed: {}", (int)sent);
    pair->state = ICE_PAIR_STATE_FAILED;
  }
}


static void handle_stun_request(turbo_ice_agent_t *agent, const uint8_t *data, size_t len,
                                const struct sockaddr *from, ice_candidate_t *local_cand) {
  if (!agent || !data || !from || !local_cand)
    return;

  /* Parse the request */
  char username[256] = {0};
  uint32_t priority = 0;
  int use_candidate = 0;
  stun_transaction_id_t rx_txn_id;
  memcpy(rx_txn_id.id, data + 8, STUN_TRANSACTION_ID_LEN);

  if (stun_parse_ice_request(data, len, username, &priority, &use_candidate) != 0) {
    return;
  }

  /* Validate username: should be "local_ufrag:remote_ufrag" */
  char expected_username[256];
  stbsp_snprintf(expected_username, sizeof(expected_username), "%s:%s", agent->local_ufrag,
                 agent->remote_ufrag);
  if (strcmp(username, expected_username) != 0) {
    return; /* Username mismatch */
  }

  /* Validate MESSAGE-INTEGRITY */
  if (stun_validate_message_integrity(data, len, agent->local_pwd) != 0) {
    return; /* Invalid authentication */
  }

  /* Get sender's address */
  char remote_ip[64];
  uint16_t remote_port;
  if (from->sa_family == AF_INET) {
    struct sockaddr_in *addr4 = (struct sockaddr_in *)from;
    inet_ntop(AF_INET, &addr4->sin_addr, remote_ip, sizeof(remote_ip));
    remote_port = ntohs(addr4->sin_port);
  } else {
    return; /* IPv6 not supported yet */
  }

  /* Build and send response - signed with remote password because that's what the peer uses to
   * validate */
  TLOG_DEBUG("Incoming BINDING REQUEST from {}:{} (txn: {})", remote_ip, remote_port,
            STUN_TRANSACTION_ID_LEN, rx_txn_id.id);
  uint8_t resp_buf[STUN_MAX_MESSAGE_SIZE];
  stun_transaction_id_t txn_id;
  memcpy(txn_id.id, data + 8, STUN_TRANSACTION_ID_LEN);

  int resp_len =
      stun_build_ice_response(resp_buf, &txn_id, agent->remote_pwd, remote_ip, remote_port);
  if (resp_len > 0) {
    char url[512];
    stbsp_snprintf(url, sizeof(url), "udp://%s:%u", remote_ip, remote_port);
    async_client_connect((async_client_t *)local_cand->socket, url);
    async_client_send((async_client_t *)local_cand->socket, (const char *)resp_buf, resp_len);
  } else {

    TLOG_DEBUG("Failed to build STUN response");
  }


  /* Find or create the pair for this check */
  ice_candidate_pair_t *pair =
      find_pair_by_addresses(agent, local_cand->ip, local_cand->port, remote_ip, remote_port);

  if (pair) {
    /* If we received USE-CANDIDATE and we're controlled, mark as nominated */
    if (use_candidate && agent->role == ICE_ROLE_CONTROLLED) {
      pair->nominated = 1;
      if (pair->state == ICE_PAIR_STATE_SUCCEEDED) {
        /* This pair is now selected */
        agent->selected_pair = pair;
        set_state(agent, ICE_STATE_CONNECTED);
      }
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
    return; /* Not our transaction */
  }

  /* Validate MESSAGE-INTEGRITY (validated with our local password) */
  if (stun_validate_message_integrity(data, len, agent->local_pwd) != 0) {
    TLOG_DEBUG("STUN response MI validation failed");
    agent->checks_in_progress = 0;
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
      TLOG_DEBUG("Check succeeded for pair {}", agent->current_check_pair);
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
      }
    }

    /* If this pair is nominated (either by us or by peer), select it */
    if (pair->nominated) {
      agent->selected_pair = pair;
      set_state(agent, ICE_STATE_CONNECTED);
    }
  }

  agent->checks_in_progress = 0;
}

static void rebuild_candidate_pairs(turbo_ice_agent_t *agent) {
  if (!agent)
    return;

  int new_pairs_added = 0;


  for (int i = 0; i < agent->local_candidate_count; i++) {
    for (int j = 0; j < agent->remote_candidate_count; j++) {
      ice_candidate_t *local = &agent->local_candidates[i];
      ice_candidate_t *remote = &agent->remote_candidates[j];

      if (!local->socket)
        continue;
      if (local->component_id != remote->component_id)
        continue;
      if (local->transport != remote->transport)
        continue;
      if (local->family != remote->family)
        continue;

      /* Check if pair already exists */
      int exists = 0;
      for (int k = 0; k < agent->pair_count; k++) {
        if (agent->pairs[k].local == local && agent->pairs[k].remote == remote) {
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
  }
}

static void on_check_timer_impl(turbo_timer_t *timer, turbo_ice_agent_t *agent) {
  if (agent->destroying || agent->state != ICE_STATE_CONNECTING)
    return;

  /* Check for timeout */
  uint64_t now = turbo_monotonic_ms();
  uint64_t elapsed = now - agent->check_start_time;

  if (elapsed > (uint64_t)agent->config.connectivity_timeout_ms) {
    TLOG_INFO("Connectivity check timeout elapsed");
    /* Timeout - check if we have any valid pairs */
    if (agent->valid_pairs_count > 0) {
      if (!agent->selected_pair) {
        /* Pick the first succeeded pair as fallback */
        for (int i = 0; i < agent->pair_count; i++) {
          if (agent->pairs[i].state == ICE_PAIR_STATE_SUCCEEDED) {
            agent->selected_pair = &agent->pairs[i];
            TLOG_INFO("No pair nominated, picking pair %d as fallback", i);
            break;
          }
        }
      }

      if (agent->selected_pair) {
        set_state(agent, ICE_STATE_COMPLETED);
      } else {
        set_state(agent, ICE_STATE_FAILED);
      }
    } else {
      set_state(agent, ICE_STATE_FAILED);
    }
    turbo_timer_stop(timer);
    return;
  }


  /* If we have a check in progress, wait for response or timeout */
  if (agent->checks_in_progress) {
    /* Check if current check timed out (3 seconds) */
    if (agent->current_check_pair >= 0 && agent->current_check_pair < agent->pair_count) {
      ice_candidate_pair_t *pair = &agent->pairs[agent->current_check_pair];
      if (pair->last_check_time > 0 && now - pair->last_check_time > 3000) {
        /* Timeout - retry or mark failed */
        if (pair->check_count < 3) {
          send_connectivity_check(agent, pair, 0);
        } else {
          pair->state = ICE_PAIR_STATE_FAILED;
          agent->checks_in_progress = 0;
        }
      }
    }
    return;
  }

  /* Find next pair to check */
  for (int i = 0; i < agent->pair_count; i++) {
    ice_candidate_pair_t *pair = &agent->pairs[i];
    if (pair->state == ICE_PAIR_STATE_WAITING) {
      agent->current_check_pair = i;
      agent->checks_in_progress = 1;
      send_connectivity_check(agent, pair, 0);
      return;
    }
  }

  /* If controlling and we have valid pairs but nothing nominated yet, nominate best */
  if (agent->role == ICE_ROLE_CONTROLLING && agent->valid_pairs_count > 0 &&
      !agent->selected_pair) {
    for (int i = 0; i < agent->pair_count; i++) {
      if (agent->pairs[i].state == ICE_PAIR_STATE_SUCCEEDED && !agent->pairs[i].nominated) {
        agent->current_check_pair = i;
        agent->checks_in_progress = 1;
        send_connectivity_check(agent, &agent->pairs[i], 1);
        agent->pairs[i].nominated = 1;
        return;
      }
    }
  }

  /* All pairs checked - determine final state */
  if (agent->selected_pair) {
    set_state(agent, ICE_STATE_COMPLETED);
    turbo_timer_stop(timer);
  } else if (elapsed > (uint64_t)agent->config.connectivity_timeout_ms) {
    /* Total timeout elapsed - check if we have any valid pairs as fallback */
    TLOG_INFO("Connectivity check timeout elapsed after %llu ms", elapsed);
    if (agent->valid_pairs_count > 0) {
      /* Pick the best succeeded pair if none nominated */
      for (int i = 0; i < agent->pair_count; i++) {
        if (agent->pairs[i].state == ICE_PAIR_STATE_SUCCEEDED) {
          agent->selected_pair = &agent->pairs[i];
          TLOG_INFO("Timeout: picking pair %d as fallback", i);
          break;
        }
      }
      if (agent->selected_pair) {
        set_state(agent, ICE_STATE_COMPLETED);
      } else {
        set_state(agent, ICE_STATE_FAILED);
      }
    } else {
      set_state(agent, ICE_STATE_FAILED);
    }
    turbo_timer_stop(timer);
  } else if (agent->pair_count > 0) {
    /* We have pairs, check if all of them have failed */
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
      turbo_timer_stop(timer);
    }
  } else {
    /* No pairs yet and not timed out - just keep waiting for Trickle ICE */
    static uint64_t last_wait_log = 0;
    if (now - last_wait_log > 2000) {
      TLOG_INFO("Waiting for remote candidates (elapsed: %llu ms)...", elapsed);
      last_wait_log = now;
    }
  }
}

static void on_check_timer(turbo_timer_t *timer) {
  turbo_ice_agent_t *agent = (turbo_ice_agent_t *)turbo_timer_get_data(timer);
  if (!agent)
    return;

  turbo_mutex_lock(&agent->lock);
  on_check_timer_impl(timer, agent);
  turbo_mutex_unlock(&agent->lock);
}


int ice_agent_start_checks(turbo_ice_agent_t *agent) {
  if (!agent)
    return -1;
  
  turbo_mutex_lock(&agent->lock);
  if (agent->gathering_state != ICE_GATHERING_COMPLETE) {
    turbo_mutex_unlock(&agent->lock);
    return -2;
  }
  if (!agent->remote_credentials_set) {
    TLOG_INFO("Cannot start checks: remote credentials not set");
    turbo_mutex_unlock(&agent->lock);
    return -3;
  }

  if (agent->state == ICE_STATE_CONNECTING || agent->state == ICE_STATE_CONNECTED ||
      agent->state == ICE_STATE_COMPLETED) {
    /* Already started, just rebuild pairs to catch any newly added trickle candidates */
    rebuild_candidate_pairs(agent);
    turbo_mutex_unlock(&agent->lock);
    return 0;
  }

  TLOG_INFO("Starting checks with {} local and {} remote candidates",
           agent->local_candidate_count, agent->remote_candidate_count);

  /* Prepare sockets for connectivity checks (reuse STUN/TURN sockets) */
  for (int i = 0; i < agent->local_candidate_count; i++) {
    ice_candidate_t *cand = &agent->local_candidates[i];
    if (cand->socket) {
      /* Sockets are already managed by async_client instances and receiving data. */
    }
  }


  /* Build initial candidate pairs */
  rebuild_candidate_pairs(agent);

  if (agent->pair_count == 0) {
    TLOG_INFO("Starting CONNECTIVITY CHECKS with 0 pairs (waiting for remote candidates)");
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

  /* Start check timer (Ta interval = 50ms) */
  turbo_timer_start(agent->check_timer, on_check_timer, 0, ICE_DEFAULT_TA_INTERVAL);

  turbo_mutex_unlock(&agent->lock);
  return 0;
}


int ice_agent_send(turbo_ice_agent_t *agent, const void *data, size_t len) {
  if (!agent || !data || len == 0)
    return -1;

  turbo_mutex_lock(&agent->lock);

  if (agent->state != ICE_STATE_CONNECTED && agent->state != ICE_STATE_COMPLETED) {
    turbo_mutex_unlock(&agent->lock);
    return -2;
  }
  if (!agent->selected_pair) {
    turbo_mutex_unlock(&agent->lock);
    return -3;
  }

  ice_candidate_t *local = agent->selected_pair->local;
  ice_candidate_t *remote = agent->selected_pair->remote;

  if (!local->socket) {
    turbo_mutex_unlock(&agent->lock);
    return -4;
  }

  /* Build remote URL */
  char url[512];
  stbsp_snprintf(url, sizeof(url), "udp://%s:%u", remote->ip, remote->port);

  /* Send */
  async_client_status_t status = ASYNC_CLIENT_STATUS_INTERNAL_ERROR;
  if (local->type == ICE_CANDIDATE_TYPE_HOST || local->type == ICE_CANDIDATE_TYPE_SRFLX) {
      async_client_connect((async_client_t *)local->socket, url);
      status = async_client_send((async_client_t *)local->socket, (const char *)data, len);
  } else if (local->type == ICE_CANDIDATE_TYPE_RELAY) {
      /* RELAY candidates use TURN client */
      turbo_mutex_unlock(&agent->lock);
      return -5;
  } else {
      turbo_mutex_unlock(&agent->lock);
      return -6;
  }

  turbo_mutex_unlock(&agent->lock);
  return (status == ASYNC_CLIENT_STATUS_OK) ? 0 : -1;
}


ice_state_t ice_agent_get_state(turbo_ice_agent_t *agent) {
  if (!agent) return ICE_STATE_CLOSED;
  turbo_mutex_lock(&agent->lock);
  ice_state_t state = agent->state;
  turbo_mutex_unlock(&agent->lock);
  return state;
}

ice_gathering_state_t ice_agent_get_gathering_state(turbo_ice_agent_t *agent) {
  if (!agent) return ICE_GATHERING_NEW;
  turbo_mutex_lock(&agent->lock);
  ice_gathering_state_t state = agent->gathering_state;
  turbo_mutex_unlock(&agent->lock);
  return state;
}

int ice_agent_get_selected_pair(turbo_ice_agent_t *agent, ice_candidate_t *local_out,
                                ice_candidate_t *remote_out) {
  if (!agent)
    return -1;

  turbo_mutex_lock(&agent->lock);
  if (!agent->selected_pair) {
    turbo_mutex_unlock(&agent->lock);
    return -1;
  }

  if (local_out)
    *local_out = *agent->selected_pair->local;
  if (remote_out)
    *remote_out = *agent->selected_pair->remote;

  turbo_mutex_unlock(&agent->lock);
  return 0;
}

int ice_agent_get_local_candidate_count(turbo_ice_agent_t *agent) {
  if (!agent) return 0;
  turbo_mutex_lock(&agent->lock);
  int count = agent->local_candidate_count;
  turbo_mutex_unlock(&agent->lock);
  return count;
}

int ice_agent_get_local_candidate(turbo_ice_agent_t *agent, int index, ice_candidate_t *out) {
  if (!agent || !out)
    return -1;

  turbo_mutex_lock(&agent->lock);
  if (index < 0 || index >= agent->local_candidate_count) {
    turbo_mutex_unlock(&agent->lock);
    return -2;
  }

  *out = agent->local_candidates[index];
  turbo_mutex_unlock(&agent->lock);
  return 0;
}

void ice_agent_set_allow_loopback(turbo_ice_agent_t *agent, int allow) {
  if (agent) {
    agent->config.allow_loopback = allow;
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

  /* TODO: Parse raddr/rport for related address */

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

  int len = stbsp_snprintf(buf, (int)buf_len, "candidate:%s %d %s %u %s %u typ %s",
                           candidate->foundation, candidate->component_id, transport_str,
                           candidate->priority, address, candidate->port, type_str);

  /* Add raddr/rport if present */
  if (candidate->related_ip[0] != '\0') {
    len += stbsp_snprintf(buf + len, (int)(buf_len - len), " raddr %s rport %u", candidate->related_ip,
                          candidate->related_port);
  }

  return len;
}
