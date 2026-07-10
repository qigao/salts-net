/**
 * @file turbo_dns.c
 * @brief Unified DNS resolution using c-ares driven by native select().
 *
 * Drives c-ares entirely with platform-native sockets (select / WSAPoll)
 * — no libuv dependency.
 *
 * - Async: c-ares socket-state callback registers fds; a background thread
 *   runs select() and calls ares_process_fd().
 * - Sync:  same loop, but caller blocks on a condition variable.
 */
#include "turbo_dns.h"
#include "turbo_thread.h"
#include "tlog.h"
#include <ares.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "turbo_error.h"
#include <stdatomic.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define SOCK_INVALID INVALID_SOCKET
  typedef SOCKET dns_sock_t;
  #define DNS_SELECT(n,r,w,t) select((int)(n),(r),(w),NULL,(t))
  #define TURBO_DNS_CLOSE_SOCKET(s) closesocket(s)
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #define SOCK_INVALID (-1)
  typedef int dns_sock_t;
  #define DNS_SELECT(n,r,w,t) select((int)(n),(r),(w),NULL,(t))
  #define TURBO_DNS_CLOSE_SOCKET(s) close(s)
#endif

#include "turbo_str_view.h"

// =============================================================================
// Internal Constants
// =============================================================================

#define MAX_SOCKETS 64
#define MAX_DNS_SERVERS 8
#define DNS_TIMEOUT_MS 5000
#define DNS_TRIES 2
#define DNS_TRY_TIMEOUT_MS ((DNS_TIMEOUT_MS - 1000) / DNS_TRIES)
#define DNS_CACHE_CAPACITY 128
#define DNS_CACHE_TTL_MS (300ULL * 1000ULL)
#define DNS_ARES_QCACHE_MAX_TTL_SECONDS 300
#define DNS_CACHE_MAX_HOSTNAME 256
#define DNS_CACHE_MAX_RESULTS (TURBO_DNS_MAX_RESULTS * 2)

// =============================================================================
// Internal Types
// =============================================================================

/**
 * @brief c-ares integration context — no libuv, pure select() loop.
 */
typedef struct {
  ares_channel channel;
  struct {
    ares_socket_t fd;
    int readable;
    int writable;
  } sockets[MAX_SOCKETS];
  int socket_count;
  bool initialized;
  int processing;    /* Currently inside ares_process_fd */
  int needs_destroy; /* destroy_ares_context was called while processing */
  turbo_mutex_t mu;   /* guards sockets[] and channel */
} turbo_ares_t;

/* Dual-stack query coordination */
typedef struct turbo_dns_query_s {
  char *hostname;
  turbo_dns_cb callback;
  turbo_dns_results_cb results_callback;
  void *user_data;
  atomic_int ref_count;
  atomic_int delivered;
  int cancelled;
  int timed_out;
  int status_v4;
  int status_v6;
  turbo_ares_t *ares;
  turbo_dns_pref_t pref;
  int started_v4;
  int started_v6;
  turbo_dns_result_t results_v4[TURBO_DNS_MAX_RESULTS];
  turbo_dns_result_t results_v6[TURBO_DNS_MAX_RESULTS];
  size_t result_count_v4;
  size_t result_count_v6;
} turbo_dns_parent_query_t;

typedef struct {
  turbo_dns_parent_query_t *parent;
  int family;
} turbo_dns_child_query_t;

typedef struct {
  int in_use;
  char hostname[DNS_CACHE_MAX_HOSTNAME];
  turbo_dns_pref_t pref;
  uint64_t expires_at_ms;
  uint64_t last_used_ms;
  size_t count;
  turbo_dns_result_t results[DNS_CACHE_MAX_RESULTS];
} turbo_dns_cache_entry_t;

/* Sync resolution state */
typedef struct {
  int port;
  struct sockaddr_storage *result_addr;
  int *result_len;
  int error;
  volatile int done;
  turbo_mutex_t mu;
  turbo_cond_t  cond;
} turbo_dns_sync_state_t;

// =============================================================================
// Global State
// =============================================================================

static char g_dns_servers[MAX_DNS_SERVERS][46];
static int  g_dns_count   = 0;
static int  g_ares_lib_ref = 0;

static turbo_once_t  g_dns_init_once       = TURBO_ONCE_INIT;
static turbo_mutex_t g_dns_lock;
static int           g_dns_lock_initialized = 0;
static int           g_dns_refcount         = 0;
static turbo_dns_cache_entry_t g_dns_cache[DNS_CACHE_CAPACITY];

static void destroy_ares_context(turbo_ares_t *ctx);

static char *dns_hostname_dup(const char *hostname) {
  return tstr_v_to_cstr(tstr_v_from_cstr(hostname));
}

static void dns_hostname_free(char *hostname) {
  free(hostname);
}

// =============================================================================
// DNS cache
// =============================================================================

static int dns_cache_make_key(const char *hostname,
                              char out[DNS_CACHE_MAX_HOSTNAME]) {
  size_t len;

  if (!hostname || !out) return TURBO_EINVAL;
  len = strlen(hostname);
  if (len == 0 || len >= DNS_CACHE_MAX_HOSTNAME) return TURBO_EINVAL;

  for (size_t i = 0; i < len; i++) {
    out[i] = (char)tolower((unsigned char)hostname[i]);
  }
  out[len] = '\0';
  return 0;
}

static void dns_cache_clear_locked(void) {
  memset(g_dns_cache, 0, sizeof(g_dns_cache));
}

