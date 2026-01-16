/**
 * Unified DNS resolution using c-ares integrated with libuv.
 *
 * Provides both synchronous (blocking) and asynchronous (callback) APIs.
 * - Async: Uses c-ares with libuv event loop integration
 * - Sync: Wraps async API with temporary event loop
 */
#include "turbo_dns.h"
#include "turbo_logger.h"
#include <uv.h>
#include <ares.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

// =============================================================================
// Internal Constants
// =============================================================================

#define MAX_SOCKETS 64
#define MAX_DNS_SERVERS 8
#define DNS_TIMEOUT_MS 5000

// =============================================================================
// Internal Types (hidden from header)
// =============================================================================

// c-ares + libuv integration context
typedef struct {
    uv_loop_t *loop;
    ares_channel channel;
    uv_timer_t timer;
    struct {
        ares_socket_t fd;
        uv_poll_t poll;
        int events;
    } sockets[MAX_SOCKETS];
    int socket_count;
    bool initialized;
    int closing;
    int open_handles;
} turbo_ares_t;

/* strdup with padding to avoid ASan false positives from stb_sprintf 4-byte reads */
static char *strdup_padded(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    size_t alloc_size = ((len + 1 + 7) / 8) * 8;  /* Round up to 8-byte boundary */
    char *copy = (char *)calloc(1, alloc_size);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

// Dual-stack query coordination
typedef struct turbo_dns_parent_query_s {
    char *hostname;
    turbo_dns_cb callback;
    void *user_data;
    int ref_count;
    int delivered;
    int status_v4;
    int status_v6;
    turbo_ares_t *ares;
    turbo_dns_pref_t pref;
    int started_v4;
    int started_v6;
} turbo_dns_parent_query_t;

typedef struct {
    turbo_dns_parent_query_t *parent;
    int family;
} turbo_dns_child_query_t;

// Sync resolution state
typedef struct {
    uv_loop_t *loop;
    int port;
    struct sockaddr_storage *result_addr;
    int *result_len;
    int error;
    int done;
    uv_timer_t *timeout_timer;
    int timed_out;
} turbo_dns_sync_state_t;

// =============================================================================
// Global State
// =============================================================================

static char g_dns_servers[MAX_DNS_SERVERS][46];
static int g_dns_count = 0;
static int g_ares_lib_ref = 0;

static turbo_once_t g_dns_init_once = TURBO_ONCE_INIT;
static turbo_mutex_t g_dns_lock;
static int g_dns_lock_initialized = 0;
static int g_dns_refcount = 0;

// =============================================================================
// Internal: Handle Cleanup
// =============================================================================

static void on_uv_handle_closed(uv_handle_t *h) {
    turbo_ares_t *c = (turbo_ares_t *)h->data;
    if (c && c->open_handles > 0) {
        c->open_handles--;
        if (c->open_handles == 0 && c->closing) {
            if (c->initialized) {
                ares_destroy(c->channel);
                c->initialized = false;
            }
            if (g_ares_lib_ref > 0) {
                g_ares_lib_ref--;
                if (g_ares_lib_ref == 0) {
                    ares_library_cleanup();
                }
            }
            free(c);
        }
    }
}

// =============================================================================
// Internal: Socket Management
// =============================================================================

static int find_socket(turbo_ares_t *ctx, ares_socket_t fd) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (ctx->sockets[i].fd == fd) return i;
    }
    return -1;
}

static int find_free_slot(turbo_ares_t *ctx) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (ctx->sockets[i].fd == ARES_SOCKET_BAD) return i;
    }
    return -1;
}

// =============================================================================
// Internal: c-ares Callbacks
// =============================================================================

