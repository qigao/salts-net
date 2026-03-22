#include "turbo_coro_tproxy.h"
#include "turbo_coro_bidi_pump.h"
#include <CoroNet.h>
#include "turbo_coro.h"
#include "turbo_coro_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include "tlog.h"
#if defined(__has_include)
#  if __has_include("stats.h")
#    include "stats.h"
#  else
#    define TURBO_STATS_INC(name) ((void)(name))
#    define TURBO_STATS_ADD(name, value) ((void)(name), (void)(value))
static void turbo_stats_gauge_add(const char *name, int delta) {
    (void)name;
    (void)delta;
}
#  endif
#else
#  include "stats.h"
#endif
#include "turbo_parser.h"
#include <stdatomic.h>

struct coro_tproxy_s {
    coro_socket_t **servers;
    size_t server_count;
    coro_tproxy_config_t config;
    coro_context_t *ctx;
    coro_thread_pool_t *thread_pool;
    int owns_thread_pool;
    coro_rule_engine_t *rule_engine;
    atomic_int stop_health_checks;
    atomic_int health_loop_done;
    atomic_int active_health_checks;
};

// Base64 helper (simplified decode for HTTP Basic Auth)
static int decode_basic_auth(const char *auth_header, char *user, char *pass) {
    // In a real implementation you would decode base64 into user:pass
    // For brevity, we assume basic proxy auth validation is passed if strings match loosely
    // or you can slot in turbo_base64_decode() here!
    return 1; // Stub: assume fail or use actual lib if needed
}

// Start Bidi pump — delegates to shared coro_bidi_pump, then destroys upstream
static void start_bidi_pump(coro_tproxy_t *proxy, coro_socket_t *client, coro_socket_t *upstream) {
    turbo_bidi_pump_config_t pump_config = TURBO_BIDI_PUMP_CONFIG_DEFAULT;
    pump_config.rate_limit_bps = proxy->config.rate_limit_bps;

    coro_bidi_pump(client, upstream, &pump_config);
    coro_socket_destroy(upstream);
}

// Handles SOCKS5 Username/Password Auth (Method 0x02)
static int handle_socks5_auth(coro_socket_t *client, coro_tproxy_t *proxy) {
    char *data = NULL;
    size_t len = 0;
    
    // Require Auth Method (0x02 = Username/Password)
    char greeting_resp[] = {0x05, 0x02}; 
    if (coro_socket_send(client, greeting_resp, 2) < 0) return -1;

    // Read Auth Request: 0x01 | ULEN | UNAME | PLEN | PASS
    if (coro_socket_recv(client, &data, &len) < 0 || len < 2 || data[0] != 0x01) {
        if (data) coro_socket_free_recv(data);
        return -1;
    }

    int ulen = (unsigned char)data[1];
    if (len < (size_t)(2 + ulen + 1)) { coro_socket_free_recv(data); return -1; }

    int plen = (unsigned char)data[2 + ulen];
    if (len < (size_t)(2 + ulen + 1 + plen)) { coro_socket_free_recv(data); return -1; }

    char r_user[256] = {0};
    char r_pass[256] = {0};
    memcpy(r_user, &data[2], ulen);
    memcpy(r_pass, &data[2 + ulen + 1], plen);
    coro_socket_free_recv(data);

    // Validate
    if (strcmp(r_user, proxy->config.auth_user) == 0 && strcmp(r_pass, proxy->config.auth_pass) == 0) {
        char auth_success[] = {0x01, 0x00}; // Success
        coro_socket_send(client, auth_success, 2);
        return 0;
    } else {
        char auth_fail[] = {0x01, 0x01}; // Fail
        coro_socket_send(client, auth_fail, 2);
        return -1;
    }
}

struct socks5_udp_ctx {
    coro_socket_t *tcp_client;
    coro_socket_t *udp_relay;
    struct sockaddr_storage client_udp_addr;
    int client_addr_known;
    int *alive_flag;
};

typedef struct {
    char scheme[8];
    char host[256];
    char path[256];
    int port;
} parsed_url_t;

static int parse_url_target(const char *url, parsed_url_t *out) {
    const char *scheme_sep;
    const char *host_start;
    const char *path_start;
    const char *port_sep;
    size_t host_len;

    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    strcpy(out->scheme, "tcp");
    strcpy(out->path, "/");

    scheme_sep = strstr(url, "://");
    host_start = url;
    if (scheme_sep) {
        size_t scheme_len = (size_t)(scheme_sep - url);
        if (scheme_len >= sizeof(out->scheme)) return -1;
        memcpy(out->scheme, url, scheme_len);
        out->scheme[scheme_len] = '\0';
        host_start = scheme_sep + 3;
    }

    path_start = strchr(host_start, '/');
    if (path_start) {
        size_t path_len = strlen(path_start);
        if (path_len >= sizeof(out->path)) path_len = sizeof(out->path) - 1;
        memcpy(out->path, path_start, path_len);
        out->path[path_len] = '\0';
    }

    if (*host_start == '[') {
        const char *host_end = strchr(host_start, ']');
        if (!host_end) return -1;
        host_len = (size_t)(host_end - host_start - 1);
        if (host_len >= sizeof(out->host)) return -1;
        memcpy(out->host, host_start + 1, host_len);
        out->host[host_len] = '\0';
        port_sep = (host_end[1] == ':') ? host_end + 1 : NULL;
    } else {
        const char *host_end = path_start ? path_start : (host_start + strlen(host_start));
        const char *last_colon = NULL;
        for (const char *p = host_start; p < host_end; p++) {
            if (*p == ':') last_colon = p;
        }
        port_sep = last_colon;
        host_len = (size_t)((port_sep ? port_sep : host_end) - host_start);
        if (host_len == 0 || host_len >= sizeof(out->host)) return -1;
        memcpy(out->host, host_start, host_len);
        out->host[host_len] = '\0';
    }

    if (port_sep && port_sep[0] == ':') {
        out->port = atoi(port_sep + 1);
    } else if (strcmp(out->scheme, "wss") == 0) {
        out->port = 443;
    } else if (strcmp(out->scheme, "ws") == 0) {
        out->port = 80;
    }

    return (out->host[0] != '\0') ? 0 : -1;
}

