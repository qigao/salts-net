/**
 * @file redis_cache_example.c
 * @brief Example web server with Redis caching using Iris framework
 *
 * Demonstrates:
 * - Redis client integration with Iris
 * - Caching expensive operations
 * - Session storage in Redis
 * - Rate limiting with Redis
 */

#include "iris.h"
#include "redis_client.h"
#include "server.h"
#include <stdlib.h>
#include <string.h>
#include "tlog.h"
#include <time.h>
#include <stb_sprintf.h>

/* Global Redis client */
static redis_client_t *redis = NULL;

/* Cache hit/miss statistics */
static struct {
  int hits;
  int misses;
} cache_stats = {0, 0};

/* Callback for cached data */
void on_cache_get(redis_client_t *client, redis_reply_t *reply, void *data) {
  Res *res = (Res *)data;

  if (reply->type == REDIS_REPLY_BULK_STRING) {
    /* Cache hit */
    cache_stats.hits++;
    TLOG_INFO("[CACHE HIT] Serving from cache");

    char *response = turbo_arena_sprintf(
        res->arena, "{\"data\":%s,\"cached\":true,\"stats\":{\"hits\":%d,\"misses\":%d}}",
        reply->str, cache_stats.hits, cache_stats.misses);

    send_json(res, 200, response);
  } else {
    /* Cache miss - generate data */
    cache_stats.misses++;
    TLOG_INFO("[CACHE MISS] Generating data");

    /* Simulate expensive operation */
    time_t now = time(NULL);
    char *data_json =
        turbo_arena_sprintf(res->arena, "{\"message\":\"Hello from server\",\"timestamp\":%ld}", now);

    /* Store in cache with 60 second expiration */
    redis_set(redis, "cached_data", data_json, NULL, NULL);
    redis_expire(redis, "cached_data", 60, NULL, NULL);

    char *response = turbo_arena_sprintf(
        res->arena, "{\"data\":%s,\"cached\":false,\"stats\":{\"hits\":%d,\"misses\":%d}}",
        data_json, cache_stats.hits, cache_stats.misses);

    send_json(res, 200, response);
  }
}

/* GET /cached - Cached endpoint */
void cached_handler(Req *req, Res *res) { redis_get(redis, "cached_data", on_cache_get, res); }

/* GET /counter - Increment counter */
void on_counter_incr(redis_client_t *client, redis_reply_t *reply, void *data) {
  Res *res = (Res *)data;

  if (reply->type == REDIS_REPLY_INTEGER) {
    char *response = turbo_arena_sprintf(res->arena, "{\"counter\":%lld}", reply->integer);
    send_json(res, 200, response);
  } else {
    char *error = turbo_arena_sprintf(res->arena, "{\"error\":\"Failed to increment counter\"}");
    send_json(res, 500, error);
  }
}

void counter_handler(Req *req, Res *res) {
  redis_incr(redis, "page_counter", on_counter_incr, res);
}

/* GET /session - Session demo */
void on_session_get(redis_client_t *client, redis_reply_t *reply, void *data) {
  Res *res = (Res *)data;

  if (reply->type == REDIS_REPLY_BULK_STRING) {
    char *response = turbo_arena_sprintf(res->arena, "{\"session\":%s}", reply->str);
    send_json(res, 200, response);
  } else {
    /* No session - create one */
    time_t now = time(NULL);
    char session_data[256];
    stbsp_snprintf(session_data, sizeof(session_data), "{\"user\":\"guest\",\"created\":%ld}", now);

    redis_set(redis, "session:demo", session_data, NULL, NULL);
    redis_expire(redis, "session:demo", 3600, NULL, NULL); /* 1 hour */

    char *response = turbo_arena_sprintf(res->arena, "{\"session\":%s,\"new\":true}", session_data);
    send_json(res, 200, response);
  }
}

void session_handler(Req *req, Res *res) { redis_get(redis, "session:demo", on_session_get, res); }

/* GET /leaderboard - Sorted set demo */
void on_leaderboard(redis_client_t *client, redis_reply_t *reply, void *data) {
  Res *res = (Res *)data;

  if (reply->type == REDIS_REPLY_ARRAY) {
    /* Build JSON array from Redis array */
    char *json = turbo_arena_sprintf(res->arena, "{\"leaderboard\":[");

    for (size_t i = 0; i < reply->element_count; i++) {
      if (reply->elements[i]->type == REDIS_REPLY_BULK_STRING) {
        if (i > 0) {
          json = turbo_arena_sprintf(res->arena, "%s,", json);
        }
        json = turbo_arena_sprintf(res->arena, "%s\"%s\"", json, reply->elements[i]->str);
      }
    }

    json = turbo_arena_sprintf(res->arena, "%s]}", json);
    send_json(res, 200, json);
  } else {
    char *error = turbo_arena_sprintf(res->arena, "{\"error\":\"Failed to get leaderboard\"}");
    send_json(res, 500, error);
  }
}

