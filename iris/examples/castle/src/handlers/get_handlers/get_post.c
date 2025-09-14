#include "context.h"
#include "handlers.h"

typedef struct
{
  turbo_arena_t* pool;
  Res* res;
  char* username;
  char* post_slug;
} ctx_t;

// static void free_ctx(ctx_t *ctx)
// {
//     if (!ctx || !ctx->pool)
//         return;

//     Arena *arena = ctx->pool;
//     arena_free(arena);
//     free(arena);
// }

static void on_query_posts(pg_async_t* pg, PGresult* result, void* data);

void get_post(Req* req, Res* res)
{
  const char* post_slug = get_params(req, "post");

  auth_context_t* auth_ctx = (auth_context_t*)get_context(req);
  if (!auth_ctx) {
    send_text(res, 500, "No auth context");
    return;
  }

  const char* user_slug = auth_ctx->user_slug;
  bool is_author = auth_ctx->is_author;

  turbo_arena_t* async_pool = calloc(1, sizeof(turbo_arena_t));
  if (!async_pool) {
    send_text(res, 500, "Arena allocation failed");
    return;
  }
  turbo_arena_init(async_pool, 4096);

  ctx_t* ctx = turbo_arena_alloc(async_pool, sizeof(ctx_t));
  if (!ctx) {
    send_text(res, 500, "Context allocation failed");
    return;
  }

  // Store arena reference
  ctx->pool = async_pool;

  ctx->res = arena_copy_res(async_pool, res);
  if (!ctx->res) {
    free_ctx(ctx->pool);
    send_text(res, 500, "Response copy failed");
    return;
  }

  ctx->username = turbo_arena_strdup(async_pool, user_slug);
  ctx->post_slug = turbo_arena_strdup(async_pool, post_slug);

  pg_async_t* pg = pquv_create(db, ctx);
  if (!pg) {
    free_ctx(ctx->pool);
    send_text(res, 500, "Database connection error");
    return;
  }

  char* select_sql;
  if (is_author) {
    select_sql = "SELECT p.id, p.header, p.slug, p.content, p.reading_time, "
                    "       p.author_id, u.username, p.created_at, p.updated_at, p.is_hidden, "
                    "       COALESCE(string_agg(c.category, ','), '') as categories, "
                    "       COALESCE(string_agg(c.slug, ','), '') as category_slugs, "
                    "       COALESCE(string_agg(c.id::text, ','), '') as category_ids "
                    "FROM posts p "
                    "JOIN users u ON p.author_id = u.id "
                    "LEFT JOIN post_categories pc ON p.id = pc.post_id "
                    "LEFT JOIN categories c ON pc.category_id = c.id "
                    "WHERE u.username = $1 AND p.slug = $2 "
                    "GROUP BY p.id, u.username";
  } else {
    select_sql = "SELECT p.id, p.header, p.slug, p.content, p.reading_time, "
                    "       p.author_id, u.username, p.created_at, p.updated_at, p.is_hidden, "
                    "       COALESCE(string_agg(c.category, ','), '') as categories, "
                    "       COALESCE(string_agg(c.slug, ','), '') as category_slugs, "
                    "       COALESCE(string_agg(c.id::text, ','), '') as category_ids "
                    "FROM posts p "
                    "JOIN users u ON p.author_id = u.id "
                    "LEFT JOIN post_categories pc ON p.id = pc.post_id "
                    "LEFT JOIN categories c ON pc.category_id = c.id "
                    "WHERE u.username = $1 AND p.slug = $2 AND p.is_hidden = FALSE "
                    "GROUP BY p.id, u.username";
  }

  const char* params[] = {ctx->username, ctx->post_slug};

  int qr = pquv_queue(pg, select_sql, 2, params, on_query_posts, ctx);
  if (qr != 0) {
    free_ctx(ctx->pool);
    send_text(res, 500, "Failed to queue query");
    return;
  }

  int exec_result = pquv_execute(pg);
  if (exec_result != 0) {
    free_ctx(ctx->pool);
    send_text(res, 500, "Failed to execute query");
    return;
  }
}

