#include "unity.h"
#include "error_recovery.h"
#include <string.h>

void setUp(void) {
    iris_error_recovery_init();
}

void tearDown(void) {
    iris_error_recovery_cleanup();
}

void test_error_recovery_init(void) {
    int result = iris_error_recovery_init();
    TEST_ASSERT_EQUAL(0, result);
    iris_error_recovery_cleanup();
}

void test_error_context_creation(void) {
    iris_error_context_t ctx = iris_create_error_context(
        IRIS_ERROR_OUT_OF_MEMORY, -1, "Test error message", 
        __FILE__, __LINE__, __func__);
    
    TEST_ASSERT_EQUAL(IRIS_ERROR_OUT_OF_MEMORY, ctx.error_type);
    TEST_ASSERT_EQUAL(-1, ctx.error_code);
    TEST_ASSERT_EQUAL_STRING("Test error message", ctx.message);
    TEST_ASSERT_NOT_NULL(ctx.file);
    TEST_ASSERT_GREATER_THAN(0, ctx.line);
    TEST_ASSERT_NOT_NULL(ctx.function);
}

void test_error_type_string(void) {
    TEST_ASSERT_EQUAL_STRING("Out of Memory", iris_error_type_string(IRIS_ERROR_OUT_OF_MEMORY));
    TEST_ASSERT_EQUAL_STRING("Bind Failed", iris_error_type_string(IRIS_ERROR_BIND_FAILED));
    TEST_ASSERT_EQUAL_STRING("Listen Failed", iris_error_type_string(IRIS_ERROR_LISTEN_FAILED));
    TEST_ASSERT_EQUAL_STRING("Server Initialization Failed", iris_error_type_string(IRIS_ERROR_SERVER_INIT_FAILED));
}

void test_error_handling_memory_error(void) {
    iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_OUT_OF_MEMORY, -1, "Memory allocation failed");
    iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
    
    /* Memory errors should trigger graceful shutdown */
    TEST_ASSERT_EQUAL(IRIS_RECOVERY_GRACEFUL_SHUTDOWN, action);
}

void test_error_handling_network_error(void) {
    iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_CONNECTION_FAILED, -1, "Connection failed");
    iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_CONTINUE);
    
    /* Network errors should allow continuation */
    TEST_ASSERT_EQUAL(IRIS_RECOVERY_CONTINUE, action);
}

void test_error_handling_security_error(void) {
    iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_INVALID_INPUT, -1, "Invalid input detected");
    iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_REJECT_REQUEST);
    
    /* Security errors should reject the request */
    TEST_ASSERT_EQUAL(IRIS_RECOVERY_REJECT_REQUEST, action);
}

/* Custom error handler for testing */
static iris_recovery_action_t test_error_handler(const iris_error_context_t *ctx) {
    (void)ctx; /* Unused parameter */
    return IRIS_RECOVERY_FALLBACK;
}

void test_custom_error_handler(void) {
    iris_set_error_handler(test_error_handler);
    
    iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_OUT_OF_MEMORY, -1, "Test error");
    iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
    
    /* Custom handler should override default behavior */
    TEST_ASSERT_EQUAL(IRIS_RECOVERY_FALLBACK, action);
    
    /* Reset to default handler */
    iris_set_error_handler(NULL);
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_error_recovery_init);
    RUN_TEST(test_error_context_creation);
    RUN_TEST(test_error_type_string);
    RUN_TEST(test_error_handling_memory_error);
    RUN_TEST(test_error_handling_network_error);
    RUN_TEST(test_error_handling_security_error);
    RUN_TEST(test_custom_error_handler);
    
    return UNITY_END();
}