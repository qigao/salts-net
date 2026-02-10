/**
 * test_route_trie.c - Unit tests for iris route trie
 *
 * Tests trie-based routing with exact matches, parameters, and wildcards.
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "tinytest.h"
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

spec("route_trie") {
    before_each() {
        trie = route_trie_create();
        turbo_arena_init(&arena, 4096);
    }

    after_each() {
        if (trie) {
            route_trie_free(trie);
            trie = NULL;
        }
        turbo_arena_free(&arena);
    }

    /* ============================================================================
     * Trie Creation Tests
     * ============================================================================ */

    it("should create trie") {
        check_not_null(trie);
        check_not_null(trie->root);
        check_int_eq(trie->route_count, 0);
    }

    it("should create multiple tries") {
        route_trie_t *trie2 = route_trie_create();
        check_not_null(trie2);
        check_true(trie != trie2);
        route_trie_free(trie2);
    }

    /* ============================================================================
     * Route Addition Tests
     * ============================================================================ */

    it("should add simple route") {
        check_int_eq(route_trie_add(trie, "GET", "/users", dummy_handler, NULL), 0);
        check_int_eq(trie->route_count, 1);
    }

    it("should add multiple routes") {
        check_int_eq(route_trie_add(trie, "GET", "/users", dummy_handler, NULL), 0);
        check_int_eq(route_trie_add(trie, "POST", "/users", dummy_handler, NULL), 0);
        check_int_eq(route_trie_add(trie, "GET", "/posts", dummy_handler, NULL), 0);
        check_int_eq(trie->route_count, 3);
    }

    it("should add nested routes") {
        check_int_eq(route_trie_add(trie, "GET", "/api/v1/users", dummy_handler, NULL), 0);
        check_int_eq(route_trie_add(trie, "GET", "/api/v1/posts", dummy_handler, NULL), 0);
        check_int_eq(route_trie_add(trie, "GET", "/api/v2/users", dummy_handler, NULL), 0);
        check_int_eq(trie->route_count, 3);
    }

    it("should add route with param") {
        check_int_eq(route_trie_add(trie, "GET", "/users/:id", dummy_handler, NULL), 0);
        check_int_eq(trie->route_count, 1);
    }

    it("should add route with multiple params") {
        check_int_eq(route_trie_add(trie, "GET", "/users/:userId/posts/:postId", dummy_handler, NULL), 0);
    }

    it("should add route with wildcard") {
        check_int_eq(route_trie_add(trie, "GET", "/static/*", dummy_handler, NULL), 0);
    }

    it("should add root route") {
        check_int_eq(route_trie_add(trie, "GET", "/", dummy_handler, NULL), 0);
    }

    it("should handle null handler in add") {
        check_int_ne(route_trie_add(trie, "GET", "/test", NULL, NULL), 0);
    }

    it("should handle null method in add") {
        check_int_ne(route_trie_add(trie, NULL, "/test", dummy_handler, NULL), 0);
    }

    it("should handle null path in add") {
        check_int_ne(route_trie_add(trie, "GET", NULL, dummy_handler, NULL), 0);
    }

    /* ============================================================================
     * Route Matching Tests - Exact Match
     * ============================================================================ */

    it("should match simple route") {
        route_trie_add(trie, "GET", "/users", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/users", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
        check_true(match.handler == handler_a);
        check_int_eq(match.param_count, 0);
    }

    it("should match nested route") {
        route_trie_add(trie, "GET", "/api/v1/users", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/api/v1/users", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
        check_true(match.handler == handler_a);
    }

    it("should match different methods") {
        route_trie_add(trie, "GET", "/users", handler_a, NULL);
        route_trie_add(trie, "POST", "/users", handler_b, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/users", &path);

        route_match_t match_get, match_post;
        bool found_get = route_trie_match(trie, "GET", &path, &match_get);
        bool found_post = route_trie_match(trie, "POST", &path, &match_post);

        check_true(found_get);
        check_true(found_post);
        check_true(match_get.handler == handler_a);
        check_true(match_post.handler == handler_b);
    }

    it("should match root route") {
        route_trie_add(trie, "GET", "/", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
        check_true(match.handler == handler_a);
    }

    it("should fail match if not found") {
        route_trie_add(trie, "GET", "/users", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/posts", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_false(found);
    }

    it("should fail match if wrong method") {
        route_trie_add(trie, "GET", "/users", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/users", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "POST", &path, &match);

        check_false(found);
    }

    /* ============================================================================
     * Route Matching Tests - Parameters
     * ============================================================================ */

    it("should match single param") {
        route_trie_add(trie, "GET", "/users/:id", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/users/123", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
        check_true(match.handler == handler_a);
        check_int_eq(match.param_count, 1);
        check_int_eq(match.params[0].key.len, 2);
        check_true(strncmp(match.params[0].key.data, "id", 2) == 0);
        check_int_eq(match.params[0].value.len, 3);
        check_true(strncmp(match.params[0].value.data, "123", 3) == 0);
    }

    it("should match multiple params") {
        route_trie_add(trie, "GET", "/users/:userId/posts/:postId", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/users/42/posts/99", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
        check_int_eq(match.param_count, 2);

        check_true(strncmp(match.params[0].key.data, "userId", match.params[0].key.len) == 0);
        check_true(strncmp(match.params[0].value.data, "42", match.params[0].value.len) == 0);

        check_true(strncmp(match.params[1].key.data, "postId", match.params[1].key.len) == 0);
        check_true(strncmp(match.params[1].value.data, "99", match.params[1].value.len) == 0);
    }

    it("should match param with special chars") {
        route_trie_add(trie, "GET", "/users/:name", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/users/john-doe", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
        check_int_eq(match.param_count, 1);
        check_int_eq(match.params[0].value.len, 8);
        check_true(strncmp(match.params[0].value.data, "john-doe", 8) == 0);
    }

    it("should match param mixed with static") {
        route_trie_add(trie, "GET", "/api/users/:id/profile", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/api/users/456/profile", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
        check_int_eq(match.param_count, 1);
        check_int_eq(match.params[0].value.len, 3);
        check_true(strncmp(match.params[0].value.data, "456", 3) == 0);
    }

    /* ============================================================================
     * Route Matching Tests - Wildcard
     * ============================================================================ */

    it("should match wildcard single segment") {
        route_trie_add(trie, "GET", "/static/*", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/static/style.css", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
        check_true(match.handler == handler_a);
    }

    it("should match wildcard multiple segments") {
        route_trie_add(trie, "GET", "/static/*", handler_a, NULL);

        tokenized_path_t path = {0};
        tokenize_path(&arena, "/static/css/main.css", &path);

        route_match_t match;
        bool found = route_trie_match(trie, "GET", &path, &match);

        check_true(found);
    }

    /* ============================================================================
     * Method Index Tests
     * ============================================================================ */

    it("should get method index") {
        check_int_eq(get_method_index("GET"), METHOD_GET);
        check_int_eq(get_method_index("POST"), METHOD_POST);
        check_int_eq(get_method_index("PUT"), METHOD_PUT);
        check_int_eq(get_method_index("DELETE"), METHOD_DELETE);
        check_int_eq(get_method_index("PATCH"), METHOD_PATCH);
        check_int_eq(get_method_index("HEAD"), METHOD_HEAD);
        check_int_eq(get_method_index("OPTIONS"), METHOD_OPTIONS);
        check_int_eq(get_method_index("INVALID"), METHOD_UNKNOWN);
        check_int_eq(get_method_index("get"), METHOD_GET);
        check_int_eq(get_method_index(NULL), METHOD_UNKNOWN);
    }

    /* ============================================================================
     * Path Tokenization Tests
     * ============================================================================ */

    it("should tokenize simple path") {
        tokenized_path_t path = {0};
        int result = tokenize_path(&arena, "/users", &path);

        check_int_eq(result, 0);
        check_int_eq(path.count, 1);
        check_int_eq(path.segments[0].len, 5);
        check_true(strncmp(path.segments[0].start, "users", 5) == 0);
        check_false(path.segments[0].is_param);
        check_false(path.segments[0].is_wildcard);
    }

    it("should tokenize nested path") {
        tokenized_path_t path = {0};
        int result = tokenize_path(&arena, "/api/v1/users", &path);

        check_int_eq(result, 0);
        check_int_eq(path.count, 3);
        check_true(strncmp(path.segments[0].start, "api", path.segments[0].len) == 0);
        check_true(strncmp(path.segments[1].start, "v1", path.segments[1].len) == 0);
        check_true(strncmp(path.segments[2].start, "users", path.segments[2].len) == 0);
    }

    it("should tokenize root path") {
        tokenized_path_t path = {0};
        int result = tokenize_path(&arena, "/", &path);

        check_int_eq(result, 0);
        check_int_eq(path.count, 0);
    }

    it("should tokenize param path") {
        tokenized_path_t path = {0};
        int result = tokenize_path(&arena, "/users/:id", &path);

        check_int_eq(result, 0);
        check_int_eq(path.count, 2);
        check_false(path.segments[0].is_param);
        check_true(path.segments[1].is_param);
    }

    it("should tokenize wildcard path") {
        tokenized_path_t path = {0};
        int result = tokenize_path(&arena, "/static/*", &path);

        check_int_eq(result, 0);
        check_int_eq(path.count, 2);
        check_false(path.segments[0].is_wildcard);
        check_true(path.segments[1].is_wildcard);
    }
}
