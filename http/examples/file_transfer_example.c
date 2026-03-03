#include "tinytest.h"
#include "http_client.h"
#include <stdio.h>

static int is_network_error(http_response_t *r) {
  if (!r) return 1;
  return (r->error_code == HTTP_ERROR_CONNECTION_FAILED ||
          r->error_code == HTTP_ERROR_TIMEOUT ||
          r->error_code == HTTP_ERROR_DNS_FAILED);
}

spec("File Transfer Test") {

  it("should successfully upload and download a file") {
    FILE *fp = fopen("test_upload_file.txt", "w");
    if (!fp) return;
    fprintf(fp, "This is a test file for upload demonstration.");
    fclose(fp);

    http_client_t *c = http_client_create();
    http_client_set_timeout(c, 10000);

    http_response_t *r = http_upload_file(c, "https://httpbin.org/post", "test_upload_file.txt");
    if (is_network_error(r)) {
      http_response_free(r);
      http_client_destroy(c);
      remove("test_upload_file.txt");
      return;
    }
    check_int_eq(r->status_code, 200);
    http_response_free(r);

    r = http_download_file(c, "https://httpbin.org/json", "downloaded_file.json");
    if (!is_network_error(r))
      check_int_eq(r->status_code, 200);
    http_response_free(r);

    http_client_destroy(c);
    remove("test_upload_file.txt");
    remove("downloaded_file.json");
  }
}
