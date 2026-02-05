#include "bdd-for-c.h"
#include "http_client.h"
#include <string.h>

spec("Multipart Form Upload Test") {
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

  it("should successfully perform a multipart form upload with memory data") {
    http_multipart_form_t *form = http_multipart_form_create();
    check(form != NULL);
    
    http_multipart_form_add_field(form, "title", "My Document");
    
    const char *file_content = "Hello, World!\nThis is a test file.";
    http_multipart_form_add_file(form, "file", "test.txt", "text/plain",
                                 file_content, strlen(file_content));
    
    http_response_t *response = http_post_multipart(client,
                                                    "https://httpbin.org/post",
                                                    form);
    
    check(response != NULL);
    check(response->error == NULL);
    check(response->status_code == 200);
    check(strstr(response->body, "My Document") != NULL);
    check(strstr(response->body, "test.txt") != NULL);
    
    http_response_free(response);
    http_multipart_form_destroy(form);
  }

  it("should successfully perform a multipart form upload with a file from path") {
    // Create a test file
    FILE *fp = fopen("test_upload.txt", "w");
    if (fp) {
      fprintf(fp, "This is a test file created for upload demonstration.\n");
      fclose(fp);
      
      http_multipart_form_t *form = http_multipart_form_create();
      check(form != NULL);
      http_multipart_form_add_field(form, "disk_file_desc", "File from disk");
      
      int res = http_multipart_form_add_file_path(form, "file", "test_upload.txt", "text/plain");
      check(res == 0);
      
      http_response_t *response = http_post_multipart(client, "https://httpbin.org/post", form);
      
      check(response != NULL);
      check(response->error == NULL);
      check(response->status_code == 200);
      check(strstr(response->body, "test_upload.txt") != NULL);
      
      http_response_free(response);
      http_multipart_form_destroy(form);
      remove("test_upload.txt");
    }
  }
}

