#include "turbo_parser.h"
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {
    // No setup required
}

void tearDown(void) {
    // No teardown required
}

void test_json_parsing(void) {
    printf("Testing JSON parsing...\n");
    const char* json_data = "{\"key\": \"value\", \"number\": 123}";
    void* result = NULL;
    int rc = turbo_parse_json((const uint8_t*)json_data, strlen(json_data), &result);
    
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_INT(TURBO_JSON_OBJECT, turbo_json_type(result));
    
    turbo_free_json(&result);
    TEST_ASSERT_NULL(result);
}

void test_ini_parsing(void) {
    printf("Testing INI parsing...\n");
    const char* ini_data = "[section]\nkey=value\n";
    void* result = NULL;
    int rc = turbo_parse_ini((const uint8_t*)ini_data, strlen(ini_data), &result);
    
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(result);
    
    turbo_free_ini(&result);
    TEST_ASSERT_NULL(result);
}

void test_uri_parsing(void) {
    printf("Testing URI parsing...\n");
    const char* uri_data = "https://example.com:8080/path?query#frag";
    void* result = NULL;
    int rc = turbo_parse_uri((const uint8_t*)uri_data, strlen(uri_data), &result);
    
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(result);
    
    turbo_free_uri(&result);
    TEST_ASSERT_NULL(result);
}

void test_cmd_parsing(void) {
    printf("Testing CMD parsing...\n");
    turbo_cmd_parser_t *parser = turbo_cmd_create("test_app", "1.0");
    TEST_ASSERT_NOT_NULL(parser);

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
    
    TEST_ASSERT_TRUE(verbose);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_EQUAL_STRING("file.txt", output);
    TEST_ASSERT_EQUAL_INT64(10, count);

    free(arg0);
    free(arg1);
    free(arg2);
    free(arg3);
    free(arg4);

    turbo_cmd_destroy(parser);
}

void test_toon_parsing(void) {
    printf("Testing TOON parsing...\n");
    const char* toon_data = 
        "server:\n"
        "  host: \"localhost\"\n"
        "  port: 1883\n"
        "  enabled: true\n"
        "  timeout: 5.5\n"
        "topics: [\"a\", \"b\", \"c\"]\n";
    
    turbo_toon_node_t* root = NULL;
    int rc = turbo_parse_toon((const uint8_t*)toon_data, strlen(toon_data), &root);
    
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(TURBO_TOON_OBJECT, turbo_toon_type(root));

    turbo_toon_node_t* host = turbo_toon_get(root, "server.host");
    TEST_ASSERT_NOT_NULL(host);
    TEST_ASSERT_EQUAL_INT(TURBO_TOON_STRING, turbo_toon_type(host));
    TEST_ASSERT_EQUAL_STRING("localhost", turbo_toon_string(host));

    turbo_toon_node_t* port = turbo_toon_get(root, "server.port");
    TEST_ASSERT_NOT_NULL(port);
    TEST_ASSERT_EQUAL_INT(1883, turbo_toon_int(port));

    turbo_toon_node_t* enabled = turbo_toon_get(root, "server.enabled");
    TEST_ASSERT_NOT_NULL(enabled);
    TEST_ASSERT_TRUE(turbo_toon_bool(enabled));

    turbo_toon_node_t* timeout = turbo_toon_get(root, "server.timeout");
    TEST_ASSERT_NOT_NULL(timeout);
    TEST_ASSERT_EQUAL_DOUBLE(5.5, turbo_toon_number(timeout));

    turbo_toon_node_t* topics = turbo_toon_get(root, "topics");
    TEST_ASSERT_NOT_NULL(topics);
    TEST_ASSERT_EQUAL_INT(TURBO_TOON_LIST, turbo_toon_type(topics));
    TEST_ASSERT_EQUAL_UINT(3, turbo_toon_array_size(topics));

    turbo_toon_node_t* topic0 = turbo_toon_array_get(topics, 0);
    TEST_ASSERT_NOT_NULL(topic0);
    TEST_ASSERT_EQUAL_STRING("a", turbo_toon_string(topic0));

    // Test serialization
    size_t serialized_len = 0;
    char* serialized = turbo_toon_serialize(root, &serialized_len);
    TEST_ASSERT_NOT_NULL(serialized);
    TEST_ASSERT_GREATER_THAN(0, serialized_len);
    TEST_ASSERT_NOT_NULL(strstr(serialized, "host: \"localhost\""));
    TEST_ASSERT_NOT_NULL(strstr(serialized, "port: 1883"));
    TEST_ASSERT_NOT_NULL(strstr(serialized, "topics: [\"a\", \"b\", \"c\"]"));

    turbo_toon_serialize_free(serialized);

    // Test JSON serialization
    char* json_out = turbo_toon_serialize_json(root, NULL);
    TEST_ASSERT_NOT_NULL(json_out);
    TEST_ASSERT_NOT_NULL(strstr(json_out, "\"host\": \"localhost\""));
    turbo_toon_serialize_json_free(json_out);

    turbo_free_toon(&root);
    TEST_ASSERT_NULL(root);
}

void test_toon_from_json(void) {
    printf("Testing TOON from JSON...\n");
    const char* json_in = "{\"name\": \"test\", \"value\": 123}";
    turbo_toon_node_t* toon = turbo_toon_from_json(json_in, strlen(json_in));
    
    TEST_ASSERT_NOT_NULL(toon);
    TEST_ASSERT_EQUAL_INT(TURBO_TOON_OBJECT, turbo_toon_type(toon));
    TEST_ASSERT_EQUAL_STRING("test", turbo_toon_string(turbo_toon_get(toon, "name")));
    TEST_ASSERT_EQUAL_INT(123, turbo_toon_int(turbo_toon_get(toon, "value")));
    
    turbo_free_toon(&toon);
    TEST_ASSERT_NULL(toon);
}

