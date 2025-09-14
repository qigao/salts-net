#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char *url = "http://httpbin.org/post";
  const char *json = "{\"name\":\"John\",\"age\":30}";

  printf("Posting JSON to: %s\n\n", url);

  // Create client
  http_client_t *client = http_client_create();
  if (!client) {
    fprintf(stderr, "Failed to create client\n");
    return 1;
  }

  // Custom headers
  const char *headers[] = {"Content-Type: application/json",
                           "Accept: application/json"};

  // Make POST request
  http_response_t *response =
      http_request(client, HTTP_POST, url, headers, 2, json, strlen(json));

  // Check for errors
  if (response->error) {
    fprintf(stderr, "Error: %s\n", response->error);
    http_response_free(response);
    http_client_destroy(client);
    return 1;
  }

  // Print response
  printf("Status: %d\n", response->status_code);
  printf("\nBody:\n%s\n", response->body);

  // Cleanup
  http_response_free(response);
  http_client_destroy(client);

  return 0;
}
