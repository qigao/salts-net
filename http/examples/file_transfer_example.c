#include "bdd-for-c.h"
#include "http_client.h"

spec("File Transfer Test") {
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

  it("should successfully upload and download a file") {
    // Create a test file
    FILE *fp = fopen("test_upload_file.txt", "w");
    if (fp) {
      fprintf(fp, "This is a test file for upload demonstration.");
      fclose(fp);
      
      http_response_t *response = http_upload_file(client, "https://httpbin.org/post", "test_upload_file.txt");
      check(response != NULL);
      check(response->error == NULL);
      check(response->status_code == 200);
      http_response_free(response);
      
      response = http_download_file(client, "https://httpbin.org/json", "downloaded_file.json");
      check(response != NULL);
      check(response->error == NULL);
      check(response->status_code == 200);
      http_response_free(response);
      
      remove("test_upload_file.txt");
      remove("downloaded_file.json");
    }
  }
}

