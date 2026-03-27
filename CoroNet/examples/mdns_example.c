/**
 * mdns_example.c - mDNS service discovery and publishing
 * 
 * Demonstrates Bonjour/Avahi-style service discovery using multicast DNS
 * via the CoroNet native backend (no libuv).
 * 
 * Usage:
 *   ./mdns_example          - Publish a test service
 *   ./mdns_example discover - Discover test services on the network
 * 
 * Press Ctrl+C to stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "CoroNet.h"
#include "turbo_mdns.h"
#include "tlog.h"

static mdns_ctx_t* g_mdns = NULL;
static coro_context_t* g_ctx = NULL;
static coro_socket_t* g_tcp_server = NULL;
static coro_socket_t* g_udp_server = NULL;

void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    if (g_ctx) coro_context_stop(g_ctx);
}

static void tcp_message_handler(coro_socket_t* client, void* userdata) {
    UNUSED(userdata);

    for (;;) {
        char* data = NULL;
        size_t len = 0;
        int r = coro_socket_recv(client, &data, &len);
        if (r != 0 || !data || len == 0) {
            if (data) {
                coro_socket_free_recv(data);
            }
            break;
        }

        printf("[TCP] Received %zu bytes: %.*s\n", len, (int)len, data);
        coro_socket_free_recv(data);
    }
}

static void udp_message_handler(coro_socket_t* client, void* userdata) {
    UNUSED(userdata);

    char* data = NULL;
    size_t len = 0;
    int r = coro_socket_recv(client, &data, &len);
    if (r != 0 || !data) {
        if (data) {
            coro_socket_free_recv(data);
        }
        return;
    }

    printf("[UDP] Received %zu bytes: %.*s\n", len, (int)len, data);
    coro_socket_free_recv(data);
}

static void stop_demo_servers(void) {
    if (g_udp_server) {
        coro_socket_destroy(g_udp_server);
        g_udp_server = NULL;
    }
    if (g_tcp_server) {
        coro_socket_destroy(g_tcp_server);
        g_tcp_server = NULL;
    }
}

static int start_demo_servers(int port) {
    int r;

    g_tcp_server = coro_socket_create_tcpv4(g_ctx);
    if (!g_tcp_server) {
        printf("Failed to create TCP server\n");
        return -1;
    }

    r = coro_socket_listen_on(g_tcp_server, "0.0.0.0", port, tcp_message_handler, NULL);
    if (r != 0) {
        printf("Failed to listen on TCP %d: %d\n", port, r);
        stop_demo_servers();
        return r;
    }

    g_udp_server = coro_socket_create_udpv4(g_ctx);
    if (!g_udp_server) {
        printf("Failed to create UDP server\n");
        stop_demo_servers();
        return -1;
    }

    r = coro_socket_listen_on(g_udp_server, "0.0.0.0", port, udp_message_handler, NULL);
    if (r != 0) {
        printf("Failed to listen on UDP %d: %d\n", port, r);
        stop_demo_servers();
        return r;
    }

    return 0;
}

void on_service_discovered(const mdns_service_t* service, void* userdata) {
    (void)userdata;
    printf("=== Service Found ===\n");
    printf("Instance: %s\n", service->instance);
    printf("Service: %s\n", service->service_type);
    printf("Host: %s\n", service->hostname);
    printf("IP: %s\n", service->ip);
    printf("Port: %u\n", service->port);
    printf("TTL: %u\n", service->ttl);
    printf("====================\n");
}

void publish_mode(void) {
    mdns_service_t services[2] = {{0}};
    strncpy(services[0].instance, "My Test Service", sizeof(services[0].instance) - 1);
    strncpy(services[0].service_type, "_test._tcp", sizeof(services[0].service_type) - 1);
    services[0].port = 8080;
    services[0].ttl = 120;

    strncpy(services[1].instance, "My Test Service", sizeof(services[1].instance) - 1);
    strncpy(services[1].service_type, "_test._udp", sizeof(services[1].service_type) - 1);
    services[1].port = 8080;
    services[1].ttl = 120;
    
    printf("Publishing services: %s._test._tcp and %s._test._udp on port %d\n",
           services[0].instance, services[1].instance, services[0].port);
    printf("Local hostname: %s\n", mdns_get_local_hostname());
    printf("Local IP: %s\n", mdns_get_local_ip());

    if (start_demo_servers(services[0].port) != 0) {
        return;
    }
    
    if (mdns_publish_many(g_mdns, services, 2) != 0) {
        printf("Failed to publish services\n");
        stop_demo_servers();
        return;
    }
    
    printf("Service published.\n");
    printf("Listening for TCP and UDP messages on port %d.\n", services[0].port);
    printf("Press Ctrl+C to stop.\n");
    coro_context_run(g_ctx, TURBO_RUN_DEFAULT);
    
    printf("Unpublishing service...\n");
    mdns_unpublish_all(g_mdns);
    stop_demo_servers();
}

void discover_mode(void) {
    const char* service_types[] = {"_test._tcp", "_test._udp"};

    printf("Discovering _test._tcp and _test._udp services. Press Ctrl+C to stop.\n");
    
    if (mdns_discover_many(g_mdns, service_types, 2, on_service_discovered, NULL, 0) != 0) {
        printf("Failed to start discovery\n");
        return;
    }
    
    coro_context_run(g_ctx, TURBO_RUN_DEFAULT);
    printf("Discovery finished.\n");
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    
    tlog_config_t log_cfg = { .min_level = TURBO_LOG_LEVEL_DEBUG, .buffer_size = 65536, .pool_size = 32768 };
    tlog_t *logger = tlog_create(&log_cfg);
    turbo_console_sink_opts_t console_opts = { .output = stdout, .use_colors = 1, .pattern = TURBO_LOG_DEFAULT_PATTERN };
    tlog_add_sink(logger, turbo_sink_console_create(&console_opts));
    tlog_set_default(logger);

    g_ctx = coro_context_create(NULL);
    if (!g_ctx) {
        printf("Failed to create coroutine context\n");
        return 1;
    }

    g_mdns = mdns_create(g_ctx);
    if (!g_mdns) {
        printf("Failed to create mDNS context\n");
        coro_context_destroy(g_ctx);
        return 1;
    }
    
    if (argc > 1 && strcmp(argv[1], "discover") == 0) {
        discover_mode();
    } else {
        publish_mode();
    }
    
    mdns_destroy(g_mdns);
    coro_context_destroy(g_ctx);
    
    return 0;
}
