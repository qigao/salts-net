#include "tinytest.h"
#include "http_client.h"
#include <string.h>
#include <stdio.h>

spec("http multipart") {

    describe("form creation") {

        it("should create and destroy") {
            http_multipart_form_t *form = http_multipart_form_create();
            check_not_null(form);
            http_multipart_form_destroy(form);
        }

        it("should add text fields") {
            http_multipart_form_t *form = http_multipart_form_create();
            http_multipart_form_add_field(form, "name", "John Doe");
            http_multipart_form_add_field(form, "email", "john@example.com");
            check_not_null(form);
            http_multipart_form_destroy(form);
        }

        it("should add file data") {
            http_multipart_form_t *form = http_multipart_form_create();
            const char *data = "This is test file content";
            http_multipart_form_add_file(form, "upload", "test.txt", "text/plain",
                                         data, strlen(data));
            check_not_null(form);
            http_multipart_form_destroy(form);
        }

        it("should add mixed content") {
            http_multipart_form_t *form = http_multipart_form_create();
            http_multipart_form_add_field(form, "title", "My Upload");
            const char *data = "File content here";
            http_multipart_form_add_file(form, "file", "doc.txt", "text/plain",
                                         data, strlen(data));
            check_not_null(form);
            http_multipart_form_destroy(form);
        }

        it("should handle binary data") {
            http_multipart_form_t *form = http_multipart_form_create();
            unsigned char binary[256];
            for (int i = 0; i < 256; i++) binary[i] = (unsigned char)i;
            http_multipart_form_add_file(form, "binary", "data.bin",
                                         "application/octet-stream",
                                         binary, sizeof(binary));
            check_not_null(form);
            http_multipart_form_destroy(form);
        }
    }

    describe("upload") {

        it("should upload multipart form") {
            http_client_t *client = http_client_create();
            http_client_set_timeout(client, 3000);

            http_multipart_form_t *form = http_multipart_form_create();
            http_multipart_form_add_field(form, "name", "Test User");
            const char *content = "Hello from multipart test!";
            http_multipart_form_add_file(form, "file", "test.txt", "text/plain",
                                         content, strlen(content));

            http_response_t *response = http_post_multipart(client,
                "https://httpbin.org/post", form);

            if (response->error) {
                http_response_free(response);
                http_multipart_form_destroy(form);
                http_client_destroy(client);
                return;
            }

            check_int_eq(response->status_code, 200);
            check_not_null(response->body);
            check_str_contains(response->body, "multipart/form-data");

            http_response_free(response);
            http_multipart_form_destroy(form);
            http_client_destroy(client);
        }

        it("should add file from path") {
            FILE *fp = fopen("test_multipart.txt", "w");
            if (!fp) return;
            fprintf(fp, "Test file content for multipart upload\n");
            fclose(fp);

            http_multipart_form_t *form = http_multipart_form_create();
            int result = http_multipart_form_add_file_path(form, "file",
                "test_multipart.txt", "text/plain");
            check_int_eq(result, 0);

            http_multipart_form_destroy(form);
            remove("test_multipart.txt");
        }
    }
}