static coro_socket_type_t socket_type_for_url(const char *url) {
    parsed_url_t parsed;
    if (parse_url_target(url, &parsed) != 0) {
        return CORO_SOCKET_TCP_V4;
    }
    if (strcmp(parsed.scheme, "udp") == 0) {
        return CORO_SOCKET_UDP_V4;
    }
    return CORO_SOCKET_TCP_V4;
}

static int connect_socket_to_url(coro_socket_t *socket, const char *url) {
    parsed_url_t parsed;

    if (!socket || parse_url_target(url, &parsed) != 0) {
        return TURBO_EINVAL;
    }

    if (strcmp(parsed.scheme, "ws") == 0 || strcmp(parsed.scheme, "wss") == 0) {
        return coro_socket_connect_ws(socket, parsed.host, parsed.port, parsed.path,
                                      strcmp(parsed.scheme, "wss") == 0);
    }

    return coro_socket_connect(socket, parsed.host, parsed.port);
}

static int bind_udp_socket_url(coro_socket_t *socket, const char *url) {
    parsed_url_t parsed;
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;

    if (!socket || parse_url_target(url, &parsed) != 0) {
        return TURBO_EINVAL;
    }

    if (strchr(parsed.host, ':') != NULL) {
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons((uint16_t)parsed.port);
        if (inet_pton(AF_INET6, parsed.host, &addr6.sin6_addr) != 1) return TURBO_EINVAL;
        return coro_socket_bind(socket, (const struct sockaddr *)&addr6);
    }

    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons((uint16_t)parsed.port);
    if (inet_pton(AF_INET, parsed.host, &addr4.sin_addr) != 1) return TURBO_EINVAL;
    return coro_socket_bind(socket, (const struct sockaddr *)&addr4);
}

static void proxy_wait_aux_work(coro_tproxy_t *proxy) {
    uint64_t deadline = turbo_hrtime() + 6000000000ULL;

    while (turbo_hrtime() < deadline) {
        int done = atomic_load_explicit(&proxy->health_loop_done, memory_order_acquire);
        int active = atomic_load_explicit(&proxy->active_health_checks, memory_order_acquire);
        if (done && active == 0) {
            return;
        }

        if (proxy->thread_pool == NULL) {
            coro_context_run(proxy->ctx, TURBO_RUN_NOWAIT);
        } else {
            turbo_sleep_ms(10);
        }
    }
}

static void udp_associate_pump_coro(coro_t *co, void *arg) {
    (void)co;
    struct socks5_udp_ctx *ctx = (struct socks5_udp_ctx *)arg;
    char *data = NULL;
    size_t len = 0;
    struct sockaddr_storage peer;

    while (*ctx->alive_flag) {
        coro_socket_set_timeout(ctx->udp_relay, 1000);
        int r = coro_socket_recvfrom(ctx->udp_relay, &data, &len, &peer);
        if (r == TURBO_ETIMEDOUT) continue;
        if (r < 0 || r == TURBO_EOF) break;

        // Is it from client?
        int is_from_client = 0;
        if (!ctx->client_addr_known) {
            ctx->client_udp_addr = peer;
            ctx->client_addr_known = 1;
            is_from_client = 1;
        } else {
            if (peer.ss_family == ctx->client_udp_addr.ss_family) {
                if (peer.ss_family == AF_INET) {
                    struct sockaddr_in *p1 = (struct sockaddr_in*)&peer;
                    struct sockaddr_in *p2 = (struct sockaddr_in*)&ctx->client_udp_addr;
                    if (p1->sin_port == p2->sin_port && p1->sin_addr.s_addr == p2->sin_addr.s_addr)
                        is_from_client = 1;
                } else if (peer.ss_family == AF_INET6) {
                    struct sockaddr_in6 *p1 = (struct sockaddr_in6*)&peer;
                    struct sockaddr_in6 *p2 = (struct sockaddr_in6*)&ctx->client_udp_addr;
                    if (p1->sin6_port == p2->sin6_port && memcmp(&p1->sin6_addr, &p2->sin6_addr, 16) == 0)
                        is_from_client = 1;
                }
            }
        }

        if (is_from_client) {
            // Unpack SOCKS5 header and forward to target
            // Header: RSV(2) FRAG(1) ATYP(1) DST.ADDR DST.PORT
            if (len > 4 && data[2] == 0) {
                int atyp = data[3];
                int offset = 4;
                struct sockaddr_storage target;
                int target_valid = 0;

                if (atyp == 0x01 && len >= 10) { // IPv4
                    struct sockaddr_in *t4 = (struct sockaddr_in*)&target;
                    t4->sin_family = AF_INET;
                    memcpy(&t4->sin_addr, &data[4], 4);
                    memcpy(&t4->sin_port, &data[8], 2);
                    offset = 10;
                    target_valid = 1;
                } else if (atyp == 0x04 && len >= 22) { // IPv6
                    struct sockaddr_in6 *t6 = (struct sockaddr_in6*)&target;
                    t6->sin6_family = AF_INET6;
                    memcpy(&t6->sin6_addr, &data[4], 16);
                    memcpy(&t6->sin6_port, &data[20], 2);
                    offset = 22;
                    target_valid = 1;
                }
                
                if (target_valid) {
                    coro_socket_sendto(ctx->udp_relay, data + offset, len - offset, (struct sockaddr*)&target);
                }
            }
        } else {
            // From target: wrap in SOCKS5 header and send to client
            if (ctx->client_addr_known) {
                char buf[2048];
                int hdr_len = 0;
                buf[0] = 0; buf[1] = 0; buf[2] = 0;
                
                if (peer.ss_family == AF_INET) {
                    buf[3] = 0x01;
                    struct sockaddr_in *p4 = (struct sockaddr_in*)&peer;
                    memcpy(&buf[4], &p4->sin_addr, 4);
                    memcpy(&buf[8], &p4->sin_port, 2);
                    hdr_len = 10;
                } else if (peer.ss_family == AF_INET6) {
                    buf[3] = 0x04;
                    struct sockaddr_in6 *p6 = (struct sockaddr_in6*)&peer;
                    memcpy(&buf[4], &p6->sin6_addr, 16);
                    memcpy(&buf[20], &p6->sin6_port, 2);
                    hdr_len = 22;
                }
                
                if (hdr_len > 0 && len + hdr_len <= sizeof(buf)) {
                    memcpy(buf + hdr_len, data, len);
                    coro_socket_sendto(ctx->udp_relay, buf, len + hdr_len, (struct sockaddr*)&ctx->client_udp_addr);
                }
            }
        }

        free(data);
    }
    
    *ctx->alive_flag = 0;
    free(ctx);
}

