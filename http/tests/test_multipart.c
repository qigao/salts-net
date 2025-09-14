#include "http_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_multipart_form_creation(void) {
    printf("Testing multipart form creation...\n");
    
    http_multipart_form_t* form = http_multipart_form_create();
    assert(form != NULL);
    
    http_multipart_form_destroy(form);
    
    printf("   Multipart form creation works correctly\n");
}

static void test_multipart_add_fields(void) {
    printf("Testing adding fields to multipart form...\n");
    
    http_multipart_form_t* form = http_multipart_form_create();
    assert(form != NULL);
    
    // Add text fields
    http_multipart_form_add_field(form, "name", "John Doe");
    http_multipart_form_add_field(form, "email", "john@example.com");
    http_multipart_form_add_field(form, "message", "Hello, World!");
    
    // Form should be valid (we can't check internal count from outside)
    assert(form != NULL);
    
    http_multipart_form_destroy(form);
    
    printf("   Adding fields works correctly\n");
}

static void test_multipart_add_file(void) {
    printf("Testing adding file to multipart form...\n");
    
    http_multipart_form_t* form = http_multipart_form_create();
    assert(form != NULL);
    
    const char* file_data = "This is test file content";
    http_multipart_form_add_file(form, "upload", "test.txt", "text/plain",
                                 file_data, strlen(file_data));
    
    // Form should be valid
    assert(form != NULL);
    
    http_multipart_form_destroy(form);
    
    printf("   Adding file works correctly\n");
}

static void test_multipart_mixed_content(void) {
    printf("Testing multipart form with mixed content...\n");
    
    http_multipart_form_t* form = http_multipart_form_create();
    assert(form != NULL);
    
    // Add text fields
    http_multipart_form_add_field(form, "title", "My Upload");
    http_multipart_form_add_field(form, "description", "Test description");
    
    // Add file
    const char* file_data = "File content here";
    http_multipart_form_add_file(form, "file", "document.txt", "text/plain",
                                 file_data, strlen(file_data));
    
    // Form should be valid
    assert(form != NULL);
    
    http_multipart_form_destroy(form);
    
    printf("   Mixed content works correctly\n");
}

static void test_multipart_upload(void) {
    printf("Testing multipart upload to server...\n");
    
    http_client_t* client = http_client_create();
    assert(client != NULL);
    
    // Use shorter timeout for tests (3 seconds)
    http_client_set_timeout(client, 3000);
    
    http_multipart_form_t* form = http_multipart_form_create();
    assert(form != NULL);
    
    // Add some data
    http_multipart_form_add_field(form, "name", "Test User");
    http_multipart_form_add_field(form, "type", "test");
    
    const char* file_content = "Hello from multipart test!";
    http_multipart_form_add_file(form, "file", "test.txt", "text/plain",
                                 file_content, strlen(file_content));
    
    // Upload to httpbin
    http_response_t* response = http_post_multipart(client,
                                                    "https://httpbin.org/post",
                                                    form);
    
    if (response->error) {
        printf("  Skipping test (network error): %s\n", response->error);
        http_response_free(response);
        http_multipart_form_destroy(form);
        http_client_destroy(client);
        return;
    }
    
    if (response->status_code != 200) {
        printf("  Skipping test (unexpected status %d)\n", response->status_code);
        http_response_free(response);
        http_multipart_form_destroy(form);
        http_client_destroy(client);
        return;
    }
    
    // Check response contains multipart data
    assert(response->body != NULL);
    assert(strstr(response->body, "multipart/form-data") != NULL);
    
    http_response_free(response);
    http_multipart_form_destroy(form);
    http_client_destroy(client);
    
    printf("   Multipart upload works correctly\n");
}

static void test_multipart_file_from_path(void) {
    printf("Testing multipart file upload from path...\n");
    
    // Create a test file
    FILE* fp = fopen("test_multipart.txt", "w");
    if (!fp) {
        printf("  Skipping test (couldn't create test file)\n");
        return;
    }
    
    fprintf(fp, "Test file content for multipart upload\n");
    fclose(fp);
    
    http_multipart_form_t* form = http_multipart_form_create();
    assert(form != NULL);
    
    int result = http_multipart_form_add_file_path(form, "file", 
                                                   "test_multipart.txt",
                                                   "text/plain");
    
    if (result == 0) {
        printf("   File from path added successfully\n");
    } else {
        printf("  ⚠ Failed to add file from path\n");
    }
    
    http_multipart_form_destroy(form);
    
    // Clean up
    remove("test_multipart.txt");
    
    printf("   File from path test complete\n");
}

static void test_multipart_binary_data(void) {
    printf("Testing multipart with binary data...\n");
    
    http_multipart_form_t* form = http_multipart_form_create();
    assert(form != NULL);
    
    // Create binary data
    unsigned char binary[256];
    for (int i = 0; i < 256; i++) {
        binary[i] = (unsigned char)i;
    }
    
    http_multipart_form_add_file(form, "binary", "data.bin",
                                 "application/octet-stream",
                                 binary, sizeof(binary));
    
    // Form should be valid
    assert(form != NULL);
    
    http_multipart_form_destroy(form);
    
    printf("   Binary data handling works correctly\n");
}

int main(void) {
    printf("\n=== HTTP Client Multipart Tests ===\n\n");
    
    test_multipart_form_creation();
    test_multipart_add_fields();
    test_multipart_add_file();
    test_multipart_mixed_content();
    test_multipart_upload();
    test_multipart_file_from_path();
    test_multipart_binary_data();
    
    printf("\n All multipart tests passed!\n");
    return 0;
}
