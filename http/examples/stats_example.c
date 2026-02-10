#include "tinytest.h"
#include "http_client.h"

spec("Client Statistics Test") {
  static http_client_t *client = NULL;

  before() {
    client = http_client_create();
    check(client != NULL);
  }

  after() {
    if (client) {
      http_client_destroy(client);
    }
  }

  it("should successfully track client statistics") {
    http_client_reset_stats(client);
    
    http_response_t *response = http_get(client, "https://httpbin.org/get");
    check(response != NULL);
    http_response_free(response);
    
    http_client_stats_t stats;
    http_client_get_stats(client, &stats);
    
    check(stats.total_requests == 1);
    check(stats.successful_requests == 1);
    check(stats.bytes_received > 0);
  }

  it("should successfully track redirects") {
    http_client_reset_stats(client);
    
    http_response_t *response = http_get(client, "https://httpbin.org/redirect/1");
    check(response != NULL);
    http_response_free(response);
    
    http_client_stats_t stats;
    http_client_get_stats(client, &stats);
    
    check(stats.redirects_followed == 1);
  }

  it("should successfully reset statistics") {
    http_get(client, "https://httpbin.org/get");
    http_client_reset_stats(client);
    
    http_client_stats_t stats;
    http_client_get_stats(client, &stats);
    
    check(stats.total_requests == 0);
  }
}