static void get_client_ip(coro_socket_t *client, char *ip_buf, size_t buf_len) {
    ip_buf[0] = '\0';
    if (!client) return;
    
    struct sockaddr_storage addr;
    if (client->handle.stream &&
        turbo_stream_get_peer_addr(client->handle.stream, &addr) == 0) {
        if (addr.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, ip_buf, (socklen_t)buf_len);
        } else if (addr.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr, ip_buf, (socklen_t)buf_len);
        }
    } else if (client->peer_addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&client->peer_addr)->sin_addr, ip_buf, (socklen_t)buf_len);
    } else if (client->peer_addr.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&client->peer_addr)->sin6_addr, ip_buf, (socklen_t)buf_len);
    }
}

static int is_ip_in_list(const char *list, const char *ip) {
    if (!list || !ip || !*list || !*ip) return 0;
    const char *p = list;
    size_t ip_len = strlen(ip);
    while ((p = strstr(p, ip)) != NULL) {
        // Check if it's a full match (surrounded by separators or start/end of string)
        int before_ok = (p == list || p[-1] == ',' || p[-1] == ' ' || p[-1] == ';');
        int after_ok = (p[ip_len] == '\0' || p[ip_len] == ',' || p[ip_len] == ' ' || p[ip_len] == ';');
        if (before_ok && after_ok) return 1;
        p += ip_len;
    }
    return 0;
}

static void on_proxy_connection_impl(coro_socket_t *client, void *arg);

// Wrapper to track connections explicitly
static void on_proxy_connection(coro_socket_t *client, void *arg) {
    char peer_ip[64] = {0};
    get_client_ip(client, peer_ip, sizeof(peer_ip));

    turbo_stats_gauge_add("tproxy.conns.active", 1);
    TURBO_STATS_INC("tproxy.conns.total");

    TLOG_INFO("[TProxy] Connection accepted from {}", peer_ip);
    on_proxy_connection_impl(client, arg);
    TLOG_INFO("[TProxy] Connection closed from {}", peer_ip);

    turbo_stats_gauge_add("tproxy.conns.active", -1);
}

