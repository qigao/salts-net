#include "turbo_parser.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main() {
    printf("Testing Turbo Parser Dedicated Interfaces...\n");

    /* Test JSON parsing */
    {
        const char* json_data = "{\"key\": \"value\", \"number\": 123}";
        void* result = NULL;
        int rc = turbo_parse_json((const uint8_t*)json_data, strlen(json_data), &result);
        if (rc == 0) {
            printf("JSON parsed successfully!\n");
            assert(result != NULL);
            turbo_free_json(&result);
            assert(result == NULL);
        } else {
            printf("JSON parsing failed with rc=%d\n", rc);
        }
    }

    /* Test INI parsing */
    {
        const char* ini_data = "[section]\nkey=value\n";
        void* result = NULL;
        int rc = turbo_parse_ini((const uint8_t*)ini_data, strlen(ini_data), &result);
        if (rc == 0) {
            printf("INI parsed successfully!\n");
            assert(result != NULL);
            turbo_free_ini(&result);
            assert(result == NULL);
        } else {
            printf("INI parsing failed with rc=%d\n", rc);
        }
    }

    /* Test URI parsing */
    {
        const char* uri_data = "https://example.com:8080/path?query#frag";
        void* result = NULL;
        int rc = turbo_parse_uri((const uint8_t*)uri_data, strlen(uri_data), &result);
        if (rc == 0) {
            printf("URI parsed successfully!\n");
            assert(result != NULL);
            turbo_free_uri(&result);
            assert(result == NULL);
        } else {
            printf("URI parsing failed with rc=%d\n", rc);
        }
    }

    printf("Turbo Parser dedicated interfaces test finished.\n");
    return 0;
}