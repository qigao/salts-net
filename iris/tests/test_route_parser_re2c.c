/**
 * @file test_route_parser_re2c.c
 * @brief Tests for re2c-based route parser
 */

#include "unity.h"
#include "route_trie.h"
#include "arena_buffer.h"
#include <string.h>

static turbo_arena_t test_arena;

void setUp(void) {
    int ret = turbo_arena_init(&test_arena, 4096);
    TEST_ASSERT_EQUAL(0, ret);
}

void tearDown(void) {
    turbo_arena_free(&test_arena);
}

/* HTTP Method Parsing Tests */

void test_parse_http_method_get(void) {
    TEST_ASSERT_EQUAL(METHOD_GET, parse_http_method_re2c("GET", 3));
    TEST_ASSERT_EQUAL(METHOD_GET, parse_http_method_re2c("get", 3));
    TEST_ASSERT_EQUAL(METHOD_GET, parse_http_method_re2c("Get", 3));
}

void test_parse_http_method_post(void) {
    TEST_ASSERT_EQUAL(METHOD_POST, parse_http_method_re2c("POST", 4));
    TEST_ASSERT_EQUAL(METHOD_POST, parse_http_method_re2c("post", 4));
}

void test_parse_http_method_put(void) {
    TEST_ASSERT_EQUAL(METHOD_PUT, parse_http_method_re2c("PUT", 3));
    TEST_ASSERT_EQUAL(METHOD_PUT, parse_http_method_re2c("put", 3));
}

void test_parse_http_method_delete(void) {
    TEST_ASSERT_EQUAL(METHOD_DELETE, parse_http_method_re2c("DELETE", 6));
    TEST_ASSERT_EQUAL(METHOD_DELETE, parse_http_method_re2c("delete", 6));
}

void test_parse_http_method_patch(void) {
    TEST_ASSERT_EQUAL(METHOD_PATCH, parse_http_method_re2c("PATCH", 5));
}

void test_parse_http_method_head(void) {
    TEST_ASSERT_EQUAL(METHOD_HEAD, parse_http_method_re2c("HEAD", 4));
}

void test_parse_http_method_options(void) {
    TEST_ASSERT_EQUAL(METHOD_OPTIONS, parse_http_method_re2c("OPTIONS", 7));
}

void test_parse_http_method_unknown(void) {
    TEST_ASSERT_EQUAL(METHOD_UNKNOWN, parse_http_method_re2c("INVALID", 7));
    TEST_ASSERT_EQUAL(METHOD_UNKNOWN, parse_http_method_re2c("GETS", 4));
    TEST_ASSERT_EQUAL(METHOD_UNKNOWN, parse_http_method_re2c("", 0));
    TEST_ASSERT_EQUAL(METHOD_UNKNOWN, parse_http_method_re2c(NULL, 0));
}

/* Path Segment Counting Tests */

void test_count_segments_simple(void) {
    TEST_ASSERT_EQUAL(3, count_path_segments_re2c("/users/123/posts", 16));
    TEST_ASSERT_EQUAL(2, count_path_segments_re2c("/api/v1", 7));
    TEST_ASSERT_EQUAL(1, count_path_segments_re2c("/users", 6));
}

void test_count_segments_root(void) {
    TEST_ASSERT_EQUAL(0, count_path_segments_re2c("/", 1));
    TEST_ASSERT_EQUAL(0, count_path_segments_re2c("", 0));
}

void test_count_segments_trailing_slash(void) {
    TEST_ASSERT_EQUAL(1, count_path_segments_re2c("/users/", 7));
    TEST_ASSERT_EQUAL(3, count_path_segments_re2c("/a/b/c/", 7));
}

void test_count_segments_double_slash(void) {
    TEST_ASSERT_EQUAL(2, count_path_segments_re2c("/users//posts", 13));
}

/* Path Tokenization Tests */

void test_tokenize_simple_path(void) {
    tokenized_path_t result;
    int ret = tokenize_path_re2c(&test_arena, "/users/123/posts", &result);
    
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(3, result.count);
    
    TEST_ASSERT_EQUAL(5, result.segments[0].len);
    TEST_ASSERT_EQUAL_STRING_LEN("users", result.segments[0].start, 5);
    TEST_ASSERT_FALSE(result.segments[0].is_param);
    TEST_ASSERT_FALSE(result.segments[0].is_wildcard);
    
    TEST_ASSERT_EQUAL(3, result.segments[1].len);
    TEST_ASSERT_EQUAL_STRING_LEN("123", result.segments[1].start, 3);
    
    TEST_ASSERT_EQUAL(5, result.segments[2].len);
    TEST_ASSERT_EQUAL_STRING_LEN("posts", result.segments[2].start, 5);
}