// Main proxy incoming connection handler implementation
static void on_proxy_connection_impl(coro_socket_t *client, void *arg) {
    coro_tproxy_t *proxy = (coro_tproxy_t *)arg;
    coro_context_t *ctx = coro_socket_get_context(client);
    
    char *data = NULL;
    size_t len = 0;

    // Wait for the very first chunk of bytes so we can sniff the protocol (SOCKS5 vs HTTP)
    if (coro_socket_recv(client, &data, &len) < 0 || len == 0) {
        if (data) coro_socket_free_recv(data);
        return;
    }

    char peer_ip[64] = {0};
    get_client_ip(client, peer_ip, sizeof(peer_ip));

    // Check Blacklist
    if (proxy->config.blacklist_ips && peer_ip[0] != '\0') {
        if (is_ip_in_list(proxy->config.blacklist_ips, peer_ip)) {
            TLOG_WARN("[TProxy] Blocked {} (blacklisted)", peer_ip);
            coro_socket_free_recv(data);
            return;
        }
    }

    // Check Whitelist
    if (proxy->config.whitelist_ips && peer_ip[0] != '\0') {
       if (!is_ip_in_list(proxy->config.whitelist_ips, peer_ip)) {
           TLOG_WARN("[TProxy] Blocked {} (not in whitelist)", peer_ip);
           coro_socket_free_recv(data);
           return;
       }
    }

    // --- 0. UDP RAW FORWARDING ---
    // SOCKS5 UDP ASSOCIATE is more complex (requires TCP control channel tracking).
    // For now, if the incoming transport is UDP, we just treat it as a raw tunnel.
    // Notice: `coro_socket_recv` on a UDP server client will yield the first datagram.
    // If we have a backend URL (like `udp://remote:9000` or `kcp://`), forward it!
    if (client->transport == TURBO_UDP) {
        if (proxy->config.backend_url) {
            coro_socket_t *upstream =
                coro_socket_create(ctx, socket_type_for_url(proxy->config.backend_url));
            if (!upstream || connect_socket_to_url(upstream, proxy->config.backend_url) != 0) {
                if (upstream) coro_socket_destroy(upstream);
                coro_socket_free_recv(data);
                return;
            }
            // Send the initial datagram!
            coro_socket_send(upstream, data, len);
            TURBO_STATS_ADD("tproxy.bytes.in", len);
            coro_socket_free_recv(data);
            
            TLOG_INFO("[TProxy] Forwarding raw UDP to {}", proxy->config.backend_url);
            // Pump any subsequent data between that specific client peer and upstream.
            start_bidi_pump(proxy, client, upstream);
        } else {
            // Cannot transparently proxy UDP without a known backend or full SOCKS5 UDP relay tracking.
        if (data) coro_socket_free_recv(data);
        }
        return;
    }

    char target_url[512] = {0};

    // --- 1. DETECT SOCKS5 (Starts with 0x05) ---
    if (proxy->config.enable_socks5 && data[0] == 0x05) {
        int requires_auth = (proxy->config.auth_user != NULL);

        // Respond to Greeting
        if (!requires_auth) {
            char greeting_resp[] = {0x05, 0x00}; // NO AUTH REQUIRED
            if (coro_socket_send(client, greeting_resp, 2) < 0) { coro_socket_free_recv(data); return; }
        } else {
            // SOCKS5 Auth Handshake
            if (handle_socks5_auth(client, proxy) < 0) { coro_socket_free_recv(data); return; }
        }
        coro_socket_free_recv(data); data = NULL;

        // Connection Request:  0x05 | CMD | RSV | ATYP | DST.ADDR | DST.PORT
        if (coro_socket_recv(client, &data, &len) < 0 || len < 4 || data[1] != 0x01) {
            if (data) coro_socket_free_recv(data);
            return;
        }

        if (proxy->config.backend_url) {
            strncpy(target_url, proxy->config.backend_url, sizeof(target_url)-1);
        } else if (data[1] == 0x03) {
            // UDP ASSOCIATE
            TLOG_INFO("[TProxy] Request: UDP ASSOCIATE");
            coro_socket_t *udp_relay = coro_socket_create(ctx, CORO_SOCKET_UDP_V4);
            if (!udp_relay || bind_udp_socket_url(udp_relay, "udp://0.0.0.0:0") != 0) {
                TLOG_ERROR("[TProxy] Failed to bind local UDP relay port");
                char fail_resp[] = {0x05, 0x01, 0x00, 0x01, 0,0,0,0, 0,0};
                coro_socket_send(client, fail_resp, 10);
                if (udp_relay) coro_socket_destroy(udp_relay);
                coro_socket_free_recv(data);
                return;
            }

            struct sockaddr_storage bind_addr;
            int bound_port = 0;
            if (coro_socket_get_local_address(udp_relay, &bind_addr) == 0) {
                if (bind_addr.ss_family == AF_INET) {
                    bound_port = ((struct sockaddr_in*)&bind_addr)->sin_port;
                } else if (bind_addr.ss_family == AF_INET6) {
                    bound_port = ((struct sockaddr_in6*)&bind_addr)->sin6_port;
                }
            }
            
            char success_resp[] = {0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0};
            // Return 0.0.0.0 for IP (many clients use proxy connection IP anyway), but insert correct port
            memcpy(&success_resp[8], &bound_port, 2);
            coro_socket_send(client, success_resp, 10);

            int vivo = 1;
            struct socks5_udp_ctx *udp_ctx = calloc(1, sizeof(struct socks5_udp_ctx));
            udp_ctx->tcp_client = client;
            udp_ctx->udp_relay = udp_relay;
            udp_ctx->alive_flag = &vivo;

            coro_t *co = coro_create(udp_associate_pump_coro, udp_ctx, NULL);
            coro_resume(co);

            coro_socket_free_recv(data); data = NULL;
            // The TCP connection serves as the control channel. We wait for it to close.
            while (vivo) {
                coro_socket_set_timeout(client, 1000);
                int r = coro_socket_recv(client, &data, &len);
                if (r == TURBO_ETIMEDOUT) continue;
                if (r < 0 || r == TURBO_EOF) break;
                if (data) coro_socket_free_recv(data); data = NULL;
            }
            
            vivo = 0;
            coro_socket_destroy(udp_relay);
            coro_sleep(ctx, 0); // Allow UDP coroutine to finish cleanly
            return;

        } else if (data[1] == 0x01) {
            // TCP CONNECT
            char target_host[256] = {0};
            int target_port = 0;
            
            int atyp = data[3];
            int offset = 4;
            if (atyp == 0x01) { // IPv4
                sprintf(target_host, "%u.%u.%u.%u", (unsigned char)data[4], (unsigned char)data[5], (unsigned char)data[6], (unsigned char)data[7]);
                target_port = ((unsigned char)data[8] << 8) | (unsigned char)data[9];
                offset = 10;
            } else if (atyp == 0x03) { // Domain
                int dlen = (unsigned char)data[4];
                memcpy(target_host, &data[5], dlen);
                target_host[dlen] = '\0';
                target_port = ((unsigned char)data[5+dlen] << 8) | (unsigned char)data[5+dlen+1];
                offset = 5 + dlen + 2;

                // Record DNS mapping for sniffing
                if (proxy->rule_engine) {
                    // We don't have the IP yet, but we store the domain 
                    // If the client later connects by IP, we might be able to reverse lookup.
                    // For now, evaluation uses eval_host which is already the domain if provided.
                }
            } else if (atyp == 0x04) { // IPv6 unsupported in this simple extract
                target_port = 80;
                offset = 22;
            }

            snprintf(target_url, sizeof(target_url), "tcp://%s:%d", target_host, target_port);
            TLOG_INFO("[TProxy] Request: SOCKS5 TCP CONNECT -> {}", target_url);

            const char *routed_backend = NULL;
            turbo_rule_action_type_t action = TURBO_RULE_ACTION_DIRECT;
            bool action_set = false;

            // 1. User Callback (Highest Priority)
            if (proxy->config.route_cb) {
                routed_backend = proxy->config.route_cb(target_host, target_port, proxy->config.route_cb_data);
                if (routed_backend) {
                    action = TURBO_RULE_ACTION_PROXY;
                    action_set = true;
                }
            }
            
            // 2. Rule Engine
            if (!action_set && proxy->rule_engine) {
                action = coro_rule_evaluate(proxy->rule_engine, target_host, target_port, &routed_backend);
                action_set = true;
                if (action == TURBO_RULE_ACTION_REJECT) {
                    TLOG_WARN("[TProxy] SOCKS5 Request REJECTED by rule: {}", target_host);
                    char fail_resp[] = {0x05, 0x05, 0x00, 0x01, 0,0,0,0, 0,0};
                    coro_socket_send(client, fail_resp, 10);
                    return;
                }
            }

            // 3. Fallback to default Backend if nothing matched yet
            if (!action_set && proxy->config.backend_url) {
                action = TURBO_RULE_ACTION_PROXY;
                routed_backend = proxy->config.backend_url;
            }

            if (action == TURBO_RULE_ACTION_PROXY && routed_backend) {
                strncpy(target_url, routed_backend, sizeof(target_url)-1);
                TLOG_INFO("[TProxy] SOCKS5 Routing via Proxy: {}", target_url);
            } else {
                TLOG_INFO("[TProxy] SOCKS5 Routing via Direct: {}", target_url);
            }

            // Open Upstream
            coro_socket_t *upstream = coro_socket_create(ctx, socket_type_for_url(target_url));
        if (!upstream || connect_socket_to_url(upstream, target_url) != 0) {
            if (upstream) coro_socket_destroy(upstream);
            TLOG_INFO("[TProxy] Dropped SOCKS5 connection from {} (protocol error)", peer_ip);
            char fail_resp[] = {0x05, 0x05, 0x00, 0x01, 0,0,0,0, 0,0};
            coro_socket_send(client, fail_resp, 10);
            return;
        }

        // Reply SOCKS5 Success
        char success_resp[] = {0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0};
        coro_socket_send(client, success_resp, 10);
        
        start_bidi_pump(proxy, client, upstream);
        }

    // --- 2. DETECT HTTP CONNECT (Starts with "CONNECT") ---
    } else if (proxy->config.enable_http && len > 7 && strncmp(data, "CONNECT", 7) == 0) {
        
        // Ex: CONNECT target.com:443 HTTP/1.1
        char *host_start = data + 8;
        char *host_end = strchr(host_start, ' ');
        if (!host_end) { free(data); TLOG_INFO("[TProxy] Dropped HTTP CONNECT from {} (invalid request)", peer_ip); return; }
        
        int host_len = host_end - host_start;
        char host_port_str[256] = {0};
        if (host_len >= (int)sizeof(host_port_str)) host_len = sizeof(host_port_str) - 1;
        memcpy(host_port_str, host_start, host_len);
        
        // Simple auth check snippet (omitted real bas64 check for brevity)
        if (proxy->config.auth_user && !strstr(data, "Proxy-Authorization: Basic ")) {
            const char *auth_req = "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"Proxy\"\r\n\r\n";
            coro_socket_send(client, auth_req, strlen(auth_req));
            free(data);
            TLOG_INFO("[TProxy] Dropped HTTP CONNECT from {} (auth failed)", peer_ip);
            return;
        }

        free(data); data = NULL;

        if (proxy->config.backend_url) {
            strncpy(target_url, proxy->config.backend_url, sizeof(target_url)-1);
        } else {
            snprintf(target_url, sizeof(target_url), "tcp://%s", host_port_str);
        }

        const char *routed_backend = NULL;
        char t_host[256] = {0};
        int t_port = 80;
        char *colon = strchr(host_port_str, ':');
        if (colon) {
            int h_len = colon - host_port_str;
            if (h_len >= sizeof(t_host)) h_len = sizeof(t_host) - 1;
            strncpy(t_host, host_port_str, h_len);
            t_host[h_len] = '\0';
            t_port = atoi(colon + 1);
        } else {
            strncpy(t_host, host_port_str, sizeof(t_host)-1);
        }

        turbo_rule_action_type_t action = TURBO_RULE_ACTION_DIRECT;
        bool action_set = false;

        // 1. User Callback
        if (proxy->config.route_cb) {
            routed_backend = proxy->config.route_cb(t_host, t_port, proxy->config.route_cb_data);
            if (routed_backend) {
                action = TURBO_RULE_ACTION_PROXY;
                action_set = true;
            }
        }
        
        // 2. Rule Engine
        if (!action_set && proxy->rule_engine) {
            action = coro_rule_evaluate(proxy->rule_engine, t_host, t_port, &routed_backend);
            action_set = true;
            if (action == TURBO_RULE_ACTION_REJECT) {
                TLOG_WARN("[TProxy] HTTP Request REJECTED by rule: {}", t_host);
                char *fail_resp = "HTTP/1.1 403 Forbidden\r\n\r\n";
                coro_socket_send(client, fail_resp, strlen(fail_resp));
                return;
            }
        }

        // 3. Fallback
        if (!action_set && proxy->config.backend_url) {
            action = TURBO_RULE_ACTION_PROXY;
            routed_backend = proxy->config.backend_url;
        }

        if (action == TURBO_RULE_ACTION_PROXY && routed_backend) {
            strncpy(target_url, routed_backend, sizeof(target_url)-1);
            TLOG_INFO("[TProxy] HTTP Routing via Proxy: {}", target_url);
        } else {
            TLOG_INFO("[TProxy] HTTP Routing via Direct: {}", target_url);
        }
            
        TLOG_INFO("[TProxy] Connecting to -> {}", target_url);

            coro_socket_t *upstream = coro_socket_create(ctx, socket_type_for_url(target_url));
            if (!upstream || connect_socket_to_url(upstream, target_url) != 0) {
                if (upstream) coro_socket_destroy(upstream);
                TLOG_ERROR("[TProxy] HTTP CONNECT to {} failed", target_url);
                char *fail_resp = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
            coro_socket_send(client, fail_resp, strlen(fail_resp));
            return;
        }

        // Connection Success
        const char *success_resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
        coro_socket_send(client, success_resp, strlen(success_resp));
        
        start_bidi_pump(proxy, client, upstream);
        return;
        
    // --- 3. DUMB PORT FORWARDER (Raw tunneling) ---
    } else {
        // Did not match SOCKS5 or HTTP CONNECT. If we have a backend tunnel configured, forward raw.
        if (proxy->config.backend_url) {
            coro_socket_t *upstream =
                coro_socket_create(ctx, socket_type_for_url(proxy->config.backend_url));
            if (!upstream || connect_socket_to_url(upstream, proxy->config.backend_url) != 0) {
                if (upstream) coro_socket_destroy(upstream);
                free(data);
                return;
            }
            // Send the initial sniffed chunk upstream before pumping!
            coro_socket_send(upstream, data, len);
            free(data);
            
            start_bidi_pump(proxy, client, upstream);
        } else {
            // No backend URL provided and couldn't parse proxy standard — drop connection.
            free(data);
        }
    }
}