static void timer_cb(uv_timer_t *timer) {
    turbo_ares_t *ctx = (turbo_ares_t *)timer->data;
    if (!ctx || !ctx->initialized || ctx->closing) return;
    ares_process_fd(ctx->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
}

static void poll_cb(uv_poll_t *poll, int status, int events) {
    (void)status;
    turbo_ares_t *ctx = (turbo_ares_t *)poll->data;
    if (!ctx || !ctx->initialized || ctx->closing) return;

    ares_socket_t rfd = ARES_SOCKET_BAD, wfd = ARES_SOCKET_BAD;
    if (events & UV_READABLE) rfd = poll->socket;
    if (events & UV_WRITABLE) wfd = poll->socket;

    ares_process_fd(ctx->channel, rfd, wfd);

    struct timeval tv;
    struct timeval *tvp = ares_timeout(ctx->channel, NULL, &tv);
    if (tvp && (tvp->tv_sec > 0 || tvp->tv_usec > 0)) {
        uint64_t timeout = tvp->tv_sec * 1000 + tvp->tv_usec / 1000;
        if (timeout > 0) {
            uv_timer_start(&ctx->timer, timer_cb, timeout, 0);
        }
    }
}

static void uv_ares_sock_state_cb(void *data, ares_socket_t socket_fd,
                                   int readable, int writable) {
    turbo_ares_t *ctx = (turbo_ares_t *)data;
    if (!ctx || ctx->closing) return;

    int idx = find_socket(ctx, socket_fd);

    if (!readable && !writable) {
        if (idx >= 0) {
            uv_poll_stop(&ctx->sockets[idx].poll);
            uv_close((uv_handle_t *)&ctx->sockets[idx].poll, on_uv_handle_closed);
            ctx->sockets[idx].fd = ARES_SOCKET_BAD;
            ctx->sockets[idx].events = 0;
            if (ctx->socket_count > 0) ctx->socket_count--;
        }
        return;
    }

    if (idx < 0) {
        if (ctx->socket_count >= MAX_SOCKETS) return;
        int free_idx = find_free_slot(ctx);
        if (free_idx < 0) return;
        idx = free_idx;
        ctx->sockets[idx].fd = socket_fd;
        ctx->sockets[idx].events = 0;
        uv_poll_init_socket(ctx->loop, &ctx->sockets[idx].poll, socket_fd);
        ctx->sockets[idx].poll.data = ctx;
        ctx->socket_count++;
        ctx->open_handles++;
    }

    int events = 0;
    if (readable) events |= UV_READABLE;
    if (writable) events |= UV_WRITABLE;

    if (events != ctx->sockets[idx].events) {
        ctx->sockets[idx].events = events;
        uv_poll_start(&ctx->sockets[idx].poll, events, poll_cb);
    }
}

// =============================================================================
// Internal: Async DNS Resolution
// =============================================================================

static void release_parent_ref(turbo_dns_parent_query_t *parent) {
    if (--parent->ref_count > 0) return;

    if (!parent->delivered) {
        int err = parent->status_v4 != ARES_SUCCESS && parent->status_v4 != 0
                      ? parent->status_v4
                      : (parent->status_v6 != ARES_SUCCESS && parent->status_v6 != 0
                             ? parent->status_v6
                             : ARES_ENODATA);
        TLOG_ERROR("DNS failed for {}: {}", parent->hostname, ares_strerror(err));
        parent->callback(parent->hostname, NULL, err, parent->user_data);
    }

    if (parent->ares && parent->ares->initialized) {
        turbo_ares_t *ctx = parent->ares;
        ctx->initialized = false;
        ctx->closing = 1;

        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (ctx->sockets[i].fd != ARES_SOCKET_BAD) {
                uv_poll_stop(&ctx->sockets[i].poll);
                uv_close((uv_handle_t *)&ctx->sockets[i].poll, on_uv_handle_closed);
                ctx->sockets[i].fd = ARES_SOCKET_BAD;
                ctx->sockets[i].events = 0;
            }
        }
        ctx->socket_count = 0;
        uv_close((uv_handle_t *)&ctx->timer, on_uv_handle_closed);
    }
    free(parent->hostname);
    free(parent);
}

