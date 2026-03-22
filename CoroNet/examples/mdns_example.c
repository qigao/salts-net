/**
 * mdns_example.c - mDNS service discovery and publishing
 * 
 * Demonstrates Bonjour/Avahi-style service discovery using multicast DNS.
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
#include <uv.h>
#include "turbo_mdns.h"

static mdns_ctx_t* g_mdns = NULL;
static volatile sig_atomic_t g_running = 1;

void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    g_running = 0;
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
    
    /* Stop discovery after first service found */
    g_running = 0;
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
    
    while (g_running) {
        int rc = uv_run(uv_default_loop(), UV_RUN_ONCE);
        if (rc == 0) {
            break; /* No more events */
        }
    }
    
    printf("Unpublishing service...\n");
    mdns_unpublish(g_mdns, service.instance, service.service_type);
}

void discover_mode(void) {
    printf("Discovering _test._tcp services for 10 seconds...\n");
    
    if (mdns_discover(g_mdns, "_test._tcp", on_service_discovered, NULL, 10000) != 0) {
        printf("Failed to start discovery\n");
        return;
    }
    
    /* Discovery will timeout automatically via the mdns_discover timeout parameter */
    
    while (g_running) {
        int rc = uv_run(uv_default_loop(), UV_RUN_ONCE);
        if (rc == 0) {
            break; /* No more events */
        }
    }
    printf("Discovery finished.\n");
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    
    g_mdns = mdns_create(uv_default_loop());
    if (!g_mdns) {
        printf("Failed to create mDNS context\n");
        return 1;
    }
    
    if (argc > 1 && strcmp(argv[1], "discover") == 0) {
        discover_mode();
    } else {
        publish_mode();
    }
    
    mdns_destroy(g_mdns);
    
    /* Drain any remaining events */
    uv_run(uv_default_loop(), UV_RUN_NOWAIT);
    
    int rc = uv_loop_close(uv_default_loop());
    if (rc != 0) {
        printf("Warning: uv_loop_close failed: %s\n", uv_strerror(rc));
    }
    
    return 0;
}