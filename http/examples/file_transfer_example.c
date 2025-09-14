#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    printf("=== File Upload/Download Example ===\n\n");
    
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        return 1;
    }

    printf("1. Creating test file for upload...\n");
    
    // Create a test file
    FILE* fp = fopen("test_upload_file.txt", "w");
    if (!fp) {
        fprintf(stderr, "Failed to create test file\n");
        http_client_destroy(client);
        return 1;
    }
    
    fprintf(fp, "This is a test file for upload demonstration.\n");
    fprintf(fp, "It contains multiple lines of text.\n");
    fprintf(fp, "Line 3\n");
    fprintf(fp, "Line 4\n");
    fprintf(fp, "End of file.\n");
    fclose(fp);
    
    printf("   Created test_upload_file.txt\n");
    
    printf("\n2. Uploading file to httpbin.org...\n");
    
    // Upload file
    http_response_t* response = http_upload_file(client,
                                                 "https://httpbin.org/post",
                                                 "test_upload_file.txt");
    
    if (response->error) {
        printf("   Error: %s (code: %d)\n", response->error, response->error_code);
    } else {
        printf("   Status: %d\n", response->status_code);
        printf("   Upload successful!\n");
        
        if (response->body && response->body_len > 0) {
            printf("   Response length: %zu bytes\n", response->body_len);
        }
    }
    
    http_response_free(response);
    
    printf("\n3. Downloading file from internet...\n");
    
    // Download a file
    response = http_download_file(client,
                                  "https://httpbin.org/json",
                                  "downloaded_file.json");
    
    if (response->error) {
        printf("   Error: %s (code: %d)\n", response->error, response->error_code);
    } else {
        printf("   Status: %d\n", response->status_code);
        printf("   File downloaded to: downloaded_file.json\n");
        
        // Verify file was created
        FILE* verify = fopen("downloaded_file.json", "r");
        if (verify) {
            fseek(verify, 0, SEEK_END);
            long size = ftell(verify);
            fclose(verify);
            printf("   Downloaded file size: %ld bytes\n", size);
        }
    }
    
    http_response_free(response);
    
    printf("\n4. Testing large file upload (simulated)...\n");
    
    // Create a larger test file
    fp = fopen("large_test_file.bin", "wb");
    if (fp) {
        // Write 1MB of data
        char buffer[1024];
        memset(buffer, 'A', sizeof(buffer));
        
        for (int i = 0; i < 1024; i++) {
            fwrite(buffer, 1, sizeof(buffer), fp);
        }
        fclose(fp);
        
        printf("   Created 1MB test file\n");
        
        // Upload it
        response = http_upload_file(client,
                                    "https://httpbin.org/post",
                                    "large_test_file.bin");
        
        if (response->error) {
            printf("   Error: %s\n", response->error);
        } else {
            printf("   Status: %d\n", response->status_code);
            printf("   Large file upload successful!\n");
        }
        
        http_response_free(response);
        
        // Clean up
        remove("large_test_file.bin");
    }
    
    printf("\n5. Testing download with progress tracking...\n");
    
    // Note: Progress callback would be used here in a real implementation
    printf("   (Progress tracking requires streaming implementation)\n");
    
    // Cleanup
    remove("test_upload_file.txt");
    remove("downloaded_file.json");
    
    http_client_destroy(client);
    
    printf("\n File transfer example complete!\n");
    printf("\nKey features:\n");
    printf("- Upload files directly from disk\n");
    printf("- Download files directly to disk\n");
    printf("- Memory-efficient for large files\n");
    printf("- Simple API for common use cases\n");
    
    return 0;
}