static void dns_dual_addrinfo_cb(void *arg, int status, int timeouts,
                                  struct ares_addrinfo *result) {
    (void)timeouts;
    turbo_dns_child_query_t *child = (turbo_dns_child_query_t *)arg;
    int family = child ? child->family : 0;
    turbo_dns_parent_query_t *parent = child ? child->parent : NULL;

    if (child) free(child);

    if (!parent) {
        if (result) ares_freeaddrinfo(result);
        return;
    }

    if (!parent->delivered && status == ARES_SUCCESS && result && result->nodes) {
        char ip[INET6_ADDRSTRLEN] = {0};
        struct ares_addrinfo_node *node = result->nodes;

        if (node->ai_family == AF_INET) {
            struct sockaddr_in *addr4 = (struct sockaddr_in *)node->ai_addr;
            inet_ntop(AF_INET, &addr4->sin_addr, ip, sizeof(ip));
        } else if (node->ai_family == AF_INET6) {
            struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)node->ai_addr;
            inet_ntop(AF_INET6, &addr6->sin6_addr, ip, sizeof(ip));
        }

        if (ip[0] != '\0') {
            TLOG_DEBUG("DNS: {} -> {}  ", parent->hostname, ip);
            parent->callback(parent->hostname, ip, 0, parent->user_data);
            parent->delivered = 1;
        }
    } else {
        if (family == AF_INET) parent->status_v4 = status;
        else if (family == AF_INET6) parent->status_v6 = status;
    }

    if (result) ares_freeaddrinfo(result);

    // Handle PREFER_IPV6 fallback
    if (!parent->delivered && parent->pref == TURBO_DNS_PREFER_IPV6 &&
        family == AF_INET6 && status != ARES_SUCCESS && !parent->started_v4) {
        turbo_dns_child_query_t *child4 = malloc(sizeof(*child4));
        if (child4) {
            child4->parent = parent;
            child4->family = AF_INET;
            parent->started_v4 = 1;
            parent->ref_count++;

            struct ares_addrinfo_hints hints = {0};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            ares_getaddrinfo(parent->ares->channel, parent->hostname, NULL,
                             &hints, dns_dual_addrinfo_cb, child4);
        }
    }

    release_parent_ref(parent);
}

static bool is_ip_address(const char *hostname) {
    struct sockaddr_storage addr;
    return turbo_dns_parse_address(hostname, 80, &addr) == 0;
}

static int init_ares_context(uv_loop_t *loop, turbo_ares_t **out_ctx) {
    turbo_ares_t *ctx = calloc(1, sizeof(turbo_ares_t));
    if (!ctx) return UV_ENOMEM;

    ctx->loop = loop;
    ctx->open_handles = 0;

    for (int i = 0; i < MAX_SOCKETS; i++) {
        ctx->sockets[i].fd = ARES_SOCKET_BAD;
        ctx->sockets[i].events = 0;
    }

    int status = ARES_SUCCESS;
    if (g_ares_lib_ref == 0) {
        status = ares_library_init(ARES_LIB_INIT_ALL);
        if (status != ARES_SUCCESS) {
            TLOG_ERROR("ares_library_init failed: {}   ", ares_strerror(status));
            free(ctx);
            return UV_EAI_FAIL;
        }
    }

    struct ares_options options = {0};
    options.sock_state_cb_data = ctx;
    options.sock_state_cb = uv_ares_sock_state_cb;
    int init_flags = ARES_OPT_SOCK_STATE_CB;

    struct in_addr dns_addrs[MAX_DNS_SERVERS];
    if (g_dns_count > 0) {
        int valid_dns = 0;
        for (int i = 0; i < g_dns_count; i++) {
            if (inet_pton(AF_INET, g_dns_servers[i], &dns_addrs[valid_dns]) == 1) {
                TLOG_INFO("Using custom DNS server: {}    ", g_dns_servers[i]);
                valid_dns++;
            }
        }
        if (valid_dns > 0) {
            options.servers = dns_addrs;
            options.nservers = valid_dns;
            init_flags |= ARES_OPT_SERVERS;
        }
    }

    uv_timer_init(loop, &ctx->timer);
    ctx->timer.data = ctx;
    ctx->open_handles = 1;

    status = ares_init_options(&ctx->channel, &options, init_flags);
    if (status != ARES_SUCCESS) {
        TLOG_ERROR("ares_init_options failed: {}   ", ares_strerror(status));
        ctx->closing = 1;
        uv_close((uv_handle_t *)&ctx->timer, on_uv_handle_closed);
        if (g_ares_lib_ref == 0) ares_library_cleanup();
        return UV_EAI_FAIL;
    }

    ctx->initialized = true;
    g_ares_lib_ref++;
    *out_ctx = ctx;
    return 0;
}

