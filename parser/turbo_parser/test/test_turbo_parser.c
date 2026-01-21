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

    /* Test CMD Parsing */
    {
        printf("Testing CMD parsing...\n");
        turbo_cmd_parser_t *parser = turbo_cmd_create("test_app", "1.0");
        assert(parser != NULL);

        bool verbose = false;
        char *output = NULL;
        int64_t count = 0;

        turbo_cmd_add_flag(parser, &verbose, "verbose", "v", "Enable verbose output");
        turbo_cmd_add_string(parser, &output, "output", "o", "Output file");
        turbo_cmd_add_integer(parser, &count, "count", "c", "Count items");

        char *arg0 = strdup("test_app");
        char *arg1 = strdup("--verbose");
        char *arg2 = strdup("-o");
        char *arg3 = strdup("file.txt");
        char *arg4 = strdup("--count=10");

        char *argv[] = {arg0, arg1, arg2, arg3, arg4};
        int argc = 5;

        turbo_cmd_parse(parser, argc, argv, false);
        
        assert(verbose == 1);
        assert(output != NULL);
        assert(strcmp(output, "file.txt") == 0);
        assert(count == 10);

        free(arg0);
        free(arg1);
        free(arg2);
        free(arg3);
        free(arg4);

        turbo_cmd_destroy(parser);
        printf("CMD parsing test passed!\n");
    }

    printf("Turbo Parser dedicated interfaces test finished.\n");
    return 0;
}