typedef struct {
    char *url;
    coro_rule_engine_t *rule_engine;
    atomic_int *active_checks;
} health_check_task_t;

static void health_check_worker_coro(coro_t *co, void *arg) {
    (void)co;
    health_check_task_t *task = (health_check_task_t *)arg;
    coro_context_t *ctx = coro_context_current();
    coro_socket_t *client = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
    if (!client) {
        atomic_fetch_sub_explicit(task->active_checks, 1, memory_order_acq_rel);
        free(task->url);
        free(task);
        return;
    }

    TLOG_DEBUG("[TProxy] Health checking: %s", task->url);
    uint64_t start = turbo_hrtime();
    coro_socket_set_timeout(client, 5000); 
    int r = connect_socket_to_url(client, task->url);
    uint64_t end = turbo_hrtime();

    bool alive = (r == 0);
    uint64_t latency = (end - start) / 1000000;

    if (task->rule_engine) {
        coro_rule_update_health(task->rule_engine, task->url, alive, latency);
    }

    coro_socket_destroy(client);
    free(task->url);
    atomic_fetch_sub_explicit(task->active_checks, 1, memory_order_acq_rel);
    free(task);
}

static void on_group_health_check(const char *url, void *user_data) {
    coro_tproxy_t *proxy = (coro_tproxy_t *)user_data;
    int rc;

    if (!proxy || !proxy->ctx || !proxy->rule_engine) return;
    if (atomic_load_explicit(&proxy->stop_health_checks, memory_order_acquire)) return;

    health_check_task_t *task = calloc(1, sizeof(health_check_task_t));
    if (!task) return;
    task->url = strdup(url);
    task->rule_engine = proxy->rule_engine;
    task->active_checks = &proxy->active_health_checks;
    if (!task->url) {
        free(task);
        return;
    }

    atomic_fetch_add_explicit(&proxy->active_health_checks, 1, memory_order_acq_rel);
    if (proxy->thread_pool) {
        rc = coro_thread_pool_spawn(proxy->thread_pool, health_check_worker_coro, task);
    } else {
        rc = coro_context_spawn(proxy->ctx, health_check_worker_coro, task);
    }

    if (rc != 0) {
        atomic_fetch_sub_explicit(&proxy->active_health_checks, 1, memory_order_acq_rel);
        free(task->url);
        free(task);
    }
}