static int dns_cache_lookup(const char *hostname,
                            turbo_dns_pref_t pref,
                            turbo_dns_result_t results[DNS_CACHE_MAX_RESULTS],
                            size_t *count) {
  char key[DNS_CACHE_MAX_HOSTNAME];
  uint64_t now_ms;

  if (!results || !count) return 0;
  *count = 0;
  if (!g_dns_lock_initialized) return 0;
  if (dns_cache_make_key(hostname, key) != 0) return 0;

  now_ms = turbo_monotonic_ms();
  turbo_mutex_lock(&g_dns_lock);
  for (int i = 0; i < DNS_CACHE_CAPACITY; i++) {
    turbo_dns_cache_entry_t *entry = &g_dns_cache[i];
    if (!entry->in_use || entry->pref != pref) continue;
    if (strcmp(entry->hostname, key) != 0) continue;

    if (entry->expires_at_ms <= now_ms) {
      memset(entry, 0, sizeof(*entry));
      turbo_mutex_unlock(&g_dns_lock);
      return 0;
    }

    memcpy(results, entry->results, entry->count * sizeof(entry->results[0]));
    *count = entry->count;
    entry->last_used_ms = now_ms;
    turbo_mutex_unlock(&g_dns_lock);
    return 1;
  }
  turbo_mutex_unlock(&g_dns_lock);
  return 0;
}

static void dns_cache_store(const char *hostname,
                            turbo_dns_pref_t pref,
                            const turbo_dns_result_t *results,
                            size_t count) {
  char key[DNS_CACHE_MAX_HOSTNAME];
  uint64_t now_ms;
  int slot = -1;

  if (!results || count == 0 || !g_dns_lock_initialized) return;
  if (count > DNS_CACHE_MAX_RESULTS) count = DNS_CACHE_MAX_RESULTS;
  if (dns_cache_make_key(hostname, key) != 0) return;

  now_ms = turbo_monotonic_ms();
  turbo_mutex_lock(&g_dns_lock);
  for (int i = 0; i < DNS_CACHE_CAPACITY; i++) {
    turbo_dns_cache_entry_t *entry = &g_dns_cache[i];
    if (entry->in_use && entry->pref == pref &&
        strcmp(entry->hostname, key) == 0) {
      slot = i;
      break;
    }
    if (slot < 0 && (!entry->in_use || entry->expires_at_ms <= now_ms)) {
      slot = i;
    }
  }

  if (slot < 0) {
    uint64_t oldest_ms = g_dns_cache[0].last_used_ms;
    slot = 0;
    for (int i = 1; i < DNS_CACHE_CAPACITY; i++) {
      if (g_dns_cache[i].last_used_ms < oldest_ms) {
        oldest_ms = g_dns_cache[i].last_used_ms;
        slot = i;
      }
    }
  }

  turbo_dns_cache_entry_t *entry = &g_dns_cache[slot];
  memset(entry, 0, sizeof(*entry));
  entry->in_use = 1;
  strcpy(entry->hostname, key);
  entry->pref = pref;
  entry->expires_at_ms = now_ms + DNS_CACHE_TTL_MS;
  entry->last_used_ms = now_ms;
  entry->count = count;
  memcpy(entry->results, results, count * sizeof(results[0]));
  turbo_mutex_unlock(&g_dns_lock);
}

// =============================================================================
// Address helpers (no libuv)
// =============================================================================

static int dns_sockaddr_length(const struct sockaddr_storage *addr) {
  if (!addr) return TURBO_EINVAL;
  if (addr->ss_family == AF_INET)  return (int)sizeof(struct sockaddr_in);
  if (addr->ss_family == AF_INET6) return (int)sizeof(struct sockaddr_in6);
  return TURBO_EAI_FAMILY;
}

/* Parse IP string → sockaddr_storage (no libuv). */
int turbo_dns_parse_address(const char *address, int port,
                             struct sockaddr_storage *addr) {
  if (!address || !addr) return TURBO_EINVAL;

  struct sockaddr_in *a4 = (struct sockaddr_in *)addr;
  memset(a4, 0, sizeof(*a4));
  if (inet_pton(AF_INET, address, &a4->sin_addr) == 1) {
    a4->sin_family = AF_INET;
    a4->sin_port   = htons((unsigned short)port);
    return 0;
  }

  struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)addr;
  memset(a6, 0, sizeof(*a6));
  if (inet_pton(AF_INET6, address, &a6->sin6_addr) == 1) {
    a6->sin6_family = AF_INET6;
    a6->sin6_port   = htons((unsigned short)port);
    return 0;
  }

  return TURBO_EAI_NONAME;
}

static int dns_parse_ip_address(const char *ip, int port,
                                struct sockaddr_storage *addr, int *addr_len) {
  int rc = turbo_dns_parse_address(ip, port, addr);
  if (rc != 0) return rc;
  int len = dns_sockaddr_length(addr);
  if (len < 0) return len;
  if (addr_len) *addr_len = len;
  return 0;
}

static int dns_format_ip_address(const struct sockaddr_storage *addr,
                                 char *buf, size_t buf_sz) {
  if (!addr || !buf || buf_sz == 0) return TURBO_EINVAL;
  if (addr->ss_family == AF_INET) {
    return inet_ntop(AF_INET,
                     &((const struct sockaddr_in *)addr)->sin_addr,
                     buf, (socklen_t)buf_sz)
               ? 0 : TURBO_EAI_FAIL;
  }
  if (addr->ss_family == AF_INET6) {
    return inet_ntop(AF_INET6,
                     &((const struct sockaddr_in6 *)addr)->sin6_addr,
                     buf, (socklen_t)buf_sz)
               ? 0 : TURBO_EAI_FAIL;
  }
  return TURBO_EAI_FAMILY;
}

// =============================================================================
// c-ares Library Ref
// =============================================================================

