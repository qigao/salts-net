/**
 * test_route_trie.c - Unit tests for iris route trie
 *
 * Tests trie-based routing with exact matches, parameters, and wildcards.
 */

#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "route_trie.h"
#include "arena_buffer.h"

static route_trie_t *trie;
static turbo_arena_t arena;

/* Dummy handler for testing */
static void dummy_handler(Req *req, Res *res) {
    (void)req;
    (void)res;
}

static void handler_a(Req *req, Res *res) {
    (void)req;
    (void)res;
}

static void handler_b(Req *req, Res *res) {
    (void)req;
    (void)res;
}

void setUp(void) {
    trie = route_trie_create();
    turbo_arena_init(&arena, 4096);
}

void tearDown(void) {
    if (trie) {
        route_trie_free(trie);
        trie = NULL;
    }
    turbo_arena_free(&arena);
}

/* ============================================================================
 * Trie Creation Tests
 * ============================================================================ */

void test_trie_create(void) {
    TEST_ASSERT_NOT_NULL(trie);
    TEST_ASSERT_NOT_NULL(trie->root);
    TEST_ASSERT_EQUAL(0, trie->route_count);
}

void test_trie_create_multiple(void) {
    route_trie_t *trie2 = route_trie_create();
    TEST_ASSERT_NOT_NULL(trie2);
    TEST_ASSERT_NOT_EQUAL(trie, trie2);
    route_trie_free(trie2);
}

/* ============================================================================
 * Route Addition Tests
 * ============================================================================ */

void test_add_simple_route(void) {
    int result = route_trie_add(trie, "GET", "/users", dummy_handler, NULL);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, trie->route_count);
}

void test_add_multiple_routes(void) {
    TEST_ASSERT_EQUAL(0, route_trie_add(trie, "GET", "/users", dummy_handler, NULL));
    TEST_ASSERT_EQUAL(0, route_trie_add(trie, "POST", "/users", dummy_handler, NULL));
    TEST_ASSERT_EQUAL(0, route_trie_add(trie, "GET", "/posts", dummy_handler, NULL));
    TEST_ASSERT_EQUAL(3, trie->route_count);
}

void test_add_nested_routes(void) {
    TEST_ASSERT_EQUAL(0, route_trie_add(trie, "GET", "/api/v1/users", dummy_handler, NULL));
    TEST_ASSERT_EQUAL(0, route_trie_add(trie, "GET", "/api/v1/posts", dummy_handler, NULL));
    TEST_ASSERT_EQUAL(0, route_trie_add(trie, "GET", "/api/v2/users", dummy_handler, NULL));
    TEST_ASSERT_EQUAL(3, trie->route_count);
}

void test_add_route_with_param(void) {
    int result = route_trie_add(trie, "GET", "/users/:id", dummy_handler, NULL);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, trie->route_count);
}

void test_add_route_with_multiple_params(void) {
    int result = route_trie_add(trie, "GET", "/users/:userId/posts/:postId", dummy_handler, NULL);
    TEST_ASSERT_EQUAL(0, result);
}

void test_add_route_with_wildcard(void) {
    int result = route_trie_add(trie, "GET", "/static/*", dummy_handler, NULL);
    TEST_ASSERT_EQUAL(0, result);
}

void test_add_root_route(void) {
    int result = route_trie_add(trie, "GET", "/", dummy_handler, NULL);
    TEST_ASSERT_EQUAL(0, result);
}

void test_add_route_null_handler(void) {
    int result = route_trie_add(trie, "GET", "/test", NULL, NULL);
    TEST_ASSERT_NOT_EQUAL(0, result);
}

void test_add_route_null_method(void) {
    int result = route_trie_add(trie, NULL, "/test", dummy_handler, NULL);
    TEST_ASSERT_NOT_EQUAL(0, result);
}

void test_add_route_null_path(void) {
    int result = route_trie_add(trie, "GET", NULL, dummy_handler, NULL);
    TEST_ASSERT_NOT_EQUAL(0, result);
}

/* ============================================================================
 * Route Matching Tests - Exact Match
 * ============================================================================ */