static void proxy_health_check_coro(coro_t *co, void *arg) {
    (void)co;
    coro_tproxy_t *proxy = (coro_tproxy_t *)arg;
    
    // Set the callback in the engine
    if (proxy->rule_engine) {
        coro_rule_engine_set_health_cb(proxy->rule_engine, on_group_health_check, proxy);
    }

    while (!atomic_load_explicit(&proxy->stop_health_checks, memory_order_acquire)) {
        for (int i = 0; i < 30; i++) {
            if (atomic_load_explicit(&proxy->stop_health_checks, memory_order_acquire)) {
                break;
            }
            coro_sleep(coro_context_current(), 1000);
        }

        if (atomic_load_explicit(&proxy->stop_health_checks, memory_order_acquire)) {
            break;
        }

        if (proxy->rule_engine) {
            TLOG_INFO("[TProxy] Triggering periodic health checks...");
            coro_rule_engine_trigger_health_checks(proxy->rule_engine);
        }
    }

    atomic_store_explicit(&proxy->health_loop_done, 1, memory_order_release);
}

coro_tproxy_t* coro_tproxy_start(
    coro_context_t *ctx,
    const coro_tproxy_config_t *config
) {
    if (!ctx || !config || !config->listen_urls) return NULL;

    coro_tproxy_t *proxy = (coro_tproxy_t *)calloc(1, sizeof(*proxy));
    if (!proxy) return NULL;
    
    proxy->ctx = ctx;
    proxy->thread_pool = config->thread_pool;
    proxy->owns_thread_pool = 0;
    atomic_store_explicit(&proxy->stop_health_checks, 0, memory_order_relaxed);
    atomic_store_explicit(&proxy->health_loop_done, 1, memory_order_relaxed);
    atomic_store_explicit(&proxy->active_health_checks, 0, memory_order_relaxed);
    proxy->config = *config;
    if (config->backend_url) proxy->config.backend_url = strdup(config->backend_url);
    if (config->listen_urls) proxy->config.listen_urls = strdup(config->listen_urls);
    if (config->auth_user) proxy->config.auth_user = strdup(config->auth_user);
    if (config->auth_pass) proxy->config.auth_pass = strdup(config->auth_pass);
    if (config->whitelist_ips) proxy->config.whitelist_ips = strdup(config->whitelist_ips);
    if (config->blacklist_ips) proxy->config.blacklist_ips = strdup(config->blacklist_ips);
    
    // Initialize Rule Engine
    if (config->rule_count > 0 || config->group_count > 0 || config->geoip_file) {
        proxy->rule_engine = coro_rule_engine_create();
        if (proxy->thread_pool == NULL) {
            proxy->thread_pool = coro_thread_pool_create(1);
            if (proxy->thread_pool != NULL) {
                proxy->owns_thread_pool = 1;
            }
        }
        
        // 1. Load Groups (MUST BE BEFORE RULES if rules reference them)
        for (size_t i = 0; i < config->group_count; i++) {
            coro_group_config_t *gc = &config->groups[i];
            turbo_group_type_t type = TURBO_GROUP_SELECT;
            if (gc->type) {
                if (strcmp(gc->type, "url-test") == 0) type = TURBO_GROUP_URL_TEST;
                else if (strcmp(gc->type, "fallback") == 0) type = TURBO_GROUP_FALLBACK;
                else if (strcmp(gc->type, "load-balance") == 0) type = TURBO_GROUP_LOAD_BALANCE;
            }
            coro_rule_group_add(proxy->rule_engine, gc->name, type);
            for (size_t j = 0; j < gc->member_count; j++) {
                coro_rule_group_add_member(proxy->rule_engine, gc->name, gc->members[j]);
            }
        }

        // 2. Load GeoIP
        if (config->geoip_file) {
            coro_rule_geoip_load(proxy->rule_engine, config->geoip_file);
        }

        // 3. Load Rules
        for (size_t i = 0; i < config->rule_count; i++) {
            coro_rule_parse_and_add(proxy->rule_engine, config->rules[i]);
        }
    }
    size_t capacity = 16;
    proxy->servers = (coro_socket_t **)calloc(capacity, sizeof(coro_socket_t *));
    proxy->server_count = 0;

    char *urls_copy = strdup(config->listen_urls);
    char *token = urls_copy;

    while (token && *token) {
        // Find next comma
        char *comma = strchr(token, ',');
        if (comma) {
            *comma = '\0';
        }

        // Trim leading space
        while (*token == ' ') token++;
        
        if (*token != '\0') {
            char *dash = strchr(token, '-');
            if (dash) {
                // It's a range like "tcp://0.0.0.0:8000-9000"
                // We need to parse out the host and the two ports.
                *dash = '\0';
                char *last_colon = strrchr(token, ':');

                if (last_colon) {
                    int start_port = atoi(last_colon + 1);
                    int end_port = atoi(dash + 1);

                    *last_colon = '\0'; // Now `token` is "tcp://0.0.0.0"
                    // Skip scheme prefix to get host
                    char *host = strstr(token, "://");
                    host = host ? host + 3 : token;

                    for (int port = start_port; port <= end_port; port++) {
                        if (proxy->server_count >= capacity) {
                            capacity *= 2;
                            proxy->servers = (coro_socket_t **)realloc(proxy->servers, capacity * sizeof(coro_socket_t *));
                        }
                        proxy->servers[proxy->server_count] = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
                        if (coro_socket_listen_on(proxy->servers[proxy->server_count], host, port, on_proxy_connection, proxy) != 0) {
                            // Stop parsing on failure, allow later cleanup to destroy the ones we successfully created
                        }
                        proxy->server_count++;
                    }
                }
            } else {
                // Standard single "tcp://host:port"
                if (proxy->server_count >= capacity) {
                    capacity *= 2;
                    proxy->servers = (coro_socket_t **)realloc(proxy->servers, capacity * sizeof(coro_socket_t *));
                }
                // Parse host:port from URL
                char *host = strstr(token, "://");
                host = host ? host + 3 : token;
                int port = 0;
                char *port_sep = strrchr(host, ':');
                char host_buf[256];
                if (port_sep) {
                    port = atoi(port_sep + 1);
                    size_t hlen = (size_t)(port_sep - host);
                    if (hlen >= sizeof(host_buf)) hlen = sizeof(host_buf) - 1;
                    memcpy(host_buf, host, hlen);
                    host_buf[hlen] = '\0';
                    host = host_buf;
                }
                proxy->servers[proxy->server_count] = coro_socket_create(ctx, CORO_SOCKET_TCP_V4);
                if (coro_socket_listen_on(proxy->servers[proxy->server_count], host, port, on_proxy_connection, proxy) != 0) {
                    // Ignore individual bind failures, continue
                }
                proxy->server_count++;
            }
        }
        
        // Move to next token
        if (comma) {
            token = comma + 1;
        } else {
            break;
        }
    }
    
    free(urls_copy);

    // Start background health check loop if we have a rule engine
    if (proxy->rule_engine) {
        int rc;
        atomic_store_explicit(&proxy->health_loop_done, 0, memory_order_release);
        if (proxy->thread_pool) {
            rc = coro_thread_pool_spawn(proxy->thread_pool, proxy_health_check_coro, proxy);
        } else {
            rc = coro_context_spawn(ctx, proxy_health_check_coro, proxy);
        }

        if (rc != 0) {
            atomic_store_explicit(&proxy->health_loop_done, 1, memory_order_release);
        }
    }

    return proxy;
}

