#include "ini_parser.h"
#include "unity.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_parse_null(void) {
    ini_t* ini = ini_parse(NULL, 0);
    TEST_ASSERT_NULL(ini);
}

void test_parse_empty(void) {
    ini_t* ini = ini_parse("", 0);
    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL(0, ini_section_count(ini));
    ini_free(ini);
}

void test_parse_single_section(void) {
    const char* content = "[section1]\nkey1 = value1\n";
    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("value1", ini_get(ini, "section1", "key1"));
    TEST_ASSERT_NULL(ini_get(ini, "section1", "key2"));
    TEST_ASSERT_NULL(ini_get(ini, "section2", "key1"));

    ini_free(ini);
}

void test_parse_multiple_sections(void) {
    const char* content =
        "[section1]\n"
        "key1 = value1\n"
        "key2 = value2\n"
        "\n"
        "[section2]\n"
        "key3 = value3\n"
        "key4 = value4\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("value1", ini_get(ini, "section1", "key1"));
    TEST_ASSERT_EQUAL_STRING("value2", ini_get(ini, "section1", "key2"));
    TEST_ASSERT_EQUAL_STRING("value3", ini_get(ini, "section2", "key3"));
    TEST_ASSERT_EQUAL_STRING("value4", ini_get(ini, "section2", "key4"));
    TEST_ASSERT_NULL(ini_get(ini, "section1", "key3"));

    ini_free(ini);
}

void test_parse_with_comments(void) {
    const char* content =
        "; This is a comment\n"
        "[section1]\n"
        "key1 = value1 ; inline comment\n"
        "# Another comment\n"
        "key2 = value2\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("value1", ini_get(ini, "section1", "key1"));
    TEST_ASSERT_EQUAL_STRING("value2", ini_get(ini, "section1", "key2"));

    ini_free(ini);
}

void test_parse_whitespace(void) {
    const char* content =
        "  [section1]  \n"
        "  key1  =  value1  \n"
        "key2=value2\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("value1", ini_get(ini, "section1", "key1"));
    TEST_ASSERT_EQUAL_STRING("value2", ini_get(ini, "section1", "key2"));

    ini_free(ini);
}

void test_parse_duplicate_keys(void) {
    const char* content =
        "[section1]\n"
        "key1 = value1\n"
        "key1 = value2\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("value2", ini_get(ini, "section1", "key1"));

    ini_free(ini);
}

void test_parse_duplicate_sections(void) {
    const char* content =
        "[section1]\n"
        "key1 = value1\n"
        "[section2]\n"
        "key2 = value2\n"
        "[section1]\n"
        "key3 = value3\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("value1", ini_get(ini, "section1", "key1"));
    TEST_ASSERT_EQUAL_STRING("value3", ini_get(ini, "section1", "key3"));
    TEST_ASSERT_EQUAL_STRING("value2", ini_get(ini, "section2", "key2"));

    ini_free(ini);
}

void test_parse_global_keys(void) {
    const char* content =
        "global_key = global_value\n"
        "[section1]\n"
        "key1 = value1\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("global_value", ini_get(ini, "", "global_key"));
    TEST_ASSERT_EQUAL_STRING("global_value", ini_get(ini, NULL, "global_key"));
    TEST_ASSERT_EQUAL_STRING("value1", ini_get(ini, "section1", "key1"));

    ini_free(ini);
}

void test_get_int(void) {
    const char* content =
        "[section1]\n"
        "port = 8080\n"
        "hex = 0xFF\n"
        "invalid = abc\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_INT(8080, ini_get_int(ini, "section1", "port", 0));
    TEST_ASSERT_EQUAL_INT(255, ini_get_int(ini, "section1", "hex", 0));
    TEST_ASSERT_EQUAL_INT(42, ini_get_int(ini, "section1", "invalid", 42));
    TEST_ASSERT_EQUAL_INT(99, ini_get_int(ini, "section1", "missing", 99));

    ini_free(ini);
}