void test_tokenize_with_params(void) {
    tokenized_path_t result;
    int ret = tokenize_path_re2c(&test_arena, "/users/:id/posts/:postId", &result);
    
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(4, result.count);
    
    TEST_ASSERT_FALSE(result.segments[0].is_param);
    TEST_ASSERT_TRUE(result.segments[1].is_param);
    TEST_ASSERT_FALSE(result.segments[2].is_param);
    TEST_ASSERT_TRUE(result.segments[3].is_param);
}

void test_tokenize_with_wildcard(void) {
    tokenized_path_t result;
    int ret = tokenize_path_re2c(&test_arena, "/static/*filepath", &result);
    
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(2, result.count);
    
    TEST_ASSERT_FALSE(result.segments[0].is_wildcard);
    TEST_ASSERT_TRUE(result.segments[1].is_wildcard);
}

void test_tokenize_root_path(void) {
    tokenized_path_t result;
    int ret = tokenize_path_re2c(&test_arena, "/", &result);
    
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(0, result.count);
}

void test_tokenize_empty_path(void) {
    tokenized_path_t result;
    int ret = tokenize_path_re2c(&test_arena, "", &result);
    
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(0, result.count);
}

void test_tokenize_null_path(void) {
    tokenized_path_t result;
    int ret = tokenize_path_re2c(&test_arena, NULL, &result);
    
    TEST_ASSERT_EQUAL(-1, ret);
}

/* Segment Extraction Tests */

void test_extract_segment_simple(void) {
    const char *path = "/users/posts";
    const char *cursor = path;
    const char *limit = path + strlen(path);
    path_segment_t seg;
    
    TEST_ASSERT_TRUE(extract_path_segment_re2c(&cursor, limit, &seg));
    TEST_ASSERT_EQUAL(5, seg.len);
    TEST_ASSERT_EQUAL_STRING_LEN("users", seg.start, 5);
    
    TEST_ASSERT_TRUE(extract_path_segment_re2c(&cursor, limit, &seg));
    TEST_ASSERT_EQUAL(5, seg.len);
    TEST_ASSERT_EQUAL_STRING_LEN("posts", seg.start, 5);
    
    TEST_ASSERT_FALSE(extract_path_segment_re2c(&cursor, limit, &seg));
}

void test_extract_segment_param(void) {
    const char *path = "/:id";
    const char *cursor = path;
    const char *limit = path + strlen(path);
    path_segment_t seg;
    
    TEST_ASSERT_TRUE(extract_path_segment_re2c(&cursor, limit, &seg));
    TEST_ASSERT_TRUE(seg.is_param);
    TEST_ASSERT_EQUAL(3, seg.len);
}

/* Integration with get_method_index */

void test_get_method_index_integration(void) {
    TEST_ASSERT_EQUAL(METHOD_GET, get_method_index("GET"));
    TEST_ASSERT_EQUAL(METHOD_POST, get_method_index("POST"));
    TEST_ASSERT_EQUAL(METHOD_PUT, get_method_index("PUT"));
    TEST_ASSERT_EQUAL(METHOD_DELETE, get_method_index("DELETE"));
    TEST_ASSERT_EQUAL(METHOD_UNKNOWN, get_method_index("INVALID"));
    TEST_ASSERT_EQUAL(METHOD_UNKNOWN, get_method_index(NULL));
}

/* Integration with tokenize_path */

void test_tokenize_path_integration(void) {
    tokenized_path_t result;
    int ret = tokenize_path(&test_arena, "/api/v1/users", &result);
    
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(3, result.count);
}

int main(void) {
    UNITY_BEGIN();
    
    /* HTTP Method Tests */
    RUN_TEST(test_parse_http_method_get);
    RUN_TEST(test_parse_http_method_post);
    RUN_TEST(test_parse_http_method_put);
    RUN_TEST(test_parse_http_method_delete);
    RUN_TEST(test_parse_http_method_patch);
    RUN_TEST(test_parse_http_method_head);
    RUN_TEST(test_parse_http_method_options);
    RUN_TEST(test_parse_http_method_unknown);
    
    /* Segment Counting Tests */
    RUN_TEST(test_count_segments_simple);
    RUN_TEST(test_count_segments_root);
    RUN_TEST(test_count_segments_trailing_slash);
    RUN_TEST(test_count_segments_double_slash);
    
    /* Tokenization Tests */
    RUN_TEST(test_tokenize_simple_path);
    RUN_TEST(test_tokenize_with_params);
    RUN_TEST(test_tokenize_with_wildcard);
    RUN_TEST(test_tokenize_root_path);
    RUN_TEST(test_tokenize_empty_path);
    RUN_TEST(test_tokenize_null_path);
    
    /* Segment Extraction Tests */
    RUN_TEST(test_extract_segment_simple);
    RUN_TEST(test_extract_segment_param);
    
    /* Integration Tests */
    RUN_TEST(test_get_method_index_integration);
    RUN_TEST(test_tokenize_path_integration);
    
    return UNITY_END();
}