// =============================================================================
// Internal: Sync Resolution Helpers
// =============================================================================

static void dns_init_once(void) {
    turbo_mutex_init(&g_dns_lock);
    if (g_dns_lock != NULL)
        g_dns_lock_initialized = 1;
}

static void sync_timeout_cb(uv_timer_t *timer) {
    turbo_dns_sync_state_t *state = (turbo_dns_sync_state_t *)timer->data;
    state->timed_out = 1;
    state->error = UV_ETIMEDOUT;
}

static void sync_dns_callback(const char *hostname, const char *ip,
                               int status, void *user_data) {
    (void)hostname;
    turbo_dns_sync_state_t *state = (turbo_dns_sync_state_t *)user_data;

    if (state->timeout_timer) {
        uv_timer_stop(state->timeout_timer);
    }

    if (status == 0 && ip) {
        struct sockaddr_storage addr;
        memset(&addr, 0, sizeof(addr));

        if (turbo_dns_parse_address(ip, state->port, &addr) == 0) {
            memcpy(state->result_addr, &addr, sizeof(addr));
            if (addr.ss_family == AF_INET) {
                *state->result_len = sizeof(struct sockaddr_in);
            } else if (addr.ss_family == AF_INET6) {
                *state->result_len = sizeof(struct sockaddr_in6);
            } else {
                state->error = UV_EAI_FAMILY;
            }
            state->error = 0;
        } else {
            state->error = UV_EAI_FAIL;
        }
    } else {
        state->error = status ? status : UV_EAI_FAIL;
    }

    state->done = 1;
}

static void force_close_cb(uv_handle_t *h, void *arg) {
    (void)arg;
    if (!uv_is_closing(h)) {
        uv_close(h, NULL);
    }
}

// =============================================================================
// Public API: Lifecycle
// =============================================================================

int turbo_dns_init(void) {
    turbo_once(&g_dns_init_once, dns_init_once);
    if (!g_dns_lock_initialized) return UV_EBUSY;

    turbo_mutex_lock(&g_dns_lock);
    g_dns_refcount++;
    turbo_mutex_unlock(&g_dns_lock);
    return 0;
}

void turbo_dns_cleanup(void) {
    if (!g_dns_lock_initialized) return;

    turbo_mutex_lock(&g_dns_lock);
    if (g_dns_refcount > 0) g_dns_refcount--;
    turbo_mutex_unlock(&g_dns_lock);
}

// =============================================================================
// Public API: Synchronous Resolution
// =============================================================================

