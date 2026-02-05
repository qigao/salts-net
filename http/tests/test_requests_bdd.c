
#include "bdd-for-c.h"
#include "http_client.h"

spec("HTTP Request Methods") {
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

    it("should handle GET request structure and errors") {
        check(client != NULL);

        // Test NULL URL handling
        http_response_t *response = http_get(client, NULL);
        check(response != NULL);
        check(response->error != NULL);
        http_response_free(response);
    }

    it("should handle POST request structure and errors") {
        const char *body = "{\"test\":\"data\"}";
        http_response_t *response = http_post(client, NULL, body, strlen(body));
        check(response != NULL);
        check(response->error != NULL);
        http_response_free(response);
    }

    it("should handle POST with empty body") {
        http_response_t *response = http_post(client, NULL, NULL, 0);
        check(response != NULL);
        check(response->error != NULL);
        http_response_free(response);
    }

    it("should handle custom headers") {
        const char *headers[] = {"X-Custom-Header: value1",
                                 "X-Another-Header: value2"};
        
        http_response_t *response =
            http_request(client, HTTP_GET, NULL, headers, 2, NULL, 0);
        check(response != NULL);
        check(response->error != NULL);
        http_response_free(response);
    }

    it("should handle request with body and headers") {
        const char *headers[] = {"Content-Type: application/json"};
        const char *body = "{\"key\":\"value\"}";

        http_response_t *response = 
            http_request(client, HTTP_POST, NULL, headers, 1, body, strlen(body));
        check(response != NULL);
        check(response->error != NULL);
        http_response_free(response);
    }

    it("should support different HTTP methods") {
        http_method_t methods[] = {HTTP_GET,    HTTP_POST, HTTP_PUT,
                                   HTTP_DELETE, HTTP_HEAD, HTTP_PATCH};
        
        for (int i = 0; i < 6; i++) {
            http_response_t *response =
                http_request(client, methods[i], NULL, NULL, 0, NULL, 0);
            check(response != NULL);
            check(response->error != NULL);
            http_response_free(response);
        }
    }

    it("should handle URLs with query params") {
        http_client_set_timeout(client, 100); 
        http_response_t *response =
            http_get(client, "http://127.0.0.1:1/test?param1=value1&param2=value2");
        check(response != NULL);
        http_response_free(response);
    }

    it("should handle URLs with fragments") {
        http_client_set_timeout(client, 100);
        http_response_t *response =
            http_get(client, "http://127.0.0.1:1/test#fragment");
        check(response != NULL);
        http_response_free(response);
    }
}
