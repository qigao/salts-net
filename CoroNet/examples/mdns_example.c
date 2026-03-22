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

void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    if (g_ctx) coro_context_stop(g_ctx);
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
    
    /* Stop after first discovery in this example */
    if (g_ctx) coro_context_stop(g_ctx);
}

void publish_mode(void) {
    mdns_service_t service = {0};
    strncpy(service.instance, "My Test Service", sizeof(service.instance) - 1);
    strncpy(service.service_type, "_test._tcp", sizeof(service.service_type) - 1);
    service.port = 8080;
    service.ttl = 120;
    
    printf("Publishing service: %s._test._tcp on port %d\n", 
           service.instance, service.port);
    printf("Local hostname: %s\n", mdns_get_local_hostname());
    printf("Local IP: %s\n", mdns_get_local_ip());
    
    if (mdns_publish(g_mdns, &service) != 0) {
        printf("Failed to publish service\n");
        return;
    }
    
    printf("Service published. Press Ctrl+C to stop.\n");
    coro_context_run(g_ctx, TURBO_RUN_DEFAULT);
    
    printf("Unpublishing service...\n");
    mdns_unpublish(g_mdns, service.instance, service.service_type);
}

void discover_mode(void) {
    printf("Discovering _test._tcp services for 10 seconds...\n");
    
    if (mdns_discover(g_mdns, "_test._tcp", on_service_discovered, NULL, 10000) != 0) {
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