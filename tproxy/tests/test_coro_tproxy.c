#include "turbo_coro_tproxy.h"
#include <CoroNet.h>
#include "tinytest.h"

spec("coro_tproxy_advanced") {
  describe("proxy lifecycle with features") {
    it("starts proxy with HTTP and Auth") {
      coro_context_t *ctx = coro_context_create(NULL);
      coro_thread_pool_t *pool = coro_thread_pool_create(1);
      coro_tproxy_config_t config = {0};
      config.listen_urls = "tcp://127.0.0.1:1080";
      config.backend_url = NULL;
      config.enable_socks5 = 1;
      config.enable_http = 1;
      config.auth_user = "admin";
      config.auth_pass = "secret123";
      config.thread_pool = pool;
      
      coro_tproxy_t *proxy = coro_tproxy_start(ctx, &config);
      check(proxy != NULL);

      coro_tproxy_destroy(proxy);

      // Pump the loop to let handler coroutines finish cleanup
      for (int i = 0; i < 50; i++) {
          coro_context_run(ctx, TURBO_RUN_NOWAIT);
      }

      coro_context_destroy(ctx);
      coro_thread_pool_destroy(pool);
    }

    it("loads config from JSON") {
      const char *json = "{\"listen_urls\":\"tcp://127.0.0.1:1100\",\"rate_limit_bps\":5000}";
      FILE *f = fopen("test_config.json", "wb");
      fwrite(json, 1, strlen(json), f);
      fclose(f);

      coro_tproxy_config_t config = {0};
      int r = coro_tproxy_config_load("test_config.json", &config);
      check(r == 0);
      check(strcmp(config.listen_urls, "tcp://127.0.0.1:1100") == 0);
      check(config.rate_limit_bps == 5000);

      if (config.listen_urls) free((void*)config.listen_urls);
      remove("test_config.json");
    }

    it("loads rules from JSON config") {
      const char *json = "{\"listen_urls\":\"tcp://127.0.0.1:1200\", \"rules\":[\"DOMAIN,google.com,Proxy,ws://p1\", \"MATCH,Direct\"]}";
      FILE *f = fopen("test_rules.json", "wb");
      fwrite(json, 1, strlen(json), f);
      fclose(f);

      coro_tproxy_config_t config = {0};
      int r = coro_tproxy_config_load("test_rules.json", &config);
      check(r == 0);
      check(config.rule_count == 2);
      check(strcmp(config.rules[0], "DOMAIN,google.com,Proxy,ws://p1") == 0);
      check(strcmp(config.rules[1], "MATCH,Direct") == 0);

      coro_context_t *ctx = coro_context_create(NULL);
      coro_tproxy_t *proxy = coro_tproxy_start(ctx, &config);
      check(proxy != NULL);

      coro_tproxy_destroy(proxy);
      coro_context_destroy(ctx);
      remove("test_rules.json");
    }
    it("loads proxy groups from JSON") {
      const char *json_g = 
        "{"
        "  \"listen_urls\": \"tcp://127.0.0.1:1300\","
        "  \"groups\": ["
        "    {"
        "      \"name\": \"G1\","
        "      \"type\": \"url-test\","
        "      \"members\": [\"p1\", \"p2\"]"
        "    }"
        "  ],"
        "  \"rules\": [\"MATCH,Direct\"]"
        "}";
      FILE *f_g = fopen("test_groups.json", "wb");
      fwrite(json_g, 1, strlen(json_g), f_g);
      fclose(f_g);

      coro_tproxy_config_t config_g = {0};
      int r_g = coro_tproxy_config_load("test_groups.json", &config_g);
      check(r_g == 0);
      check(config_g.group_count == 1);
      check(strcmp(config_g.groups[0].name, "G1") == 0);
      check(strcmp(config_g.groups[0].type, "url-test") == 0);
      check(config_g.groups[0].member_count == 2);
      check(strcmp(config_g.groups[0].members[0], "p1") == 0);

      coro_context_t *ctx_g = coro_context_create(NULL);
      coro_tproxy_t *proxy_g = coro_tproxy_start(ctx_g, &config_g);
      check(proxy_g != NULL);

      coro_tproxy_destroy(proxy_g);
      coro_context_destroy(ctx_g);
      remove("test_groups.json");
    }
  }
}
