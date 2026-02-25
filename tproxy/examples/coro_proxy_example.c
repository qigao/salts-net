#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "turbo_coro_tproxy.h"

static turbo_coro_context_t *ctx = NULL;
static turbo_coro_tproxy_t *proxy = NULL;

void handle_sigint(int sig) {
    (void)sig;
    printf("\n[server] Interrupted. Stopping proxy...\n");
    if (ctx) {
        turbo_coro_context_stop(ctx);
    }
}

static const char* my_router(const char *target_host, int target_port, void *user_data) {
    (void)target_port;
    (void)user_data;
    if (strstr(target_host, "google") || strstr(target_host, "youtube")) return NULL;
    return NULL;
}

int main(int argc, char **argv) {
    turbo_coro_tproxy_config_t config = {0};
    config.listen_urls = "tcp://0.0.0.0:1080";
    config.enable_socks5 = 1;
    config.enable_http = 1;
    config.route_cb = my_router;

    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) config_path = argv[++i];
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) config.listen_urls = argv[++i];
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) config.backend_url = argv[++i];
    }

    if (config_path) {
        printf("[server] Loading config from %s...\n", config_path);
        if (turbo_coro_tproxy_config_load(config_path, &config) != 0) {
            fprintf(stderr, "[server] Failed to load config file\n");
            return 1;
        }
    }

    signal(SIGINT, handle_sigint);
    ctx = turbo_coro_context_create(NULL);
    if (!ctx) return 1;

    printf("==========================================\n");
    printf("   TurboNet Coroutine Proxy Server        \n");
    printf("==========================================\n");
    printf("[config] Listen URLs:  %s\n", config.listen_urls);
    if (config.backend_url)  printf("[config] Default Upstream: %s\n", config.backend_url);
    if (config.whitelist_ips) printf("[config] Whitelist:    %s\n", config.whitelist_ips);
    if (config.rate_limit_bps) printf("[config] Rate Limit:   %zu bytes/s\n", config.rate_limit_bps);
    printf("------------------------------------------\n");

    proxy = turbo_coro_tproxy_start(ctx, &config);
    if (!proxy) {
        fprintf(stderr, "[server] Failed to start proxy\n");
        return 1;
    }

    turbo_coro_context_run(ctx, TURBO_RUN_DEFAULT);
    turbo_coro_tproxy_destroy(proxy);
    turbo_coro_context_destroy(ctx);
    return 0;
}
