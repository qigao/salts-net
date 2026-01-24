#include <unity.h>
#include <dotenv.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define setenv(name,val,overwrite) _putenv_s(name, val)
#endif

void setUp(void) {
    // Clean up environment variables used in tests
    #ifdef _WIN32
    _putenv_s("TEST_KEY", "");
    _putenv_s("NESTED_KEY", "");
    _putenv_s("OVERWRITE_KEY", "");
    #else
    unsetenv("TEST_KEY");
    unsetenv("NESTED_KEY");
    unsetenv("OVERWRITE_KEY");
    #endif
}

void tearDown(void) {}

void test_dotenv_load_simple(void) {
    const char *env_content = "TEST_KEY=test_value\n";
    FILE *f = fopen(".env.test.simple", "w");
    fputs(env_content, f);
    fclose(f);

    int res = dotenv_load(".env.test.simple", true);
    TEST_ASSERT_EQUAL(0, res);

    const char *val = getenv("TEST_KEY");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("test_value", val);

    remove(".env.test.simple");
}

void test_dotenv_load_nested(void) {
    setenv("EXISTING_VAR", "base", 1);
    const char *env_content = "NESTED_KEY=${EXISTING_VAR}/extra\n";
    FILE *f = fopen(".env.test.nested", "w");
    fputs(env_content, f);
    fclose(f);

    int res = dotenv_load(".env.test.nested", true);
    TEST_ASSERT_EQUAL(0, res);

    const char *val = getenv("NESTED_KEY");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING("base/extra", val);

    remove(".env.test.nested");
}

void test_dotenv_overwrite(void) {
    setenv("OVERWRITE_KEY", "original", 1);
    const char *env_content = "OVERWRITE_KEY=new_value\n";
    FILE *f = fopen(".env.test.overwrite", "w");
    fputs(env_content, f);
    fclose(f);

    // Test without overwrite
    dotenv_load(".env.test.overwrite", false);
    TEST_ASSERT_EQUAL_STRING("original", getenv("OVERWRITE_KEY"));

    // Test with overwrite
    dotenv_load(".env.test.overwrite", true);
    TEST_ASSERT_EQUAL_STRING("new_value", getenv("OVERWRITE_KEY"));

    remove(".env.test.overwrite");
}

void test_dotenv_missing_file(void) {
    int res = dotenv_load("non_existent_file", true);
    TEST_ASSERT_LESS_THAN_INT(0, res);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_dotenv_load_simple);
    RUN_TEST(test_dotenv_load_nested);
    RUN_TEST(test_dotenv_overwrite);
    RUN_TEST(test_dotenv_missing_file);
    return UNITY_END();
}
