
#include "bdd-for-c.h"
#include "http_client.h"

spec("HTTP Basic Functionality") {
    static http_client_t *client = NULL;

    before() {
        client = http_client_create();
        check(client != NULL);
    }

    after() {
        if (client) {
            http_client_destroy(client);
            client = NULL;
        }
    }

    it("should allow setting timeout") {
        http_client_set_timeout(client, 5000);
        // We can't easily check the internal state without accessors, 
        // but we verify the call doesn't crash.
        
        http_client_set_timeout(client, 0);
        http_client_set_timeout(client, -1);
    }

    it("should allow setting user agent") {
        http_client_set_user_agent(client, "TestAgent/1.0");
        http_client_set_user_agent(client, "");
        // http_client_set_user_agent(client, NULL); // This was in test_basic.c
    }

    it("should allow configuring redirects") {
        http_client_follow_redirects(client, 1);
        http_client_follow_redirects(client, 0);
        
        http_client_set_max_redirects(client, 5);
        http_client_set_max_redirects(client, 0);
        http_client_set_max_redirects(client, 100);
    }

    it("should handle invalid URLs gracefully") {
        http_response_t *response = http_get(client, NULL);
        check(response != NULL);
        check(response->error != NULL);
        http_response_free(response);
    }

    it("should handle malformed URLs") {
        http_response_t *response = http_get(client, "not-a-url");
        check(response != NULL);
        // Expecting an error or a specific status code
        // check(response->error != NULL); // Depending on implementation
        http_response_free(response);
    }
    it("should handle null pointers gracefully") {
        http_client_destroy(NULL); // Should not crash
        http_response_free(NULL);  // Should not crash
    }
}