void leaderboard_handler(Req *req, Res *res) {
  /* Get top 10 from sorted set */
  redis_command(redis, on_leaderboard, res, "ZREVRANGE leaderboard 0 9");
}

/* GET /stats - Cache statistics */
void stats_handler(Req *req, Res *res) {
  double hit_rate = 0.0;
  int total = cache_stats.hits + cache_stats.misses;
  if (total > 0) {
    hit_rate = (double)cache_stats.hits / total * 100.0;
  }

  char *response =
      turbo_arena_sprintf(res->arena, "{\"hits\":%d,\"misses\":%d,\"total\":%d,\"hit_rate\":%.2f}",
                    cache_stats.hits, cache_stats.misses, total, hit_rate);

  send_json(res, 200, response);
}

/* GET / - Home page */
void home_handler(Req *req, Res *res) {
  const char *html = "<!DOCTYPE html>\n"
                     "<html>\n"
                     "<head>\n"
                     "  <title>Redis Cache Example</title>\n"
                     "  <style>\n"
                     "    body { font-family: Arial, sans-serif; margin: 40px; }\n"
                     "    h1 { color: #333; }\n"
                     "    .endpoint { background: #f4f4f4; padding: 10px; margin: 10px 0; }\n"
                     "    code { background: #e8e8e8; padding: 2px 6px; }\n"
                     "  </style>\n"
                     "</head>\n"
                     "<body>\n"
                     "  <h1>Redis Cache Example</h1>\n"
                     "  <p>Iris web framework with Redis caching</p>\n"
                     "  \n"
                     "  <h2>Endpoints</h2>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>GET /cached</strong> - Cached data (60s TTL)<br>\n"
                     "    <a href='/cached'>Try it</a>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>GET /counter</strong> - Page counter<br>\n"
                     "    <a href='/counter'>Try it</a>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>GET /session</strong> - Session demo<br>\n"
                     "    <a href='/session'>Try it</a>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>GET /leaderboard</strong> - Sorted set demo<br>\n"
                     "    <a href='/leaderboard'>Try it</a>\n"
                     "  </div>\n"
                     "  <div class='endpoint'>\n"
                     "    <strong>GET /stats</strong> - Cache statistics<br>\n"
                     "    <a href='/stats'>Try it</a>\n"
                     "  </div>\n"
                     "</body>\n"
                     "</html>";

  send_html(res, 200, html);
}

/* Redis connection callback */
void on_redis_connect(redis_client_t *client, int status, void *user_data) {
  if (status == 0) {
    TLOG_INFO(" Connected to Redis");

    /* Initialize some demo data */
    redis_command(client, NULL, NULL, "ZADD leaderboard 100 Alice");
    redis_command(client, NULL, NULL, "ZADD leaderboard 95 Bob");
    redis_command(client, NULL, NULL, "ZADD leaderboard 90 Charlie");
    redis_command(client, NULL, NULL, "ZADD leaderboard 85 David");
    redis_command(client, NULL, NULL, "ZADD leaderboard 80 Eve");
  } else {
    TLOG_ERROR(" Failed to connect to Redis");
  }
}

int main(void) {
  TLOG_INFO("Redis Cache Example");
  TLOG_INFO("===================");

  /* Create and connect Redis client */
  redis = redis_client_create("localhost", 6379);
  if (!redis) {
    TLOG_ERROR("Failed to create Redis client");
    return 1;
  }

  redis_client_connect(redis, on_redis_connect, NULL);

  /* Register routes */
  get("/", home_handler);
  get("/cached", cached_handler);
  get("/counter", counter_handler);
  get("/session", session_handler);
  get("/leaderboard", leaderboard_handler);
  get("/stats", stats_handler);

  TLOG_INFO("Server starting on http://localhost:8080");
  TLOG_INFO("Redis: localhost:6379");
  TLOG_INFO("");
  TLOG_INFO("Endpoints:");
  TLOG_INFO("  http://localhost:8080/         - Home page");
  TLOG_INFO("  http://localhost:8080/cached   - Cached data");
  TLOG_INFO("  http://localhost:8080/counter  - Page counter");
  TLOG_INFO("  http://localhost:8080/session  - Session demo");
  TLOG_INFO("  http://localhost:8080/leaderboard - Leaderboard");
  TLOG_INFO("  http://localhost:8080/stats    - Cache stats");
  TLOG_INFO("");
  TLOG_INFO("Press Ctrl+C to stop");

  /* Start server */
  int result = ecewo(8080);
  if (result != 0) {
    TLOG_ERROR("Server failed to start: {:d}", result);
  }

  /* Cleanup */
  redis_client_destroy(redis);

  return 0;
}