static void on_query_posts(pg_async_t* pg, PGresult* result, void* data)
{
  ctx_t* ctx = (ctx_t*)data;

  if (!ctx || !ctx->res) {
    free_ctx(ctx->pool);
    return;
  }

  if (!result) {
    free_ctx(ctx->pool);
    return;
  }

  ExecStatusType status = PQresultStatus(result);

  if (status != PGRES_TUPLES_OK) {
    free_ctx(ctx->pool);
    return;
  }

  int ntuples = PQntuples(result);

  if (ntuples == 0) {
    send_text(ctx->res, NOT_FOUND, "Post not found");
    free_ctx(ctx->pool);
    return;
  }

  cJSON* response = cJSON_CreateObject();
  if (!response) {
    free_ctx(ctx->pool);
    return;
  }

  char* id_val = PQgetvalue(result, 0, PQfnumber(result, "id"));
  char* reading_time_val = PQgetvalue(result, 0, PQfnumber(result, "reading_time"));
  char* author_id_val = PQgetvalue(result, 0, PQfnumber(result, "author_id"));

  cJSON_AddNumberToObject(response, "id", atoi(id_val));
  cJSON_AddNumberToObject(response, "reading_time", atoi(reading_time_val));
  cJSON_AddNumberToObject(response, "author_id", atoi(author_id_val));

  char* header_val = PQgetvalue(result, 0, PQfnumber(result, "header"));
  cJSON_AddStringToObject(response, "header", header_val);

  char* slug_val = PQgetvalue(result, 0, PQfnumber(result, "slug"));
  cJSON_AddStringToObject(response, "slug", slug_val);

  char* content_val = PQgetvalue(result, 0, PQfnumber(result, "content"));
  cJSON_AddStringToObject(response, "content", content_val);

  char* username_val = PQgetvalue(result, 0, PQfnumber(result, "username"));
  cJSON_AddStringToObject(response, "username", username_val);

  char* created_at_val = PQgetvalue(result, 0, PQfnumber(result, "created_at"));
  cJSON_AddStringToObject(response, "created_at", created_at_val);

  char* updated_at_val = PQgetvalue(result, 0, PQfnumber(result, "updated_at"));
  cJSON_AddStringToObject(response, "updated_at", updated_at_val);

  char* is_hidden_val = PQgetvalue(result, 0, PQfnumber(result, "is_hidden"));

  cJSON_AddBoolToObject(response, "is_hidden", strcmp(is_hidden_val, "t") == 0);

  char* cats = PQgetvalue(result, 0, PQfnumber(result, "categories"));
  char* slugs = PQgetvalue(result, 0, PQfnumber(result, "category_slugs"));
  char* ids = PQgetvalue(result, 0, PQfnumber(result, "category_ids"));

  cJSON* arr = cJSON_CreateArray();
  if (!arr) {
    cJSON_Delete(response);
    free_ctx(ctx->pool);
    return;
  }

  if (cats && *cats) {
    char *c_copy = strdup(cats), *s_copy = strdup(slugs), *i_copy = strdup(ids);
    if (!c_copy || !s_copy || !i_copy) {
      free(c_copy);
      free(s_copy);
      free(i_copy);
      cJSON_Delete(arr);
      cJSON_Delete(response);
      free_ctx(ctx->pool);
      return;
    }

    char *ctok, *stok, *itok;
    char *save1, *save2, *save3;

    ctok = strtok_r(c_copy, ",", &save1);
    stok = strtok_r(s_copy, ",", &save2);
    itok = strtok_r(i_copy, ",", &save3);

    while (ctok && stok && itok) {
      cJSON* o = cJSON_CreateObject();
      cJSON_AddNumberToObject(o, "id", atoi(itok));
      cJSON_AddStringToObject(o, "category", ctok);
      cJSON_AddStringToObject(o, "slug", stok);
      cJSON_AddItemToArray(arr, o);

      ctok = strtok_r(NULL, ",", &save1);
      stok = strtok_r(NULL, ",", &save2);
      itok = strtok_r(NULL, ",", &save3);
    }

    free(c_copy);
    free(s_copy);
    free(i_copy);
  }

  cJSON_AddItemToObject(response, "categories", arr);

  char* json_str = cJSON_PrintUnformatted(response);
  if (!json_str) {
    cJSON_Delete(response);
    free_ctx(ctx->pool);
    return;
  }

  send_json(ctx->res, OK, json_str);
  free(json_str);
  cJSON_Delete(response);
  free_ctx(ctx->pool);
}