int turbo_dns_resolve(void *loop, const char *host, int port,
                      struct sockaddr_storage *out, int *out_len) {
    if (!host || !out || !out_len) return UV_EINVAL;

    // Fast path: already IP address
    if (turbo_dns_parse_address(host, port, out) == 0) {
        if (out->ss_family == AF_INET) {
            *out_len = sizeof(struct sockaddr_in);
        } else if (out->ss_family == AF_INET6) {
            *out_len = sizeof(struct sockaddr_in6);
        } else {
            return UV_EAI_FAMILY;
        }
        return 0;
    }

    // Need DNS resolution
    uv_loop_t temp_loop;
    uv_loop_t *use_loop = (uv_loop_t *)loop;
    int cleanup_loop = 0;

    if (!use_loop) {
        if (uv_loop_init(&temp_loop) != 0) return UV_EAI_FAIL;
        use_loop = &temp_loop;
        cleanup_loop = 1;
    }

    turbo_dns_sync_state_t state = {0};
    state.loop = use_loop;
    state.port = port;
    state.result_addr = out;
    state.result_len = out_len;
    state.error = UV_EAI_FAIL;

    uv_timer_t timeout_timer;
    if (uv_timer_init(use_loop, &timeout_timer) == 0) {
        timeout_timer.data = &state;
        state.timeout_timer = &timeout_timer;
        uv_timer_start(&timeout_timer, sync_timeout_cb, DNS_TIMEOUT_MS, 0);
    }

    int rc = turbo_dns_resolve_async(use_loop, host, TURBO_DNS_ANY,
                                      sync_dns_callback, &state);
    if (rc != 0) {
        if (state.timeout_timer) {
            uv_timer_stop(state.timeout_timer);
            uv_close((uv_handle_t *)state.timeout_timer, NULL);
            uv_run(use_loop, UV_RUN_NOWAIT);
        }
        if (cleanup_loop) uv_loop_close(use_loop);
        return rc;
    }

    while (!state.done && !state.timed_out) {
        if (uv_run(use_loop, UV_RUN_ONCE) == 0) break;
    }

    if (state.done && !state.timed_out) {
        for (int i = 0; i < 10 && uv_loop_alive(use_loop); i++) {
            uv_run(use_loop, UV_RUN_NOWAIT);
        }
    }

    if (state.timeout_timer && !uv_is_closing((uv_handle_t *)state.timeout_timer)) {
        uv_close((uv_handle_t *)state.timeout_timer, NULL);
        for (int i = 0; i < 10 && uv_loop_alive(use_loop); i++) {
            uv_run(use_loop, UV_RUN_NOWAIT);
        }
    }

    if (cleanup_loop) {
        int attempts = 0;
        while (uv_loop_close(use_loop) == UV_EBUSY && attempts < 100) {
            uv_run(use_loop, UV_RUN_NOWAIT);
            attempts++;
        }
        if (attempts >= 100) {
            uv_walk(use_loop, force_close_cb, NULL);
            uv_run(use_loop, UV_RUN_NOWAIT);
            uv_loop_close(use_loop);
        }
    }

    return state.error;
}

int turbo_dns_resolve_sync(const char *hostname, char *ip_buffer,
                           size_t buffer_size, int family_pref) {
    if (!hostname || !ip_buffer || buffer_size == 0) return -3;

    ip_buffer[0] = '\0';

    // Fast path: already IP address
    struct sockaddr_storage addr;
    if (turbo_dns_parse_address(hostname, 80, &addr) == 0) {
        if (addr.ss_family == AF_INET) {
            if (family_pref == 6) return -4;
            struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
            if (uv_ip4_name(addr4, ip_buffer, buffer_size) != 0) return -5;
        } else if (addr.ss_family == AF_INET6) {
            if (family_pref == 4) return -6;
            struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
            if (uv_ip6_name(addr6, ip_buffer, buffer_size) != 0) return -7;
        } else {
            return -8;
        }
        return 0;
    }

    // DNS resolution
    struct sockaddr_storage result_addr;
    int result_len = 0;

    int rc = turbo_dns_resolve(NULL, hostname, 80, &result_addr, &result_len);
    if (rc != 0) return rc;

    if (result_addr.ss_family == AF_INET) {
        if (family_pref == 6) return -4;
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&result_addr;
        if (uv_ip4_name(addr4, ip_buffer, buffer_size) != 0) return -5;
    } else if (result_addr.ss_family == AF_INET6) {
        if (family_pref == 4) return -6;
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&result_addr;
        if (uv_ip6_name(addr6, ip_buffer, buffer_size) != 0) return -7;
    } else {
        return -8;
    }

    return 0;
}

// =============================================================================
// Public API: Asynchronous Resolution
// =============================================================================

