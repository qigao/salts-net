#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  const char *url = argc > 1 ? argv[1] : "http://httpbin.org/get";

  printf("Fetching: %s\n\n", url);

  // Create client
  http_client_t *client = http_client_create();
  if (!client) {
    fprintf(stderr, "Failed to create client\n");
    return 1;
  }

  // Make GET request
  http_response_t *response = http_get(client, url);

  // Check for errors
  if (response->error) {
    fprintf(stderr, "Error: %s\n", response->error);
    http_response_free(response);
    http_client_destroy(client);
    return 1;
  }

  // Print response
  printf("Status: %d\n", response->status_code);
  printf("\nHeaders:\n%s\n",
         response->headers ? response->headers : "(no headers)");
  printf("\nBody:\n%s\n", response->body ? response->body : "(no body)");

  // Cleanup
  http_response_free(response);
  http_client_destroy(client);

  return 0;
}
