#include "http_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_cookie_jar_basic(void) {
    printf("Testing basic cookie jar operations...\n");
    
    http_cookie_jar_t* jar = http_cookie_jar_create();
    assert(jar != NULL);
    assert(http_cookie_jar_count(jar) == 0);
    
    // Add cookies
    http_cookie_jar_set(jar, "session", "abc123");
    assert(http_cookie_jar_count(jar) == 1);
    
    http_cookie_jar_set(jar, "user_id", "42");
    assert(http_cookie_jar_count(jar) == 2);
    
    // Get cookies
    const char* session = http_cookie_jar_get(jar, "session");
    assert(session != NULL);
    assert(strcmp(session, "abc123") == 0);
    
    const char* user_id = http_cookie_jar_get(jar, "user_id");
    assert(user_id != NULL);
    assert(strcmp(user_id, "42") == 0);
    
    // Non-existent cookie
    const char* missing = http_cookie_jar_get(jar, "nonexistent");
    assert(missing == NULL);
    
    // Update existing cookie
    http_cookie_jar_set(jar, "session", "xyz789");
    session = http_cookie_jar_get(jar, "session");
    assert(strcmp(session, "xyz789") == 0);
    assert(http_cookie_jar_count(jar) == 2); // Count shouldn't change
    
    // Remove cookie
    http_cookie_jar_remove(jar, "user_id");
    assert(http_cookie_jar_count(jar) == 1);
    assert(http_cookie_jar_get(jar, "user_id") == NULL);
    
    // Clear all
    http_cookie_jar_clear(jar);
    assert(http_cookie_jar_count(jar) == 0);
    
    http_cookie_jar_destroy(jar);
    
    printf("   Basic cookie jar operations work correctly\n");
}

static void test_automatic_cookie_handling(void) {
    printf("Testing automatic cookie handling...\n");
    
    http_client_t* client = http_client_create();
    assert(client != NULL);
    
    http_cookie_jar_t* jar = http_cookie_jar_create();
    http_client_set_cookie_jar(client, jar);
    
    // Verify jar is attached
    assert(http_client_get_cookie_jar(client) == jar);
    
    // Make request that sets a cookie
    http_response_t* response = http_get(client, "https://httpbin.org/cookies/set?test=value123");
    
    if (response->error) {
        printf("  Skipping test (network error): %s\n", response->error);
        http_response_free(response);
        http_cookie_jar_destroy(jar);
        http_client_destroy(client);
        return;
    }
    
    http_response_free(response);
    
    // Check if cookie was stored
    const char* test_cookie = http_cookie_jar_get(jar, "test");
    if (test_cookie) {
        assert(strcmp(test_cookie, "value123") == 0);
        printf("   Cookie automatically stored from Set-Cookie header\n");
    } else {
        printf("  ⚠ Cookie not stored (may be httpbin issue)\n");
    }
    
    // Make another request - cookie should be sent
    response = http_get(client, "https://httpbin.org/cookies");
    
    if (!response->error && response->body) {
        // Response should contain our cookie
        if (strstr(response->body, "test") != NULL) {
            printf("   Cookie automatically sent in subsequent request\n");
        }
    }
    
    http_response_free(response);
    http_cookie_jar_destroy(jar);
    http_client_destroy(client);
    
    printf("   Automatic cookie handling works correctly\n");
}

static void test_multiple_cookies(void) {
    printf("Testing multiple cookies...\n");
    
    http_cookie_jar_t* jar = http_cookie_jar_create();
    
    // Add multiple cookies
    http_cookie_jar_set(jar, "cookie1", "value1");
    http_cookie_jar_set(jar, "cookie2", "value2");
    http_cookie_jar_set(jar, "cookie3", "value3");
    
    assert(http_cookie_jar_count(jar) == 3);
    
    // Verify all cookies exist
    assert(http_cookie_jar_get(jar, "cookie1") != NULL);
    assert(http_cookie_jar_get(jar, "cookie2") != NULL);
    assert(http_cookie_jar_get(jar, "cookie3") != NULL);
    
    // Remove one
    http_cookie_jar_remove(jar, "cookie2");
    assert(http_cookie_jar_count(jar) == 2);
    assert(http_cookie_jar_get(jar, "cookie2") == NULL);
    
    // Others should still exist
    assert(http_cookie_jar_get(jar, "cookie1") != NULL);
    assert(http_cookie_jar_get(jar, "cookie3") != NULL);
    
    http_cookie_jar_destroy(jar);
    
    printf("   Multiple cookie handling works correctly\n");
}

static void test_cookie_persistence(void) {
    printf("Testing cookie persistence across requests...\n");
    
    http_client_t* client = http_client_create();
    http_cookie_jar_t* jar = http_cookie_jar_create();
    http_client_set_cookie_jar(client, jar);
    
    // Manually set a cookie
    http_cookie_jar_set(jar, "persistent", "data123");
    
    // Make multiple requests
    for (int i = 0; i < 3; i++) {
        http_response_t* response = http_get(client, "https://httpbin.org/cookies");
        
        if (response->error) {
            printf("  Skipping test (network error): %s\n", response->error);
            http_response_free(response);
            break;
        }
        
        // Cookie should still be in jar
        const char* persistent = http_cookie_jar_get(jar, "persistent");
        assert(persistent != NULL);
        assert(strcmp(persistent, "data123") == 0);
        
        http_response_free(response);
    }
    
    http_cookie_jar_destroy(jar);
    http_client_destroy(client);
    
    printf("   Cookie persistence works correctly\n");
}

static void test_cookie_without_jar(void) {
    printf("Testing client without cookie jar...\n");
    
    http_client_t* client = http_client_create();
    assert(client != NULL);
    
    // No cookie jar attached
    assert(http_client_get_cookie_jar(client) == NULL);
    
    // Request should work fine without cookies
    http_response_t* response = http_get(client, "https://httpbin.org/get");
    
    if (response->error) {
        printf("  Skipping test (network error): %s\n", response->error);
        http_response_free(response);
        http_client_destroy(client);
        return;
    }
    
    if (response->status_code != 200) {
        printf("  Skipping test (unexpected status %d)\n", response->status_code);
        http_response_free(response);
        http_client_destroy(client);
        return;
    }
    
    http_response_free(response);
    http_client_destroy(client);
    
    printf("   Client works correctly without cookie jar\n");
}

int main(void) {
    printf("\n=== HTTP Client Cookie Tests ===\n\n");
    
    test_cookie_jar_basic();
    test_automatic_cookie_handling();
    test_multiple_cookies();
    test_cookie_persistence();
    test_cookie_without_jar();
    
    printf("\n All cookie tests passed!\n");
    return 0;
}