int turbo_dns_resolve_async(void *loop, const char *hostname,
                            turbo_dns_pref_t pref, turbo_dns_cb callback,
                            void *user_data) {
    if (!loop || !hostname || !callback) return UV_EINVAL;

    // Fast path: already IP address
    if (is_ip_address(hostname)) {
        callback(hostname, hostname, 0, user_data);
        return 0;
    }

    turbo_ares_t *ares_ctx = NULL;
    int err = init_ares_context((uv_loop_t *)loop, &ares_ctx);
    if (err != 0) return err;

    turbo_dns_parent_query_t *parent = calloc(1, sizeof(*parent));
    if (!parent) {
        ares_ctx->closing = 1;
        ares_ctx->initialized = false;
        uv_close((uv_handle_t *)&ares_ctx->timer, on_uv_handle_closed);
        return UV_ENOMEM;
    }

    parent->hostname = strdup_padded(hostname);
    parent->callback = callback;
    parent->user_data = user_data;
    parent->pref = pref;
    parent->ares = ares_ctx;

    if (!parent->hostname) {
        ares_ctx->closing = 1;
        ares_ctx->initialized = false;
        uv_close((uv_handle_t *)&ares_ctx->timer, on_uv_handle_closed);
        free(parent);
        return UV_ENOMEM;
    }

    // Start queries based on preference
    parent->ref_count = 1; /* Hold reference to prevent premature cleanup */

    if (pref == TURBO_DNS_IPV4_ONLY || pref == TURBO_DNS_ANY) {
        turbo_dns_child_query_t *child4 = malloc(sizeof(*child4));
        if (child4) {
            child4->parent = parent;
            child4->family = AF_INET;
            parent->ref_count++;
            parent->started_v4 = 1;

            struct ares_addrinfo_hints hints = {0};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            ares_getaddrinfo(ares_ctx->channel, hostname, NULL, &hints,
                             dns_dual_addrinfo_cb, child4);
        }
    }

    if (pref == TURBO_DNS_IPV6_ONLY || pref == TURBO_DNS_ANY ||
        pref == TURBO_DNS_PREFER_IPV6) {
        turbo_dns_child_query_t *child6 = malloc(sizeof(*child6));
        if (child6) {
            child6->parent = parent;
            child6->family = AF_INET6;
            parent->ref_count++;
            parent->started_v6 = 1;

            struct ares_addrinfo_hints hints = {0};
            hints.ai_family = AF_INET6;
            hints.ai_socktype = SOCK_STREAM;
            ares_getaddrinfo(ares_ctx->channel, hostname, NULL, &hints,
                             dns_dual_addrinfo_cb, child6);
        }
    }

    // Start timer
    struct timeval tv;
    struct timeval *tvp = ares_timeout(ares_ctx->channel, NULL, &tv);
    if (tvp && (tvp->tv_sec > 0 || tvp->tv_usec > 0)) {
        uint64_t timeout = tvp->tv_sec * 1000 + tvp->tv_usec / 1000;
        if (timeout > 0) {
            uv_timer_start(&ares_ctx->timer, timer_cb, timeout, 0);
        }
    }

    TLOG_DEBUG("Started DNS lookup for {}  ", hostname);
    release_parent_ref(parent); /* Release initial reference */
    return 0;
}

// =============================================================================
// Public API: DNS Server Configuration
// =============================================================================

int turbo_dns_set_servers(const char *servers[], int count) {
    if (!servers || count < 0 || count > MAX_DNS_SERVERS) return UV_EINVAL;

    g_dns_count = 0;
    for (int i = 0; i < count; i++) {
        if (!servers[i]) continue;
        size_t len = strlen(servers[i]);
        if (len >= sizeof(g_dns_servers[i])) continue;

        strcpy(g_dns_servers[g_dns_count], servers[i]);
        g_dns_count++;
        TLOG_INFO("Added DNS server[{}]: {}   ", g_dns_count - 1, servers[i]);
    }

    TLOG_INFO("Configured {} DNS servers      ", g_dns_count);
    return 0;
}

int turbo_dns_get_servers(char servers[][46], int max_servers, int *count) {
    if (!servers || !count) return UV_EINVAL;

    int copy_count = (g_dns_count > max_servers) ? max_servers : g_dns_count;
    for (int i = 0; i < copy_count; i++) {
        strcpy(servers[i], g_dns_servers[i]);
    }

    *count = copy_count;
    return 0;
}

// =============================================================================
// Public API: Utilities
// =============================================================================

int turbo_dns_parse_address(const char *address, int port,
                            struct sockaddr_storage *addr) {
    if (!address || !addr) return UV_EINVAL;

    struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
    if (uv_ip4_addr(address, port, addr4) == 0) return 0;

    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
    if (uv_ip6_addr(address, port, addr6) == 0) return 0;

    return UV_EAI_NONAME;
}