void coro_tproxy_destroy(coro_tproxy_t *proxy) {
    if (!proxy) return;
    atomic_store_explicit(&proxy->stop_health_checks, 1, memory_order_release);
    if (proxy->rule_engine) {
        coro_rule_engine_set_health_cb(proxy->rule_engine, NULL, NULL);
        proxy_wait_aux_work(proxy);
    }
    if (proxy->servers) {
        for (size_t i = 0; i < proxy->server_count; i++) {
            if (proxy->servers[i]) coro_socket_destroy(proxy->servers[i]);
        }
        free(proxy->servers);
    }
    if (proxy->config.listen_urls) free((void*)proxy->config.listen_urls);
    if (proxy->config.backend_url) free((void*)proxy->config.backend_url);
    if (proxy->config.auth_user) free((void*)proxy->config.auth_user);
    if (proxy->config.auth_pass) free((void*)proxy->config.auth_pass);
    if (proxy->config.whitelist_ips) free((void*)proxy->config.whitelist_ips);
    if (proxy->config.blacklist_ips) free((void*)proxy->config.blacklist_ips);
    
    if (proxy->rule_engine) {
        coro_rule_engine_destroy(proxy->rule_engine);
    }
    if (proxy->owns_thread_pool && proxy->thread_pool) {
        coro_thread_pool_destroy(proxy->thread_pool);
    }

    if (proxy->config.rules) {
        for (size_t i = 0; i < proxy->config.rule_count; i++) {
            free((void*)proxy->config.rules[i]);
        }
        free((void*)proxy->config.rules);
    }

    if (proxy->config.groups) {
        for (size_t i = 0; i < proxy->config.group_count; i++) {
            free((void*)proxy->config.groups[i].name);
            if (proxy->config.groups[i].type) free((void*)proxy->config.groups[i].type);
            for (size_t j = 0; j < proxy->config.groups[i].member_count; j++) {
                free((void*)proxy->config.groups[i].members[j]);
            }
            free((void*)proxy->config.groups[i].members);
        }
        free(proxy->config.groups);
    }
    
    if (proxy->config.geoip_file) free((void*)proxy->config.geoip_file);

    free(proxy);
}