void test_dotenv_parsing(void) {
    printf("Testing DotEnv parsing...\n");
    const char *env_file = ".env.turbo_test";
    FILE *f = fopen(env_file, "w");
    if (f) {
        fprintf(f, "TURBO_PARSER_TEST=success\n");
        fclose(f);
    }

    int rc = turbo_dotenv_load(env_file, true);
    TEST_ASSERT_EQUAL_INT(0, rc);

    char *val = getenv("TURBO_PARSER_TEST");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("success", val);

    remove(env_file);
}

void test_json_building(void) {
    printf("Testing JSON building...\n");
    json_value_t *root = turbo_json_create_object();
    TEST_ASSERT_NOT_NULL(root);

    turbo_json_object_set_string(root, "name", "turbo");
    turbo_json_object_set_number(root, "version", 2.0);
    turbo_json_object_set_bool(root, "active", true);
    
    json_value_t *arr = turbo_json_create_array();
    turbo_json_array_add(arr, turbo_json_create_number(1));
    turbo_json_array_add(arr, turbo_json_create_number(2));
    turbo_json_object_add(root, "items", arr);

    size_t len = 0;
    char *serialized = turbo_json_serialize(root, &len);
    TEST_ASSERT_NOT_NULL(serialized);
    TEST_ASSERT_GREATER_THAN(0, len);
    
    // Simple string check
    TEST_ASSERT_NOT_NULL(strstr(serialized, "\"name\":\"turbo\""));
    TEST_ASSERT_NOT_NULL(strstr(serialized, "\"version\":2"));
    TEST_ASSERT_NOT_NULL(strstr(serialized, "\"items\":[1,2]"));

    turbo_json_serialize_free(serialized);
    turbo_free_json(&root);
    TEST_ASSERT_NULL(root);
}

void test_toml_parsing(void) {
    printf("Testing TOML parsing...\n");
    const char* toml_data = 
        "[server]\n"
        "host = \"localhost\"\n"
        "port = 8080\n"
        "enabled = true\n"
        "timeout = 5.0\n"
        "started = 1979-05-27T07:32:00Z\n"
        "\n"
        "[[databases]]\n"
        "name = \"db1\"\n"
        "\n"
        "[[databases]]\n"
        "name = \"db2\"\n";
    
    turbo_toml_t* root = NULL;
    int rc = turbo_parse_toml((const uint8_t*)toml_data, strlen(toml_data), &root);
    
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_NOT_NULL(root);
    
    // Test table access
    turbo_toml_t* server = turbo_toml_table(root, "server");
    TEST_ASSERT_NOT_NULL(server);
    
    // Test string
    turbo_toml_value_t host = turbo_toml_string(server, "host");
    TEST_ASSERT_TRUE(host.ok);
    TEST_ASSERT_EQUAL_STRING("localhost", host.u.s);
    free(host.u.s);
    
    // Test int
    turbo_toml_value_t port = turbo_toml_int(server, "port");
    TEST_ASSERT_TRUE(port.ok);
    TEST_ASSERT_EQUAL_INT(8080, port.u.i);
    
    // Test bool
    turbo_toml_value_t enabled = turbo_toml_bool(server, "enabled");
    TEST_ASSERT_TRUE(enabled.ok);
    TEST_ASSERT_TRUE(enabled.u.b);
    
    // Test double
    turbo_toml_value_t timeout = turbo_toml_double(server, "timeout");
    TEST_ASSERT_TRUE(timeout.ok);
    TEST_ASSERT_EQUAL_DOUBLE(5.0, timeout.u.d);
    
    // Test timestamp
    turbo_toml_value_t started = turbo_toml_timestamp(server, "started");
    TEST_ASSERT_TRUE(started.ok);
    TEST_ASSERT_EQUAL_INT('d', started.u.ts.kind);
    // TEST_ASSERT_EQUAL_INT(1979, started.u.ts.year); // Unity might not have equal int for struct members directly without extraction? No, access is direct.
    
    // Test array of tables
    turbo_toml_array_t* dbs = turbo_toml_array(root, "databases");
    TEST_ASSERT_NOT_NULL(dbs);
    TEST_ASSERT_EQUAL_INT(2, turbo_toml_array_len(dbs));
    
    turbo_toml_t* db1 = turbo_toml_array_table(dbs, 0);
    TEST_ASSERT_NOT_NULL(db1);
    turbo_toml_value_t name1 = turbo_toml_string(db1, "name");
    TEST_ASSERT_TRUE(name1.ok);
    TEST_ASSERT_EQUAL_STRING("db1", name1.u.s);
    free(name1.u.s);
    
    turbo_free_toml(&root);
    TEST_ASSERT_NULL(root);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_json_parsing);
    RUN_TEST(test_ini_parsing);
    RUN_TEST(test_uri_parsing);
    RUN_TEST(test_cmd_parsing);
    RUN_TEST(test_toon_parsing);
    RUN_TEST(test_toon_from_json);
    RUN_TEST(test_dotenv_parsing);
    RUN_TEST(test_json_building);
    RUN_TEST(test_toml_parsing);
    return UNITY_END();
}