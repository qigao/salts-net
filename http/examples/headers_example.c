#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  http_client_t *client = http_client_create();
  if (!client) {
    fprintf(stderr, "Failed to create HTTP client\n");
    return 1;
  }

  printf("=== Response Headers Example ===\n\n");

  // Make a request to httpbin.org
  printf("Fetching https://httpbin.org/json...\n\n");
  http_response_t *response = http_get(client, "https://httpbin.org/json");

  if (response->error) {
    printf("Error: %s (code: %d)\n", response->error, response->error_code);
    http_response_free(response);
    http_client_destroy(client);
    return 1;
  }

  printf("Status Code: %d\n\n", response->status_code);

  // Get specific headers
  printf("=== Specific Headers ===\n");
  char *content_type = (char *)http_response_content_type(response);
  if (content_type) {
    printf("Content-Type: %s\n", content_type);
    free(content_type);
  }

  size_t content_length = http_response_content_length(response);
  printf("Content-Length: %zu\n", content_length);

  char *server = (char *)http_response_get_header(response, "Server");
  if (server) {
    printf("Server: %s\n", server);
    free(server);
  }

  char *date = (char *)http_response_get_header(response, "Date");
  if (date) {
    printf("Date: %s\n", date);
    free(date);
  }

  printf("\n=== Content Type Checks ===\n");
  printf("Is JSON? %s\n", http_response_is_json(response) ? "Yes" : "No");
  printf("Is HTML? %s\n", http_response_is_html(response) ? "Yes" : "No");
  printf("Is Text? %s\n", http_response_is_text(response) ? "Yes" : "No");

  printf("\n=== Response Body ===\n");
  printf("%s\n", response->body);

  http_response_free(response);

  // Test with HTML
  printf("\n\n=== Testing with HTML ===\n");
  response = http_get(client, "https://httpbin.org/html");

  if (!response->error) {
    printf("Is JSON? %s\n", http_response_is_json(response) ? "Yes" : "No");
    printf("Is HTML? %s\n", http_response_is_html(response) ? "Yes" : "No");
    printf("Is Text? %s\n", http_response_is_text(response) ? "Yes" : "No");
  }

  http_response_free(response);
  http_client_destroy(client);
  return 0;
}