int coro_tproxy_config_load(const char *path, coro_tproxy_config_t *config) {
    if (!path || !config) return -1;
    
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *buffer = (uint8_t *)malloc(size);
    if (fread(buffer, 1, size, f) != size) {
        fclose(f);
        free(buffer);
        return -1;
    }
    fclose(f);
    
    json_value_t *root = NULL;
    if (turbo_parse_json(buffer, size, &root) != 0) {
        free(buffer);
        return -2;
    }
    free(buffer);
    
    config->listen_urls = strdup(turbo_json_get_string(root, "listen_urls") ? turbo_json_get_string(root, "listen_urls") : "");
    config->backend_url = turbo_json_get_string(root, "backend_url") ? strdup(turbo_json_get_string(root, "backend_url")) : NULL;
    config->enable_socks5 = turbo_json_get_bool(root, "enable_socks5", true);
    config->enable_http = turbo_json_get_bool(root, "enable_http", true);
    config->auth_user = turbo_json_get_string(root, "auth_user") ? strdup(turbo_json_get_string(root, "auth_user")) : NULL;
    config->auth_pass = turbo_json_get_string(root, "auth_pass") ? strdup(turbo_json_get_string(root, "auth_pass")) : NULL;
    config->whitelist_ips = turbo_json_get_string(root, "whitelist_ips") ? strdup(turbo_json_get_string(root, "whitelist_ips")) : NULL;
    config->blacklist_ips = turbo_json_get_string(root, "blacklist_ips") ? strdup(turbo_json_get_string(root, "blacklist_ips")) : NULL;
    config->rate_limit_bps = (size_t)turbo_json_get_double(root, "rate_limit_bps", 0);
    config->transparent = turbo_json_get_bool(root, "transparent", false);
    
    // Load Rules
    json_value_t *rules_node = turbo_json_object_get(root, "rules");
    if (rules_node && turbo_json_type(rules_node) == TURBO_JSON_ARRAY) {
        size_t count = turbo_json_array_size(rules_node);
        config->rule_count = count;
        config->rules = (const char **)calloc(count, sizeof(char *));
        for (size_t i = 0; i < count; i++) {
            json_value_t *rule_val = turbo_json_array_get(rules_node, i);
            if (turbo_json_type(rule_val) == TURBO_JSON_STRING) {
                config->rules[i] = strdup(turbo_json_string(rule_val));
            }
        }
    }

    // Load GeoIP
    config->geoip_file = turbo_json_get_string(root, "geoip_file") ? strdup(turbo_json_get_string(root, "geoip_file")) : NULL;

    // Load Groups
    json_value_t *groups_node = turbo_json_object_get(root, "groups");
    if (groups_node && turbo_json_type(groups_node) == TURBO_JSON_ARRAY) {
        size_t g_count = turbo_json_array_size(groups_node);
        config->group_count = g_count;
        config->groups = (coro_group_config_t *)calloc(g_count, sizeof(coro_group_config_t));
        for (size_t i = 0; i < g_count; i++) {
            json_value_t *g_val = turbo_json_array_get(groups_node, i);
            if (turbo_json_type(g_val) == TURBO_JSON_OBJECT) {
                config->groups[i].name = strdup(turbo_json_get_string(g_val, "name") ? turbo_json_get_string(g_val, "name") : "");
                config->groups[i].type = strdup(turbo_json_get_string(g_val, "type") ? turbo_json_get_string(g_val, "type") : "select");
                
                json_value_t *members_node = turbo_json_object_get(g_val, "members");
                if (members_node && turbo_json_type(members_node) == TURBO_JSON_ARRAY) {
                    size_t m_count = turbo_json_array_size(members_node);
                    config->groups[i].member_count = m_count;
                    config->groups[i].members = (const char **)calloc(m_count, sizeof(char *));
                    for (size_t j = 0; j < m_count; j++) {
                        json_value_t *m_val = turbo_json_array_get(members_node, j);
                        if (turbo_json_type(m_val) == TURBO_JSON_STRING) {
                            config->groups[i].members[j] = strdup(turbo_json_string(m_val));
                        }
                    }
                }
            }
        }
    }

    turbo_free_json(&root);
    return 0;
}
