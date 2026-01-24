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

    /* Test DotEnv */
    {
        printf("Testing DotEnv parsing...\n");
        const char *env_file = ".env.turbo_test";
        FILE *f = fopen(env_file, "w");
        if (f) {
            fprintf(f, "TURBO_PARSER_TEST=success\n");
            fclose(f);
        }

        int rc = turbo_dotenv_load(env_file, true);
        assert(rc == 0);

        char *val = getenv("TURBO_PARSER_TEST");
        assert(val != NULL);
        assert(strcmp(val, "success") == 0);

        remove(env_file);
        printf("DotEnv parsing test passed!\n");
    }

    /* Test JSON creation/modification */
    {
        printf("Testing JSON building...\n");
        json_value_t *root = turbo_json_create_object();
        assert(root != NULL);

        turbo_json_object_set_string(root, "name", "turbo");
        turbo_json_object_set_number(root, "version", 2.0);
        turbo_json_object_set_bool(root, "active", true);
        
        json_value_t *arr = turbo_json_create_array();
        turbo_json_array_add(arr, turbo_json_create_number(1));
        turbo_json_array_add(arr, turbo_json_create_number(2));
        turbo_json_object_add(root, "items", arr);

        size_t len = 0;
        char *serialized = turbo_json_serialize(root, &len);
        assert(serialized != NULL);
        assert(len > 0);
        
        // Simple string check
        assert(strstr(serialized, "\"name\":\"turbo\"") != NULL);
        assert(strstr(serialized, "\"version\":2") != NULL);
        assert(strstr(serialized, "\"items\":[1,2]") != NULL);

        turbo_json_serialize_free(serialized);
        turbo_free_json(&root);
        assert(root == NULL);
        printf("JSON building test passed!\n");
    }

    printf("Turbo Parser dedicated interfaces test finished.\n");
    return 0;
}