static void dns_release_ares_library_ref(void) {
  if (!g_dns_lock_initialized) return;
  turbo_mutex_lock(&g_dns_lock);
  if (g_ares_lib_ref > 0 && --g_ares_lib_ref == 0)
    ares_library_cleanup();
  turbo_mutex_unlock(&g_dns_lock);
}

// =============================================================================
// c-ares Socket-State Callback (no libuv — just track fd interest)
// =============================================================================

static void on_ares_sock_state_cb(void *data, ares_socket_t fd,
                               int readable, int writable) {
  turbo_ares_t *ctx = (turbo_ares_t *)data;
  if (!ctx || !ctx->initialized) return;

  turbo_mutex_lock(&ctx->mu);

  /* Find existing slot */
  int idx = -1;
  for (int i = 0; i < MAX_SOCKETS; i++) {
    if (ctx->sockets[i].fd == fd) { idx = i; break; }
  }

  if (!readable && !writable) {
    /* c-ares closed this socket */
    if (idx >= 0) {
      ctx->sockets[idx].fd       = ARES_SOCKET_BAD;
      ctx->sockets[idx].readable = 0;
      ctx->sockets[idx].writable = 0;
      ctx->socket_count--;
    }
    turbo_mutex_unlock(&ctx->mu);
    return;
  }

  if (idx < 0) {
    /* New socket — find a free slot */
    for (int i = 0; i < MAX_SOCKETS; i++) {
      if (ctx->sockets[i].fd == ARES_SOCKET_BAD) { idx = i; break; }
    }
    if (idx < 0) { turbo_mutex_unlock(&ctx->mu); return; }
    ctx->sockets[idx].fd = fd;
    ctx->socket_count++;
  }

  ctx->sockets[idx].readable = readable;
  ctx->sockets[idx].writable = writable;

  turbo_mutex_unlock(&ctx->mu);
}

// =============================================================================
// Drive c-ares with select() — called from the caller's loop
// =============================================================================

/**
 * @brief Run one poll iteration for this ares context.
 *
 * Builds fd_sets from the socket table, calls select() with the timeout
 * c-ares requests, then calls ares_process_fd() for all ready fds.
 *
 * @param ctx    ares context
 * @param max_ms maximum milliseconds to wait; 0 = nowait
 * @return 1 if there are still active sockets, 0 if idle
 */
