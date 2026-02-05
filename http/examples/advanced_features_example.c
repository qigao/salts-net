#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include "bdd-for-c.h"
#include "http_client.h"

spec("Advanced Features Test") {
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

  it("should successfully apply custom timeouts") {
    http_client_set_connect_timeout(client, 3000);
    http_client_set_read_timeout(client, 10000);
    
    http_response_t *response = http_get(client, "https://httpbin.org/delay/1");
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    
    http_response_free(response);
  }

  it("should successfully handle compressed responses") {
    http_client_enable_compression(client, 1);
    
    http_response_t *response = http_get(client, "https://httpbin.org/gzip");
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    
    char *encoding = http_response_get_header(response, "Content-Encoding");
    if (encoding) {
        check(strstr(encoding, "gzip") != NULL);
        free(encoding);
    }
    
    http_response_free(response);
  }

  it("should successfully perform range requests") {
    http_response_t *response = http_get_range(client, "https://httpbin.org/bytes/1000", 0, 99);
    
    check(response != NULL);
    check(response->error == NULL);
    if (response->status_code == 206) {
        check(response->body_len == 100);
    }
    
    http_response_free(response);
  }
}

