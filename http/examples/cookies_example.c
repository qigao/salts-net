#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("=== Cookie Management Example ===\n\n");
    
    http_client_t* client = http_client_create();
    if (!client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        return 1;
    }

    // Create a cookie jar
    http_cookie_jar_t* jar = http_cookie_jar_create();
    
    // Attach cookie jar to client for automatic cookie handling
    http_client_set_cookie_jar(client, jar);
    
    printf("1. Testing automatic cookie handling with httpbin.org...\n");
    
    // This endpoint sets a cookie
    http_response_t* response = http_get(client, "https://httpbin.org/cookies/set?session=abc123");
    
    if (response->error) {
        printf("   Error: %s\n", response->error);
    } else {
        printf("   Status: %d\n", response->status_code);
        printf("   Cookies in jar: %d\n", http_cookie_jar_count(jar));
        
        // Check if cookie was stored
        const char* session = http_cookie_jar_get(jar, "session");
        if (session) {
            printf("   Session cookie value: %s\n", session);
        }
    }
    
    http_response_free(response);
    
    printf("\n2. Making another request - cookies should be sent automatically...\n");
    
    // This endpoint returns the cookies we sent
    response = http_get(client, "https://httpbin.org/cookies");
    
    if (!response->error) {
        printf("   Status: %d\n", response->status_code);
        printf("   Response:\n%s\n", response->body);
    }
    
    http_response_free(response);
    
    printf("\n3. Manual cookie operations...\n");
    
    // Manually set a cookie
    http_cookie_jar_set(jar, "user_id", "12345");
    http_cookie_jar_set(jar, "preferences", "dark_mode");
    
    printf("   Added 2 cookies manually\n");
    printf("   Total cookies: %d\n", http_cookie_jar_count(jar));
    
    // Get cookie values
    const char* user_id = http_cookie_jar_get(jar, "user_id");
    const char* prefs = http_cookie_jar_get(jar, "preferences");
    
    printf("   user_id: %s\n", user_id ? user_id : "not found");
    printf("   preferences: %s\n", prefs ? prefs : "not found");
    
    printf("\n4. Testing cookie removal...\n");
    
    http_cookie_jar_remove(jar, "user_id");
    printf("   Removed 'user_id' cookie\n");
    printf("   Total cookies: %d\n", http_cookie_jar_count(jar));
    
    printf("\n5. Clearing all cookies...\n");
    
    http_cookie_jar_clear(jar);
    printf("   Cleared all cookies\n");
    printf("   Total cookies: %d\n", http_cookie_jar_count(jar));
    
    printf("\n6. Testing session persistence...\n");
    
    // Set a cookie and make multiple requests
    http_cookie_jar_set(jar, "api_key", "secret-key-123");
    
    for (int i = 0; i < 3; i++) {
        printf("   Request %d...\n", i + 1);
        response = http_get(client, "https://httpbin.org/cookies");
        
        if (!response->error) {
            printf("     Status: %d (cookie sent automatically)\n", response->status_code);
        }
        
        http_response_free(response);
    }
    
    // Cleanup
    http_cookie_jar_destroy(jar);
    http_client_destroy(client);
    
    printf("\n Cookie management example complete!\n");
    return 0;
}