static int ares_poll_once(turbo_ares_t *ctx, int max_ms) {
  fd_set rfds, wfds;
  FD_ZERO(&rfds);
  FD_ZERO(&wfds);
  int nfds = 0;

  turbo_mutex_lock(&ctx->mu);
  if (ctx->socket_count == 0) {
    turbo_mutex_unlock(&ctx->mu);
    return 0;
  }

  for (int i = 0; i < MAX_SOCKETS; i++) {
    ares_socket_t fd = ctx->sockets[i].fd;
    if (fd == ARES_SOCKET_BAD) continue;
    if (ctx->sockets[i].readable) FD_SET((dns_sock_t)fd, &rfds);
    if (ctx->sockets[i].writable) FD_SET((dns_sock_t)fd, &wfds);
    if ((int)fd + 1 > nfds) nfds = (int)fd + 1;
  }
  turbo_mutex_unlock(&ctx->mu);

  /* Ask c-ares how long to wait */
  struct timeval tv_buf, *tvp;
  {
    struct timeval mt;
    mt.tv_sec  = max_ms / 1000;
    mt.tv_usec = (max_ms % 1000) * 1000;
    tvp = ares_timeout(ctx->channel, &mt, &tv_buf);
  }

  int n = DNS_SELECT(nfds, &rfds, &wfds, tvp);
  if (n <= 0) {
    /* Timeout — notify c-ares so it can retry or fail */
    ares_process_fd(ctx->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
    return (ctx->socket_count > 0) ? 1 : 0;
  }

  turbo_mutex_lock(&ctx->mu);
  for (int i = 0; i < MAX_SOCKETS; i++) {
    ares_socket_t fd = ctx->sockets[i].fd;
    if (fd == ARES_SOCKET_BAD) continue;
    ares_socket_t rfd = FD_ISSET((dns_sock_t)fd, &rfds) ? fd : ARES_SOCKET_BAD;
    ares_socket_t wfd = FD_ISSET((dns_sock_t)fd, &wfds) ? fd : ARES_SOCKET_BAD;
    if (rfd != ARES_SOCKET_BAD || wfd != ARES_SOCKET_BAD) {
      ctx->processing++;
      turbo_mutex_unlock(&ctx->mu);
      ares_process_fd(ctx->channel, rfd, wfd);
      turbo_mutex_lock(&ctx->mu);
      ctx->processing--;
    }
  }

  if (ctx->processing == 0 && ctx->needs_destroy) {
    ctx->needs_destroy = 0;
    turbo_mutex_unlock(&ctx->mu);
    destroy_ares_context(ctx);
    return 0;
  }
  turbo_mutex_unlock(&ctx->mu);

  return (ctx->socket_count > 0) ? 1 : 0;
}

// =============================================================================
// ares context init / cleanup
// =============================================================================

static void dns_copy_custom_servers(struct in_addr *dns_addrs,
                                    char local_servers[][46],
                                    int *valid_dns) {
  *valid_dns = 0;
  turbo_mutex_lock(&g_dns_lock);
  for (int i = 0; i < g_dns_count; i++) {
    strcpy(local_servers[i], g_dns_servers[i]);
    if (inet_pton(AF_INET, local_servers[i], &dns_addrs[*valid_dns]) == 1)
      (*valid_dns)++;
  }
  turbo_mutex_unlock(&g_dns_lock);
}

static int init_ares_context(turbo_ares_t **out_ctx) {
  int status, valid_dns;
  struct in_addr dns_addrs[MAX_DNS_SERVERS];
  char local_servers[MAX_DNS_SERVERS][46];

  if (!g_dns_lock_initialized) return TURBO_EBUSY;

  turbo_ares_t *ctx = calloc(1, sizeof(turbo_ares_t));
  if (!ctx) return TURBO_ENOMEM;

  for (int i = 0; i < MAX_SOCKETS; i++)
    ctx->sockets[i].fd = ARES_SOCKET_BAD;

  turbo_mutex_init(&ctx->mu);
  if (!ctx->mu) { free(ctx); return TURBO_ENOMEM; }

  turbo_mutex_lock(&g_dns_lock);
  if (g_ares_lib_ref == 0) {
    status = ares_library_init(ARES_LIB_INIT_ALL);
    if (status != ARES_SUCCESS) {
      turbo_mutex_unlock(&g_dns_lock);
      TLOG_DEBUG("ares_library_init failed: {}", ares_strerror(status));
      turbo_mutex_destroy(&ctx->mu);
      free(ctx);
      return TURBO_EAI_FAIL;
    }
  }
  g_ares_lib_ref++;
  turbo_mutex_unlock(&g_dns_lock);

  dns_copy_custom_servers(dns_addrs, local_servers, &valid_dns);

  struct ares_options options = {0};
  options.sock_state_cb_data = ctx;
  options.sock_state_cb      = on_ares_sock_state_cb;
  options.timeout            = DNS_TRY_TIMEOUT_MS;
  options.tries              = DNS_TRIES;
  int init_flags = ARES_OPT_SOCK_STATE_CB | ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES;
#ifdef ARES_OPT_QUERY_CACHE
  options.qcache_max_ttl = DNS_ARES_QCACHE_MAX_TTL_SECONDS;
  init_flags |= ARES_OPT_QUERY_CACHE;
#endif

  if (valid_dns > 0) {
    options.servers  = dns_addrs;
    options.nservers = valid_dns;
    init_flags |= ARES_OPT_SERVERS;
    for (int i = 0; i < valid_dns; i++)
      TLOG_INFO("Using custom DNS server: {}", local_servers[i]);
  }

  status = ares_init_options(&ctx->channel, &options, init_flags);
  if (status != ARES_SUCCESS) {
    TLOG_DEBUG("ares_init_options failed: {}", ares_strerror(status));
    dns_release_ares_library_ref();
    turbo_mutex_destroy(&ctx->mu);
    free(ctx);
    return TURBO_EAI_FAIL;
  }

  if (valid_dns == 0)
    ares_set_servers_csv(ctx->channel, "8.8.8.8,8.8.4.4");

  ctx->initialized = true;
  *out_ctx = ctx;
  return 0;
}

static void destroy_ares_context(turbo_ares_t *ctx) {
  if (!ctx) return;

  turbo_mutex_lock(&ctx->mu);
  if (ctx->processing > 0) {
    ctx->needs_destroy = 1;
    turbo_mutex_unlock(&ctx->mu);
    return;
  }

  if (ctx->initialized) {
    ares_destroy(ctx->channel);
    ctx->initialized = false;
  }
  turbo_mutex_unlock(&ctx->mu);

  dns_release_ares_library_ref();
  turbo_mutex_destroy(&ctx->mu);
  free(ctx);
}

// =============================================================================
// Async query helpers
// =============================================================================

static void release_parent_ref(turbo_dns_parent_query_t *parent) {
  turbo_dns_result_t ordered[TURBO_DNS_MAX_RESULTS * 2];
  size_t count = 0;

  if (atomic_fetch_sub(&parent->ref_count, 1) > 1) return;

  if (parent->results_callback) {
    if (parent->pref == TURBO_DNS_IPV6_ONLY || parent->pref == TURBO_DNS_PREFER_IPV6) {
      for (size_t i = 0; i < parent->result_count_v6 && count < TURBO_DNS_MAX_RESULTS * 2; i++) {
        ordered[count++] = parent->results_v6[i];
      }
      for (size_t i = 0; i < parent->result_count_v4 && count < TURBO_DNS_MAX_RESULTS * 2; i++) {
        ordered[count++] = parent->results_v4[i];
      }
    } else {
      for (size_t i = 0; i < parent->result_count_v4 && count < TURBO_DNS_MAX_RESULTS * 2; i++) {
        ordered[count++] = parent->results_v4[i];
      }
      for (size_t i = 0; i < parent->result_count_v6 && count < TURBO_DNS_MAX_RESULTS * 2; i++) {
        ordered[count++] = parent->results_v6[i];
      }
    }

    if (count > 0) {
      dns_cache_store(parent->hostname, parent->pref, ordered, count);
      parent->results_callback(parent->hostname, ordered, count, 0, parent->user_data);
    } else {
      int err = (parent->status_v4 != ARES_SUCCESS && parent->status_v4 != 0) ? parent->status_v4 :
                (parent->status_v6 != ARES_SUCCESS && parent->status_v6 != 0) ? parent->status_v6 :
                ARES_ENODATA;
      TLOG_DEBUG("DNS failed for {}: {}", parent->hostname, ares_strerror(err));
      parent->results_callback(parent->hostname, NULL, 0, err, parent->user_data);
    }
  } else if (!atomic_load(&parent->delivered)) {
    int err = (parent->status_v4 != ARES_SUCCESS && parent->status_v4 != 0) ? parent->status_v4 :
              (parent->status_v6 != ARES_SUCCESS && parent->status_v6 != 0) ? parent->status_v6 :
              ARES_ENODATA;
    TLOG_DEBUG("DNS failed for {}: {}", parent->hostname, ares_strerror(err));
    parent->callback(parent->hostname, NULL, err, parent->user_data);
  }

  if (parent->ares && parent->ares->initialized)
    destroy_ares_context(parent->ares);

  dns_hostname_free(parent->hostname);
  free(parent);
}

static void dns_dual_addrinfo_cb(void *arg, int status, int timeouts,
                                 struct ares_addrinfo *result) {
  (void)timeouts;
  turbo_dns_child_query_t *child  = (turbo_dns_child_query_t *)arg;
  int                      family = child ? child->family : 0;
  turbo_dns_parent_query_t *parent = child ? child->parent : NULL;

  if (child) free(child);
  if (!parent) { if (result) ares_freeaddrinfo(result); return; }

  if (status == ARES_ECANCELLED && parent->timed_out) {
    status = TURBO_ETIMEDOUT;
  }

  if (parent->results_callback) {
    if (status == ARES_SUCCESS && result && result->nodes) {
      struct ares_addrinfo_node *node;

      for (node = result->nodes; node != NULL; node = node->ai_next) {
        turbo_dns_result_t *slot = NULL;
        size_t *slot_count = NULL;

        if (node->ai_family == AF_INET) {
          if (parent->result_count_v4 >= TURBO_DNS_MAX_RESULTS) continue;
          slot = &parent->results_v4[parent->result_count_v4];
          slot_count = &parent->result_count_v4;
          inet_ntop(AF_INET,
                    &((struct sockaddr_in *)node->ai_addr)->sin_addr,
                    slot->ip, sizeof(slot->ip));
        } else if (node->ai_family == AF_INET6) {
          if (parent->result_count_v6 >= TURBO_DNS_MAX_RESULTS) continue;
          slot = &parent->results_v6[parent->result_count_v6];
          slot_count = &parent->result_count_v6;
          inet_ntop(AF_INET6,
                    &((struct sockaddr_in6 *)node->ai_addr)->sin6_addr,
                    slot->ip, sizeof(slot->ip));
        }

        if (slot && slot->ip[0] != '\0') {
          slot->family = node->ai_family;
          (*slot_count)++;
          TLOG_DEBUG("DNS: {} -> {}", parent->hostname, slot->ip);
        }
      }
    } else {
      if (family == AF_INET) parent->status_v4 = status;
      else if (family == AF_INET6) parent->status_v6 = status;
    }
  } else if (!parent->delivered && status == ARES_SUCCESS && result && result->nodes) {
    char ip[INET6_ADDRSTRLEN] = {0};
    struct ares_addrinfo_node *node = result->nodes;

    if (node->ai_family == AF_INET) {
      inet_ntop(AF_INET,
                &((struct sockaddr_in *)node->ai_addr)->sin_addr,
                ip, sizeof(ip));
    } else if (node->ai_family == AF_INET6) {
      inet_ntop(AF_INET6,
                &((struct sockaddr_in6 *)node->ai_addr)->sin6_addr,
                ip, sizeof(ip));
    }

    if (ip[0] != '\0') {
      TLOG_DEBUG("DNS: {} -> {}", parent->hostname, ip);
      if (!atomic_exchange(&parent->delivered, 1)) {
        parent->callback(parent->hostname, ip, 0, parent->user_data);
      }
    } else {
      if (family == AF_INET) parent->status_v4 = status;
      else if (family == AF_INET6) parent->status_v6 = status;
    }
  } else {
    if (family == AF_INET)  parent->status_v4 = status;
    else if (family == AF_INET6) parent->status_v6 = status;
  }

  if (result) ares_freeaddrinfo(result);
  release_parent_ref(parent);
}

static bool is_ip_address(const char *host) {
  struct sockaddr_storage tmp;
  return turbo_dns_parse_address(host, 80, &tmp) == 0;
}

static int dns_pref_wants_ipv4(turbo_dns_pref_t pref) {
  return pref == TURBO_DNS_IPV4_ONLY || pref == TURBO_DNS_ANY ||
         pref == TURBO_DNS_PREFER_IPV6;
}

static int dns_pref_wants_ipv6(turbo_dns_pref_t pref) {
  return pref == TURBO_DNS_IPV6_ONLY || pref == TURBO_DNS_ANY ||
         pref == TURBO_DNS_PREFER_IPV6;
}

static turbo_dns_child_query_t *dns_alloc_child_query(turbo_dns_parent_query_t *parent,
                                                      int family) {
  turbo_dns_child_query_t *child = malloc(sizeof(*child));
  if (!child) return NULL;
  child->parent = parent;
  child->family = family;
  return child;
}

static void dns_submit_child_query(turbo_dns_parent_query_t *parent,
                                   const char *hostname,
                                   turbo_dns_child_query_t *child) {
  int family = child->family;
  atomic_fetch_add(&parent->ref_count, 1);
  if (family == AF_INET) parent->started_v4 = 1;
  else if (family == AF_INET6) parent->started_v6 = 1;

  struct ares_addrinfo_hints hints = {0};
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  ares_getaddrinfo(parent->ares->channel, hostname, NULL,
                   &hints, dns_dual_addrinfo_cb, child);
}

static int dns_start_queries(turbo_dns_parent_query_t *parent,
                             const char *hostname) {
  turbo_dns_child_query_t *child_v4 = NULL;
  turbo_dns_child_query_t *child_v6 = NULL;

  if (dns_pref_wants_ipv4(parent->pref)) {
    child_v4 = dns_alloc_child_query(parent, AF_INET);
    if (!child_v4) {
      return TURBO_ENOMEM;
    }
  }

  if (dns_pref_wants_ipv6(parent->pref)) {
    child_v6 = dns_alloc_child_query(parent, AF_INET6);
    if (!child_v6) {
      free(child_v4);
      return TURBO_ENOMEM;
    }
  }

  if (!child_v4 && !child_v6) {
    return TURBO_EINVAL;
  }

  if (child_v4) {
    dns_submit_child_query(parent, hostname, child_v4);
  }
  if (child_v6) {
    dns_submit_child_query(parent, hostname, child_v6);
  }
  return 0;
}

// =============================================================================
// Once / Init / Cleanup
// =============================================================================

static void dns_init_once(void) {
  turbo_mutex_init(&g_dns_lock);
  if (g_dns_lock != NULL)
    g_dns_lock_initialized = 1;
}

int turbo_dns_init(void) {
  turbo_once(&g_dns_init_once, dns_init_once);
  if (!g_dns_lock_initialized) return TURBO_EBUSY;
  turbo_mutex_lock(&g_dns_lock);
  g_dns_refcount++;
  turbo_mutex_unlock(&g_dns_lock);
  return 0;
}

void turbo_dns_cleanup(void) {
  if (!g_dns_lock_initialized) return;
  turbo_mutex_lock(&g_dns_lock);
  if (g_dns_refcount > 0) {
    g_dns_refcount--;
    if (g_dns_refcount == 0) {
      dns_cache_clear_locked();
    }
  }
  turbo_mutex_unlock(&g_dns_lock);
}

// =============================================================================
// Public API: Synchronous Resolution
// =============================================================================

static void sync_dns_results_callback(const char *hostname,
                                      const turbo_dns_result_t *results,
                                      size_t count,
                                      int status,
                                      void *user_data) {
  (void)hostname;
  turbo_dns_sync_state_t *state = (turbo_dns_sync_state_t *)user_data;

  turbo_mutex_lock(&state->mu);
  if (!state->done) {
    if (status == 0 && results && count > 0) {
      struct sockaddr_storage addr;
      int addr_len = 0;
      if (dns_parse_ip_address(results[0].ip, state->port, &addr, &addr_len) == 0) {
        memcpy(state->result_addr, &addr, sizeof(addr));
        *state->result_len = addr_len;
        state->error = 0;
      } else {
        state->error = TURBO_EAI_FAIL;
      }
    } else {
      state->error = status ? status : TURBO_EAI_FAIL;
    }

    state->done = 1;
    turbo_cond_signal(&state->cond);
  }
  turbo_mutex_unlock(&state->mu);
}

int turbo_dns_resolve(void *loop_unused, const char *host, int port,
                      struct sockaddr_storage *out, int *out_len) {
  (void)loop_unused;
  if (!host || !out || !out_len) return TURBO_EINVAL;

  /* Fast path: already an IP */
  if (dns_parse_ip_address(host, port, out, out_len) == 0) return 0;

  turbo_once(&g_dns_init_once, dns_init_once);

  turbo_dns_result_t cached_results[DNS_CACHE_MAX_RESULTS];
  size_t cached_count = 0;
  if (dns_cache_lookup(host, TURBO_DNS_ANY, cached_results, &cached_count) &&
      cached_count > 0) {
    return dns_parse_ip_address(cached_results[0].ip, port, out, out_len);
  }

  turbo_ares_t *ares_ctx = NULL;
  int err = init_ares_context(&ares_ctx);
  if (err != 0) return err;

  turbo_dns_sync_state_t state = {0};
  state.port        = port;
  state.result_addr = out;
  state.result_len  = out_len;
  state.error       = TURBO_EAI_FAIL;
  turbo_mutex_init(&state.mu);
  turbo_cond_init(&state.cond);

  turbo_dns_parent_query_t *parent = calloc(1, sizeof(*parent));
  if (!parent) {
    destroy_ares_context(ares_ctx);
    turbo_mutex_destroy(&state.mu);
    turbo_cond_destroy(&state.cond);
    return TURBO_ENOMEM;
  }

  parent->hostname  = dns_hostname_dup(host);
  parent->results_callback = sync_dns_results_callback;
  parent->user_data = &state;
  parent->pref      = TURBO_DNS_ANY;
  parent->ares      = ares_ctx;
  atomic_init(&parent->ref_count, 1);
  atomic_init(&parent->delivered, 1);

  if (!parent->hostname) {
    free(parent);
    destroy_ares_context(ares_ctx);
    turbo_mutex_destroy(&state.mu);
    turbo_cond_destroy(&state.cond);
    return TURBO_ENOMEM;
  }

  err = dns_start_queries(parent, host);
  if (err != 0) {
    release_parent_ref(parent);
    turbo_mutex_destroy(&state.mu);
    turbo_cond_destroy(&state.cond);
    return err;
  }

  release_parent_ref(parent); /* release initial ref */

  /* Drive select loop until done or timeout */
  int64_t deadline_ms = DNS_TIMEOUT_MS;
  while (deadline_ms > 0) {
    int poll_ms = (deadline_ms > 100) ? 100 : (int)deadline_ms;

    turbo_mutex_lock(&state.mu);
    if (state.done) { turbo_mutex_unlock(&state.mu); break; }
    turbo_mutex_unlock(&state.mu);

    ares_poll_once(ares_ctx, poll_ms);
    deadline_ms -= poll_ms;
  }

  if (!state.done) {
    turbo_mutex_lock(&state.mu);
    state.done = 1;
    state.error = TURBO_ETIMEDOUT;
    turbo_mutex_unlock(&state.mu);
    parent->timed_out = 1;
    parent->cancelled = 1;
    if (ares_ctx && ares_ctx->initialized) {
      ares_cancel(ares_ctx->channel);
    }
  }

  turbo_mutex_destroy(&state.mu);
  turbo_cond_destroy(&state.cond);
  /* ares_ctx is freed inside release_parent_ref when last child done */
  return state.error;
}

int turbo_dns_resolve_sync(const char *hostname, char *ip_buffer,
                            size_t buffer_size, int family_pref) {
  if (!hostname || !ip_buffer || buffer_size == 0) return TURBO_EINVAL;
  ip_buffer[0] = '\0';

  struct sockaddr_storage result_addr;
  int rc = dns_parse_ip_address(hostname, 80, &result_addr, NULL);
  if (rc != TURBO_EAI_FAIL) {
    if (rc != 0) return rc;
  } else {
    int result_len = 0;
    rc = turbo_dns_resolve(NULL, hostname, 80, &result_addr, &result_len);
    if (rc != 0) return rc;
  }

  if (result_addr.ss_family == AF_INET && family_pref == 6) return TURBO_EAI_FAMILY;
  if (result_addr.ss_family == AF_INET6 && family_pref == 4) return TURBO_EAI_FAMILY;
  if (result_addr.ss_family != AF_INET && result_addr.ss_family != AF_INET6)
    return TURBO_EAI_FAMILY;

  return dns_format_ip_address(&result_addr, ip_buffer, buffer_size);
}

// =============================================================================
// Public API: Asynchronous Resolution
// =============================================================================

/**
 * @brief Async DNS context — bundles ares_ctx + background driver thread.
 *
 * The async path runs a tiny background thread that calls ares_poll_once()
 * in a loop until the query completes.  The caller's callback is invoked
 * from that thread.
 */
typedef struct {
  turbo_dns_parent_query_t *parent;
  volatile int           running;
} async_driver_t;

static void async_driver_thread(void *arg) {
  async_driver_t *drv = (async_driver_t *)arg;
  turbo_dns_parent_query_t *parent = drv->parent;
  turbo_ares_t *ares = parent ? parent->ares : NULL;
  int64_t deadline = DNS_TIMEOUT_MS;

  while (drv->running && deadline > 0) {
    if (!ares || !ares->initialized) break;
    int poll_ms = (deadline > 50) ? 50 : (int)deadline;
    int active  = ares_poll_once(ares, poll_ms);
    deadline -= poll_ms;
    if (!active) break;
  }

  if (drv->running && deadline <= 0 && parent && ares && ares->initialized) {
    parent->timed_out = 1;
    parent->cancelled = 1;
    ares_cancel(ares->channel);
  }

  /* c-ares can occasionally go idle with one family query still pending.
     Force-cancel any leftovers so every child query drops its parent ref. */
  if (drv->running && parent != NULL && ares != NULL && ares->initialized &&
      !parent->cancelled &&
      atomic_load_explicit(&parent->ref_count, memory_order_acquire) > 1) {
    parent->cancelled = 1;
    ares_cancel(ares->channel);
  }

  drv->running = 0;
  if (parent) {
    release_parent_ref(parent);
  }
  free(drv);
}

int turbo_dns_resolve_async2(void *loop_unused, const char *hostname,
                             turbo_dns_pref_t pref, turbo_dns_cb callback,
                             void *user_data, turbo_dns_query_t **out_query) {
  (void)loop_unused;
  if (out_query) *out_query = NULL;
  if (!hostname || !callback) return TURBO_EINVAL;

  /* Fast path: already an IP */
  if (is_ip_address(hostname)) {
    callback(hostname, hostname, 0, user_data);
    return 0;
  }

  turbo_once(&g_dns_init_once, dns_init_once);

  turbo_dns_result_t cached_results[DNS_CACHE_MAX_RESULTS];
  size_t cached_count = 0;
  if (dns_cache_lookup(hostname, pref, cached_results, &cached_count) &&
      cached_count > 0) {
    callback(hostname, cached_results[0].ip, 0, user_data);
    return 0;
  }

  turbo_ares_t *ares_ctx = NULL;
  int err = init_ares_context(&ares_ctx);
  if (err != 0) return err;

  turbo_dns_parent_query_t *parent = calloc(1, sizeof(*parent));
  if (!parent) { destroy_ares_context(ares_ctx); return TURBO_ENOMEM; }

  parent->hostname  = dns_hostname_dup(hostname);
  parent->callback  = callback;
  parent->user_data = user_data;
  parent->pref      = pref;
  parent->ares      = ares_ctx;
  atomic_init(&parent->ref_count, 1); /* Initial ref */
  atomic_init(&parent->delivered, 0);

  if (!parent->hostname) {
    free(parent);
    destroy_ares_context(ares_ctx);
    return TURBO_ENOMEM;
  }

  if (out_query) {
    *out_query = parent;
  }

  err = dns_start_queries(parent, hostname);
  if (err != 0) {
    if (out_query) {
      *out_query = NULL;
    }
    release_parent_ref(parent);
    return err;
  }

  /* Start background driver thread */
  async_driver_t *drv = malloc(sizeof(*drv));
  turbo_thread_t driver_thread = NULL;
  if (!drv) {
    if (out_query) {
      *out_query = NULL; 
    }
    destroy_ares_context(ares_ctx);
    dns_hostname_free(parent->hostname);
    free(parent);
    return TURBO_ENOMEM;
  }
  drv->parent  = parent;
  drv->running = 1;
  atomic_fetch_add(&parent->ref_count, 1); /* driver thread owns parent/ares lifetime */

  if (turbo_thread_create(&driver_thread, async_driver_thread, drv) != 0) {
    release_parent_ref(parent);
    free(drv);
    if (out_query) { *out_query = NULL; }
    destroy_ares_context(ares_ctx);
    dns_hostname_free(parent->hostname);
    free(parent);
    return TURBO_EAI_FAIL;
  }
  turbo_thread_destroy(&driver_thread); /* detach so it cleans itself up */

  release_parent_ref(parent); /* release initial ref */
  TLOG_DEBUG("Started async DNS lookup for {}", hostname);
  return 0;
}

int turbo_dns_resolve_async(void *loop, const char *hostname, turbo_dns_pref_t pref,
                             turbo_dns_cb callback, void *user_data) {
  return turbo_dns_resolve_async2(loop, hostname, pref, callback, user_data, NULL);
}

int turbo_dns_resolve_async_results2(void *loop_unused, const char *hostname,
                                     turbo_dns_pref_t pref, turbo_dns_results_cb callback,
                                     void *user_data, turbo_dns_query_t **out_query) {
  (void)loop_unused;
  if (out_query) *out_query = NULL;
  if (!hostname || !callback) return TURBO_EINVAL;

  if (is_ip_address(hostname)) {
    turbo_dns_result_t result;

    memset(&result, 0, sizeof(result));
    strncpy(result.ip, hostname, sizeof(result.ip) - 1);
    result.family = strchr(hostname, ':') ? AF_INET6 : AF_INET;
    callback(hostname, &result, 1, 0, user_data);
    return 0;
  }

  turbo_once(&g_dns_init_once, dns_init_once);

  turbo_dns_result_t cached_results[DNS_CACHE_MAX_RESULTS];
  size_t cached_count = 0;
  if (dns_cache_lookup(hostname, pref, cached_results, &cached_count) &&
      cached_count > 0) {
    callback(hostname, cached_results, cached_count, 0, user_data);
    return 0;
  }

  turbo_ares_t *ares_ctx = NULL;
  int err = init_ares_context(&ares_ctx);
  if (err != 0) return err;

  turbo_dns_parent_query_t *parent = calloc(1, sizeof(*parent));
  if (!parent) {
    destroy_ares_context(ares_ctx);
    return TURBO_ENOMEM;
  }

  parent->hostname = dns_hostname_dup(hostname);
  parent->results_callback = callback;
  parent->user_data = user_data;
  parent->pref = pref;
  parent->ares = ares_ctx;
  atomic_init(&parent->ref_count, 1);
  atomic_init(&parent->delivered, 1);

  if (!parent->hostname) {
    free(parent);
    destroy_ares_context(ares_ctx);
    return TURBO_ENOMEM;
  }

  if (out_query) {
    *out_query = parent;
  }

  err = dns_start_queries(parent, hostname);
  if (err != 0) {
    if (out_query) {
      *out_query = NULL;
    }
    release_parent_ref(parent);
    return err;
  }

  async_driver_t *drv = malloc(sizeof(*drv));
  turbo_thread_t driver_thread = NULL;
  if (!drv) {
    if (out_query) {
      *out_query = NULL;
    }
    destroy_ares_context(ares_ctx);
    dns_hostname_free(parent->hostname);
    free(parent);
    return TURBO_ENOMEM;
  }
  drv->parent = parent;
  drv->running = 1;
  atomic_fetch_add(&parent->ref_count, 1); /* driver thread owns parent/ares lifetime */

  if (turbo_thread_create(&driver_thread, async_driver_thread, drv) != 0) {
    release_parent_ref(parent);
    free(drv);
    if (out_query) {
      *out_query = NULL;
    }
    destroy_ares_context(ares_ctx);
    dns_hostname_free(parent->hostname);
    free(parent);
    return TURBO_EAI_FAIL;
  }
  turbo_thread_destroy(&driver_thread);

  release_parent_ref(parent); /* release initial ref */
  TLOG_DEBUG("Started async DNS multi-result lookup for {}", hostname);
  return 0;
}

int turbo_dns_resolve_async_results(void *loop, const char *hostname,
                                    turbo_dns_pref_t pref,
                                    turbo_dns_results_cb callback,
                                    void *user_data) {
  return turbo_dns_resolve_async_results2(loop, hostname, pref, callback, user_data, NULL);
}

void turbo_dns_cancel(turbo_dns_query_t *query) {
  if (!query) return;
  turbo_dns_parent_query_t *parent = (turbo_dns_parent_query_t *)query;
  if (!parent->cancelled && parent->ares && parent->ares->initialized) {
    parent->cancelled = 1;
    ares_cancel(parent->ares->channel);
  }
}

// =============================================================================
// Public API: DNS Server Configuration
// =============================================================================

int turbo_dns_set_servers(const char *servers[], int count) {
  if (!servers || count < 0 || count > MAX_DNS_SERVERS) return TURBO_EINVAL;
  if (!g_dns_lock_initialized) return TURBO_EBUSY;

  turbo_mutex_lock(&g_dns_lock);
  g_dns_count = 0;
  for (int i = 0; i < count; i++) {
    if (!servers[i]) continue;
    size_t len = strlen(servers[i]);
    if (len >= sizeof(g_dns_servers[i])) continue;
    strcpy(g_dns_servers[g_dns_count], servers[i]);
    g_dns_count++;
    TLOG_INFO("Added DNS server[{}]: {}", g_dns_count - 1, servers[i]);
  }
  int final_count = g_dns_count;
  dns_cache_clear_locked();
  turbo_mutex_unlock(&g_dns_lock);

  TLOG_INFO("Configured {} DNS servers", final_count);
  return 0;
}

int turbo_dns_get_servers(char servers[][46], int max_servers, int *count) {
  if (!servers || !count) return TURBO_EINVAL;
  if (!g_dns_lock_initialized) return TURBO_EBUSY;

  turbo_mutex_lock(&g_dns_lock);
  int n = (g_dns_count > max_servers) ? max_servers : g_dns_count;
  for (int i = 0; i < n; i++)
    strcpy(servers[i], g_dns_servers[i]);
  *count = n;
  turbo_mutex_unlock(&g_dns_lock);
  return 0;
}
