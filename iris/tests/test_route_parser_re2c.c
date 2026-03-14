/**
 * @file test_route_parser_re2c.c
 * @brief Tests for re2c-based route parser
 */

#include "tinytest.h"
#include "route_trie.h"
#include "turbo_buffer.h"
#include <string.h>

static mem_pool_t test_arena;

spec("route_parser_re2c") {
    before_each() {
        int ret = mem_init(&test_arena, 4096);
        check_int_eq(ret, 0);
    }

    after_each() {
        mem_destroy(&test_arena);
    }

    /* HTTP Method Parsing Tests */

    it("should parse HTTP method GET") {
        check_int_eq(parse_http_method_re2c("GET", 3), METHOD_GET);
        check_int_eq(parse_http_method_re2c("get", 3), METHOD_GET);
        check_int_eq(parse_http_method_re2c("Get", 3), METHOD_GET);
    }

    it("should parse HTTP method POST") {
        check_int_eq(parse_http_method_re2c("POST", 4), METHOD_POST);
        check_int_eq(parse_http_method_re2c("post", 4), METHOD_POST);
    }

    it("should parse HTTP method PUT") {
        check_int_eq(parse_http_method_re2c("PUT", 3), METHOD_PUT);
        check_int_eq(parse_http_method_re2c("put", 3), METHOD_PUT);
    }

    it("should parse HTTP method DELETE") {
        check_int_eq(parse_http_method_re2c("DELETE", 6), METHOD_DELETE);
        check_int_eq(parse_http_method_re2c("delete", 6), METHOD_DELETE);
    }

    it("should parse HTTP method PATCH") {
        check_int_eq(parse_http_method_re2c("PATCH", 5), METHOD_PATCH);
    }

    it("should parse HTTP method HEAD") {
        check_int_eq(parse_http_method_re2c("HEAD", 4), METHOD_HEAD);
    }

    it("should parse HTTP method OPTIONS") {
        check_int_eq(parse_http_method_re2c("OPTIONS", 7), METHOD_OPTIONS);
    }

    it("should handle unknown HTTP methods") {
        check_int_eq(parse_http_method_re2c("INVALID", 7), METHOD_UNKNOWN);
        check_int_eq(parse_http_method_re2c("GETS", 4), METHOD_UNKNOWN);
        check_int_eq(parse_http_method_re2c("", 0), METHOD_UNKNOWN);
        check_int_eq(parse_http_method_re2c(NULL, 0), METHOD_UNKNOWN);
    }

    /* Path Segment Counting Tests */

    it("should count simple path segments") {
        check_int_eq(count_path_segments_re2c("/users/123/posts", 16), 3);
        check_int_eq(count_path_segments_re2c("/api/v1", 7), 2);
        check_int_eq(count_path_segments_re2c("/users", 6), 1);
    }

    it("should count root path segments") {
        check_int_eq(count_path_segments_re2c("/", 1), 0);
        check_int_eq(count_path_segments_re2c("", 0), 0);
    }

    it("should count segments with trailing slash") {
        check_int_eq(count_path_segments_re2c("/users/", 7), 1);
        check_int_eq(count_path_segments_re2c("/a/b/c/", 7), 3);
    }

    it("should count segments with double slash") {
        check_int_eq(count_path_segments_re2c("/users//posts", 13), 2);
    }

    /* Path Tokenization Tests */

    it("should tokenize simple path") {
        tokenized_path_t result;
        int ret = tokenize_path_re2c(&test_arena, "/users/123/posts", &result);
        
        check_int_eq(ret, 0);
        check_int_eq(result.count, 3);
        
        check_int_eq(result.segments[0].len, 5);
        check_mem_eq(result.segments[0].start, "users", 5);
        check_false(result.segments[0].is_param);
        check_false(result.segments[0].is_wildcard);
        
        check_int_eq(result.segments[1].len, 3);
        check_mem_eq(result.segments[1].start, "123", 3);
        
        check_int_eq(result.segments[2].len, 5);
        check_mem_eq(result.segments[2].start, "posts", 5);
    }

    it("should tokenize path with params") {
        tokenized_path_t result;
        int ret = tokenize_path_re2c(&test_arena, "/users/:id/posts/:postId", &result);
        
        check_int_eq(ret, 0);
        check_int_eq(result.count, 4);
        
        check_false(result.segments[0].is_param);
        check_true(result.segments[1].is_param);
        check_false(result.segments[2].is_param);
        check_true(result.segments[3].is_param);
    }

    it("should tokenize path with wildcard") {
        tokenized_path_t result;
        int ret = tokenize_path_re2c(&test_arena, "/static/*filepath", &result);
        
        check_int_eq(ret, 0);
        check_int_eq(result.count, 2);
        
        check_false(result.segments[0].is_wildcard);
        check_true(result.segments[1].is_wildcard);
    }

    it("should tokenize root path") {
        tokenized_path_t result;
        int ret = tokenize_path_re2c(&test_arena, "/", &result);
        
        check_int_eq(ret, 0);
        check_int_eq(result.count, 0);
    }

    it("should tokenize empty path") {
        tokenized_path_t result;
        int ret = tokenize_path_re2c(&test_arena, "", &result);
        
        check_int_eq(ret, 0);
        check_int_eq(result.count, 0);
    }

    it("should handle NULL path") {
        tokenized_path_t result;
        int ret = tokenize_path_re2c(&test_arena, NULL, &result);
        
        check_int_eq(ret, -1);
    }

    /* Segment Extraction Tests */

    it("should extract simple segments") {
        const char *path = "/users/posts";
        const char *cursor = path;
        const char *limit = path + strlen(path);
        path_segment_t seg;
        
        check_true(extract_path_segment_re2c(&cursor, limit, &seg));
        check_int_eq(seg.len, 5);
        check_mem_eq(seg.start, "users", 5);
        
        check_true(extract_path_segment_re2c(&cursor, limit, &seg));
        check_int_eq(seg.len, 5);
        check_mem_eq(seg.start, "posts", 5);
        
        check_false(extract_path_segment_re2c(&cursor, limit, &seg));
    }

    it("should extract param segment") {
        const char *path = "/:id";
        const char *cursor = path;
        const char *limit = path + strlen(path);
        path_segment_t seg;
        
        check_true(extract_path_segment_re2c(&cursor, limit, &seg));
        check_true(seg.is_param);
        check_int_eq(seg.len, 3);
    }

    /* Integration with get_method_index */

    it("should integrate with get_method_index") {
        check_int_eq(get_method_index("GET"), METHOD_GET);
        check_int_eq(get_method_index("POST"), METHOD_POST);
        check_int_eq(get_method_index("PUT"), METHOD_PUT);
        check_int_eq(get_method_index("DELETE"), METHOD_DELETE);
        check_int_eq(get_method_index("INVALID"), METHOD_UNKNOWN);
        check_int_eq(get_method_index(NULL), METHOD_UNKNOWN);
    }

    /* Integration with tokenize_path */

    it("should integrate with tokenize_path") {
        tokenized_path_t result;
        int ret = tokenize_path(&test_arena, "/api/v1/users", &result);
        
        check_int_eq(ret, 0);
        check_int_eq(result.count, 3);
    }
}