void test_get_bool(void) {
    const char* content =
        "[section1]\n"
        "enabled1 = true\n"
        "enabled2 = yes\n"
        "enabled3 = on\n"
        "enabled4 = 1\n"
        "disabled1 = false\n"
        "disabled2 = no\n"
        "disabled3 = off\n"
        "disabled4 = 0\n"
        "invalid = abc\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_TRUE(ini_get_bool(ini, "section1", "enabled1", false));
    TEST_ASSERT_TRUE(ini_get_bool(ini, "section1", "enabled2", false));
    TEST_ASSERT_TRUE(ini_get_bool(ini, "section1", "enabled3", false));
    TEST_ASSERT_TRUE(ini_get_bool(ini, "section1", "enabled4", false));
    TEST_ASSERT_FALSE(ini_get_bool(ini, "section1", "disabled1", true));
    TEST_ASSERT_FALSE(ini_get_bool(ini, "section1", "disabled2", true));
    TEST_ASSERT_FALSE(ini_get_bool(ini, "section1", "disabled3", true));
    TEST_ASSERT_FALSE(ini_get_bool(ini, "section1", "disabled4", true));
    TEST_ASSERT_TRUE(ini_get_bool(ini, "section1", "invalid", true));
    TEST_ASSERT_FALSE(ini_get_bool(ini, "section1", "missing", false));

    ini_free(ini);
}

void test_get_double(void) {
    const char* content =
        "[section1]\n"
        "pi = 3.14159\n"
        "negative = -1.5\n"
        "invalid = abc\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 3.14159, ini_get_double(ini, "section1", "pi", 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, -1.5, ini_get_double(ini, "section1", "negative", 0.0));
    TEST_ASSERT_DOUBLE_WITHIN(0.0001, 1.0, ini_get_double(ini, "section1", "invalid", 1.0));

    ini_free(ini);
}

void test_section_iteration(void) {
    const char* content =
        "[section1]\n"
        "key1 = value1\n"
        "[section2]\n"
        "key2 = value2\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL(2, ini_section_count(ini));

    ini_free(ini);
}

void test_key_iteration(void) {
    const char* content =
        "[section1]\n"
        "key1 = value1\n"
        "key2 = value2\n"
        "key3 = value3\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL(3, ini_key_count(ini, "section1"));
    TEST_ASSERT_NOT_NULL(ini_key_name(ini, "section1", 0));
    TEST_ASSERT_NOT_NULL(ini_key_name(ini, "section1", 1));
    TEST_ASSERT_NOT_NULL(ini_key_name(ini, "section1", 2));
    TEST_ASSERT_NULL(ini_key_name(ini, "section1", 3));

    ini_free(ini);
}

void test_value_with_spaces(void) {
    const char* content =
        "[section1]\n"
        "key1 = value with spaces\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("value with spaces", ini_get(ini, "section1", "key1"));

    ini_free(ini);
}

void test_crlf_line_endings(void) {
    const char* content =
        "[section1]\r\n"
        "key1 = value1\r\n"
        "key2 = value2\r\n";

    ini_t* ini = ini_parse(content, strlen(content));

    TEST_ASSERT_NOT_NULL(ini);
    TEST_ASSERT_EQUAL_STRING("value1", ini_get(ini, "section1", "key1"));
    TEST_ASSERT_EQUAL_STRING("value2", ini_get(ini, "section1", "key2"));

    ini_free(ini);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_parse_null);
    RUN_TEST(test_parse_empty);
    RUN_TEST(test_parse_single_section);
    RUN_TEST(test_parse_multiple_sections);
    RUN_TEST(test_parse_with_comments);
    RUN_TEST(test_parse_whitespace);
    RUN_TEST(test_parse_duplicate_keys);
    RUN_TEST(test_parse_duplicate_sections);
    RUN_TEST(test_parse_global_keys);
    RUN_TEST(test_get_int);
    RUN_TEST(test_get_bool);
    RUN_TEST(test_get_double);
    RUN_TEST(test_section_iteration);
    RUN_TEST(test_key_iteration);
    RUN_TEST(test_value_with_spaces);
    RUN_TEST(test_crlf_line_endings);

    return UNITY_END();
}
