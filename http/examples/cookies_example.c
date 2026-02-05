#include "bdd-for-c.h"
#include "http_client.h"

spec("Cookie Management Test") {
  static http_client_t *client = NULL;
  static http_cookie_jar_t *jar = NULL;

  before() {
    client = http_client_create();
    jar = http_cookie_jar_create();
    http_client_set_cookie_jar(client, jar);
    check(client != NULL);
    check(jar != NULL);
  }

  after() {
    if (client) {
      http_client_destroy(client);
    }
    if (jar) {
      http_cookie_jar_destroy(jar);
    }
  }

  it("should successfully handle automatic cookies") {
    http_client_follow_redirects(client, 0);
    http_response_t *response = http_get(client, "https://httpbin.org/cookies/set?session=abc123");
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 302);
    check(http_cookie_jar_count(jar) >= 1);
    
    const char *session = http_cookie_jar_get(jar, "session");
    check(session != NULL);
    check(strcmp(session, "abc123") == 0);
    
    http_response_free(response);

    // Second request should send cookies automatically
    http_client_follow_redirects(client, 1);
    response = http_get(client, "https://httpbin.org/cookies");
    check(response != NULL);
    check(response->error == NULL);
    check(strstr(response->body, "\"session\": \"abc123\"") != NULL);
    http_response_free(response);
  }


  it("should successfully perform manual cookie operations") {
    http_cookie_jar_set(jar, "user_id", "12345");
    http_cookie_jar_set(jar, "preferences", "dark_mode");
    
    check(http_cookie_jar_count(jar) >= 2);
    
    check(strcmp(http_cookie_jar_get(jar, "user_id"), "12345") == 0);
    check(strcmp(http_cookie_jar_get(jar, "preferences"), "dark_mode") == 0);
    
    http_cookie_jar_remove(jar, "user_id");
    check(http_cookie_jar_get(jar, "user_id") == NULL);
    
    http_cookie_jar_clear(jar);
    check(http_cookie_jar_count(jar) == 0);
  }
}

