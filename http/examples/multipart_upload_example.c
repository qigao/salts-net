#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    printf("=== Multipart Form Upload Example ===\n\n");
    
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        return 1;
    }

    // Create multipart form
    http_multipart_form_t* form = http_multipart_form_create();
    if (!form) {
        fprintf(stderr, "Failed to create multipart form\n");
        http_client_destroy(client);
        return 1;
    }
    
    printf("1. Adding text fields to form...\n");
    
    // Add regular form fields
    http_multipart_form_add_field(form, "title", "My Document");
    http_multipart_form_add_field(form, "description", "This is a test upload");
    http_multipart_form_add_field(form, "category", "testing");
    
    printf("   Added 3 text fields\n");
    
    printf("\n2. Adding file from memory...\n");
    
    // Create a simple text file in memory
    const char* file_content = "Hello, World!\nThis is a test file.\nLine 3 of the file.";
    http_multipart_form_add_file(form, "file", "test.txt", "text/plain",
                                 file_content, strlen(file_content));
    
    printf("   Added test.txt (%zu bytes)\n", strlen(file_content));
    
    printf("\n3. Adding another file (simulated binary data)...\n");
    
    // Simulate binary data
    unsigned char binary_data[256];
    for (int i = 0; i < 256; i++) {
        binary_data[i] = (unsigned char)i;
    }
    
    http_multipart_form_add_file(form, "binary", "data.bin", "application/octet-stream",
                                 binary_data, sizeof(binary_data));
    
    printf("   Added data.bin (%zu bytes)\n", sizeof(binary_data));
    
    printf("\n4. Uploading to httpbin.org...\n");
    
    // Upload the form
    http_response_t* response = http_post_multipart(client,
                                                    "https://httpbin.org/post",
                                                    form);
    
    if (response->error) {
        printf("   Error: %s (code: %d)\n", response->error, response->error_code);
    } else {
        printf("   Status: %d\n", response->status_code);
        printf("   Response length: %zu bytes\n\n", response->body_len);
        
        // Show part of the response
        if (response->body) {
            printf("   Response preview:\n");
            // Print first 500 chars
            size_t preview_len = response->body_len < 500 ? response->body_len : 500;
            printf("   %.*s", (int)preview_len, response->body);
            if (response->body_len > 500) {
                printf("\n   ... (truncated)");
            }
            printf("\n");
        }
    }
    
    http_response_free(response);
    
    printf("\n5. Testing file upload from path (if file exists)...\n");
    
    // Create a test file
    FILE* fp = fopen("test_upload.txt", "w");
    if (fp) {
        fprintf(fp, "This is a test file created for upload demonstration.\n");
        fprintf(fp, "It contains multiple lines.\n");
        fprintf(fp, "Line 3\n");
        fclose(fp);
        
        // Create new form with file from path
        http_multipart_form_t* form2 = http_multipart_form_create();
        http_multipart_form_add_field(form2, "description", "File from disk");
        
        if (http_multipart_form_add_file_path(form2, "file", "test_upload.txt", "text/plain") == 0) {
            printf("   Successfully added file from path\n");
            
            response = http_post_multipart(client, "https://httpbin.org/post", form2);
            
            if (!response->error) {
                printf("   Upload successful! Status: %d\n", response->status_code);
            } else {
                printf("   Upload failed: %s\n", response->error);
            }
            
            http_response_free(response);
        } else {
            printf("   Failed to add file from path\n");
        }
        
        http_multipart_form_destroy(form2);
        
        // Clean up test file
        remove("test_upload.txt");
    } else {
        printf("   Skipping file path test (couldn't create test file)\n");
    }
    
    // Cleanup
    http_multipart_form_destroy(form);
    http_client_destroy(client);
    
    printf("\n Multipart upload example complete!\n");
    return 0;
}