void test_match_simple_route(void) {
    route_trie_add(trie, "GET", "/users", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/users", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(handler_a, match.handler);
    TEST_ASSERT_EQUAL(0, match.param_count);
}

void test_match_nested_route(void) {
    route_trie_add(trie, "GET", "/api/v1/users", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/api/v1/users", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(handler_a, match.handler);
}

void test_match_different_methods(void) {
    route_trie_add(trie, "GET", "/users", handler_a, NULL);
    route_trie_add(trie, "POST", "/users", handler_b, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/users", &path);

    route_match_t match_get, match_post;
    bool found_get = route_trie_match(trie, "GET", &path, &match_get);
    bool found_post = route_trie_match(trie, "POST", &path, &match_post);

    TEST_ASSERT_TRUE(found_get);
    TEST_ASSERT_TRUE(found_post);
    TEST_ASSERT_EQUAL(handler_a, match_get.handler);
    TEST_ASSERT_EQUAL(handler_b, match_post.handler);
}

void test_match_root_route(void) {
    route_trie_add(trie, "GET", "/", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(handler_a, match.handler);
}

void test_match_not_found(void) {
    route_trie_add(trie, "GET", "/users", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/posts", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_FALSE(found);
}

void test_match_wrong_method(void) {
    route_trie_add(trie, "GET", "/users", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/users", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "POST", &path, &match);

    TEST_ASSERT_FALSE(found);
}

/* ============================================================================
 * Route Matching Tests - Parameters
 * ============================================================================ */

void test_match_single_param(void) {
    route_trie_add(trie, "GET", "/users/:id", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/users/123", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(handler_a, match.handler);
    TEST_ASSERT_EQUAL(1, match.param_count);
    TEST_ASSERT_EQUAL(2, match.params[0].key.len);
    TEST_ASSERT_EQUAL_STRING_LEN("id", match.params[0].key.data, 2);
    TEST_ASSERT_EQUAL(3, match.params[0].value.len);
    TEST_ASSERT_EQUAL_STRING_LEN("123", match.params[0].value.data, 3);
}

void test_match_multiple_params(void) {
    route_trie_add(trie, "GET", "/users/:userId/posts/:postId", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/users/42/posts/99", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(2, match.param_count);

    TEST_ASSERT_EQUAL_STRING_LEN("userId", match.params[0].key.data, match.params[0].key.len);
    TEST_ASSERT_EQUAL_STRING_LEN("42", match.params[0].value.data, match.params[0].value.len);

    TEST_ASSERT_EQUAL_STRING_LEN("postId", match.params[1].key.data, match.params[1].key.len);
    TEST_ASSERT_EQUAL_STRING_LEN("99", match.params[1].value.data, match.params[1].value.len);
}

void test_match_param_with_special_chars(void) {
    route_trie_add(trie, "GET", "/users/:name", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/users/john-doe", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(1, match.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("john-doe", match.params[0].value.data, match.params[0].value.len);
}

void test_match_param_mixed_with_static(void) {
    route_trie_add(trie, "GET", "/api/users/:id/profile", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/api/users/456/profile", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(1, match.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("456", match.params[0].value.data, match.params[0].value.len);
}

/* ============================================================================
 * Route Matching Tests - Wildcard
 * ============================================================================ */

void test_match_wildcard_single_segment(void) {
    route_trie_add(trie, "GET", "/static/*", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/static/style.css", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL(handler_a, match.handler);
}

void test_match_wildcard_multiple_segments(void) {
    route_trie_add(trie, "GET", "/static/*", handler_a, NULL);

    tokenized_path_t path = {0};
    tokenize_path(&arena, "/static/css/main.css", &path);

    route_match_t match;
    bool found = route_trie_match(trie, "GET", &path, &match);

    TEST_ASSERT_TRUE(found);
}

/* ============================================================================
 * Method Index Tests
 * ============================================================================ */

void test_method_index_get(void) {
    TEST_ASSERT_EQUAL(METHOD_GET, get_method_index("GET"));
}

void test_method_index_post(void) {
    TEST_ASSERT_EQUAL(METHOD_POST, get_method_index("POST"));
}

void test_method_index_put(void) {
    TEST_ASSERT_EQUAL(METHOD_PUT, get_method_index("PUT"));
}

void test_method_index_delete(void) {
    TEST_ASSERT_EQUAL(METHOD_DELETE, get_method_index("DELETE"));
}

void test_method_index_patch(void) {
    TEST_ASSERT_EQUAL(METHOD_PATCH, get_method_index("PATCH"));
}

void test_method_index_head(void) {
    TEST_ASSERT_EQUAL(METHOD_HEAD, get_method_index("HEAD"));
}

void test_method_index_options(void) {
    TEST_ASSERT_EQUAL(METHOD_OPTIONS, get_method_index("OPTIONS"));
}

void test_method_index_unknown(void) {
    TEST_ASSERT_EQUAL(METHOD_UNKNOWN, get_method_index("INVALID"));
}

void test_method_index_case_insensitive(void) {
    TEST_ASSERT_EQUAL(METHOD_GET, get_method_index("get"));
    TEST_ASSERT_EQUAL(METHOD_POST, get_method_index("Post"));
}

void test_method_index_null(void) {
    TEST_ASSERT_EQUAL(METHOD_UNKNOWN, get_method_index(NULL));
}

/* ============================================================================
 * Path Tokenization Tests
 * ============================================================================ */

void test_tokenize_simple_path(void) {
    tokenized_path_t path = {0};
    int result = tokenize_path(&arena, "/users", &path);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, path.count);
    TEST_ASSERT_EQUAL_STRING_LEN("users", path.segments[0].start, path.segments[0].len);
    TEST_ASSERT_FALSE(path.segments[0].is_param);
    TEST_ASSERT_FALSE(path.segments[0].is_wildcard);
}

void test_tokenize_nested_path(void) {
    tokenized_path_t path = {0};
    int result = tokenize_path(&arena, "/api/v1/users", &path);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(3, path.count);
    TEST_ASSERT_EQUAL_STRING_LEN("api", path.segments[0].start, path.segments[0].len);
    TEST_ASSERT_EQUAL_STRING_LEN("v1", path.segments[1].start, path.segments[1].len);
    TEST_ASSERT_EQUAL_STRING_LEN("users", path.segments[2].start, path.segments[2].len);
}

void test_tokenize_root_path(void) {
    tokenized_path_t path = {0};
    int result = tokenize_path(&arena, "/", &path);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(0, path.count);
}

void test_tokenize_param_path(void) {
    tokenized_path_t path = {0};
    int result = tokenize_path(&arena, "/users/:id", &path);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(2, path.count);
    TEST_ASSERT_FALSE(path.segments[0].is_param);
    TEST_ASSERT_TRUE(path.segments[1].is_param);
}

void test_tokenize_wildcard_path(void) {
    tokenized_path_t path = {0};
    int result = tokenize_path(&arena, "/static/*", &path);

    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(2, path.count);
    TEST_ASSERT_FALSE(path.segments[0].is_wildcard);
    TEST_ASSERT_TRUE(path.segments[1].is_wildcard);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    UNITY_BEGIN();

    /* Trie creation */
    RUN_TEST(test_trie_create);
    RUN_TEST(test_trie_create_multiple);

    /* Route addition */
    RUN_TEST(test_add_simple_route);
    RUN_TEST(test_add_multiple_routes);
    RUN_TEST(test_add_nested_routes);
    RUN_TEST(test_add_route_with_param);
    RUN_TEST(test_add_route_with_multiple_params);
    RUN_TEST(test_add_route_with_wildcard);
    RUN_TEST(test_add_root_route);
    RUN_TEST(test_add_route_null_handler);
    RUN_TEST(test_add_route_null_method);
    RUN_TEST(test_add_route_null_path);

    /* Exact matching */
    RUN_TEST(test_match_simple_route);
    RUN_TEST(test_match_nested_route);
    RUN_TEST(test_match_different_methods);
    RUN_TEST(test_match_root_route);
    RUN_TEST(test_match_not_found);
    RUN_TEST(test_match_wrong_method);

    /* Parameter matching */
    RUN_TEST(test_match_single_param);
    RUN_TEST(test_match_multiple_params);
    RUN_TEST(test_match_param_with_special_chars);
    RUN_TEST(test_match_param_mixed_with_static);

    /* Wildcard matching */
    RUN_TEST(test_match_wildcard_single_segment);
    RUN_TEST(test_match_wildcard_multiple_segments);

    /* Method index */
    RUN_TEST(test_method_index_get);
    RUN_TEST(test_method_index_post);
    RUN_TEST(test_method_index_put);
    RUN_TEST(test_method_index_delete);
    RUN_TEST(test_method_index_patch);
    RUN_TEST(test_method_index_head);
    RUN_TEST(test_method_index_options);
    RUN_TEST(test_method_index_unknown);
    RUN_TEST(test_method_index_case_insensitive);
    RUN_TEST(test_method_index_null);

    /* Path tokenization */
    RUN_TEST(test_tokenize_simple_path);
    RUN_TEST(test_tokenize_nested_path);
    RUN_TEST(test_tokenize_root_path);
    RUN_TEST(test_tokenize_param_path);
    RUN_TEST(test_tokenize_wildcard_path);

    return UNITY_END();
}
