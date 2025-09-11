/**
 * Simple DNS Resolution for TurboNet
 * "Make the common case fast and the special case disappear" - Linus
 * 
 * No vtable bullshit, no bind/connect abstraction
 * Just: domain name -> IP address, period.
 */
#include "turbonet.h"
#include "turbonet_internal.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <uv.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

// Simple DNS request - no bullshit wrapper
typedef struct {
    uv_getaddrinfo_t req;
    turbo_resolve_cb callback;
    void* user_data;
    char domain[];  // Flexible array member - clean
} dns_request_t;

// Global DNS servers (if we ever want custom DNS later)
static char g_dns_servers[8][64];
static int g_dns_count = 0;
static bool g_dns_configured = false;

// =============================================================================
// Core DNS Resolution - the only thing that matters
// =============================================================================

static void dns_resolved_cb(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
    dns_request_t* dns_req = (dns_request_t*)req;
    
    if (status != 0) {
        log_error("DNS failed for %s: %s", dns_req->domain, uv_strerror(status));
        dns_req->callback(dns_req->domain, NULL, status, dns_req->user_data);
        goto cleanup;
    }
    
    if (!res) {
        log_error("No address for %s", dns_req->domain);
        dns_req->callback(dns_req->domain, NULL, UV_EAI_NONAME, dns_req->user_data);
        goto cleanup;
    }
    
    // Extract the first IP address - simple
    char ip[INET6_ADDRSTRLEN] = {0};
    if (res->ai_family == AF_INET) {
        struct sockaddr_in* addr4 = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &addr4->sin_addr, ip, sizeof(ip));
        log_debug("DNS: %s -> %s (IPv4)", dns_req->domain, ip);
    } else if (res->ai_family == AF_INET6) {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)res->ai_addr;
        inet_ntop(AF_INET6, &addr6->sin6_addr, ip, sizeof(ip));
        log_debug("DNS: %s -> %s (IPv6)", dns_req->domain, ip);
    } else {
        log_error("Unknown address family %d for %s", res->ai_family, dns_req->domain);
        dns_req->callback(dns_req->domain, NULL, UV_EAFNOSUPPORT, dns_req->user_data);
        goto cleanup;
    }
    
    // Success callback
    dns_req->callback(dns_req->domain, ip, 0, dns_req->user_data);
    
cleanup:
    if (res) uv_freeaddrinfo(res);
    free(dns_req);
}

// =============================================================================
// Public API - dead simple
// =============================================================================

 int turbo_resolve_hostname(uv_loop_t* loop, const char* hostname, 
                                       turbo_resolve_cb callback, void* user_data) {
    if (!loop || !hostname || !callback) {
        return UV_EINVAL;
    }
    
    // Fast path: already an IP address?
    struct sockaddr_storage addr;
    if (turbo_parse_address(hostname, 80, &addr) == 0) {
        // It's already an IP - call callback immediately
        log_debug("Already IP address: %s", hostname);
        callback(hostname, hostname, 0, user_data);
        return 0;
    }
    
    // Custom DNS servers configured? Log this info
    if (g_dns_configured) {
        log_info("Using custom DNS servers for %s (count: %d)", hostname, g_dns_count);
        for (int i = 0; i < g_dns_count; i++) {
            log_debug("  DNS[%d]: %s", i, g_dns_servers[i]);
        }
    }
    
    // Slow path: DNS lookup needed
    size_t domain_len = strlen(hostname);
    dns_request_t* dns_req = malloc(sizeof(dns_request_t) + domain_len + 1);
    if (!dns_req) {
        return UV_ENOMEM;
    }
    
    dns_req->callback = callback;
    dns_req->user_data = user_data;
    strcpy(dns_req->domain, hostname);
    
    // Setup hints for getaddrinfo
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;    // Both IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM;
    
    int err = uv_getaddrinfo(loop, &dns_req->req, dns_resolved_cb, hostname, NULL, &hints);
    if (err != 0) {
        log_error("Failed to start DNS lookup: %s", uv_strerror(err));
        free(dns_req);
        return err;
    }
    
    log_debug("Starting DNS lookup for %s", hostname);
    return 0;
}

// =============================================================================
// Address parsing - moved from complex shit to simple function
// =============================================================================

 int turbo_parse_address(const char* address, int port, struct sockaddr_storage* addr) {
    if (!address || !addr) {
        return UV_EINVAL;
    }
    
    // Try IPv4
    struct sockaddr_in* addr4 = (struct sockaddr_in*)addr;
    if (uv_ip4_addr(address, port, addr4) == 0) {
        return 0;
    }
    
    // Try IPv6  
    struct sockaddr_in6* addr6 = (struct sockaddr_in6*)addr;
    if (uv_ip6_addr(address, port, addr6) == 0) {
        return 0;
    }
    
    return UV_EAI_NONAME;  // Not a valid IP
}

// =============================================================================
// DNS server management - simple global state
// =============================================================================

 int turbo_set_dns_servers(const char* servers[], int count) {
    if (!servers || count < 0 || count > 8) {
        return UV_EINVAL;
    }
    
    g_dns_count = 0;
    g_dns_configured = false;
    for (int i = 0; i < count && i < 8; i++) {
        if (!servers[i]) continue;
        
        size_t len = strlen(servers[i]);
        if (len >= sizeof(g_dns_servers[i])) continue;
        
        strcpy(g_dns_servers[g_dns_count], servers[i]);
        g_dns_count++;
        log_info("DNS server: %s", servers[i]);
    }
    
    if (g_dns_count > 0) {
        g_dns_configured = true;
    }
    
    log_info("Configured %d DNS servers", g_dns_count);
    return 0;
}

 int turbo_get_dns_servers(char servers[][64], int max_servers, int* count) {
    if (!servers || !count) {
        return UV_EINVAL;
    }
    
    int copy_count = (g_dns_count > max_servers) ? max_servers : g_dns_count;
    
    for (int i = 0; i < copy_count; i++) {
        strcpy(servers[i], g_dns_servers[i]);
    }
    
    *count = copy_count;
    return 0;
}