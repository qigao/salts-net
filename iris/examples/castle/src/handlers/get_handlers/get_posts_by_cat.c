#include "handlers.h"
#include "context.h"

typedef struct
{
    turbo_arena_t *pool;
    Res *res;
    bool is_author;
} ctx_t;

void on_result(pg_async_t *pg, PGresult *result, void *data);

void get_posts_by_cat(Req *req, Res *res)
{
    const char *category = get_query(req, "category");

    if (!category || strlen(category) == 0)
    {
        send_text(res, BAD_REQUEST, "Category parameter is required");
        return;
    }

    auth_context_t *auth_ctx = (auth_context_t *)get_context(req);

    turbo_arena_t *async_pool = malloc(sizeof(turbo_arena_t)); if (!async_pool || turbo_arena_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    ctx_t *ctx = turbo_arena_alloc(async_pool, sizeof(ctx_t));
    if (!ctx) {
        send_text(res, 500, "Context allocation failed");
        return;
    }

    // Store arena reference
    ctx->pool = async_pool;

    ctx->res = arena_copy_res(async_pool, res);
    if (!ctx->res)
    {
        free_ctx(ctx->pool);
        send_text(res, 500, "Response copy failed");
        return;
    }

    ctx->is_author = auth_ctx->is_author;

    pg_async_t *pg = pquv_create(db, ctx);
    if (!pg)
    {
        free_ctx(ctx->pool);
        send_text(res, 500, "Database connection error");
        return;
    }

    char *select_sql =
        "SELECT p.id, p.header, p.slug, p.reading_time, "
        "       p.author_id, u.username, p.created_at, p.updated_at, p.is_hidden, "
        "       COALESCE(string_agg(c.category, ','), '') as categories, "
        "       COALESCE(string_agg(c.slug, ','), '') as category_slugs, "
        "       COALESCE(string_agg(c.id::text, ','), '') as category_ids "
        "FROM posts p "
        "JOIN users u ON p.author_id = u.id "
        "JOIN post_categories pc ON p.id = pc.post_id "
        "JOIN categories c ON pc.category_id = c.id "
        "WHERE u.username = $1 AND c.slug = $2 "
        "GROUP BY p.id, u.username "
        "ORDER BY p.created_at DESC";

    const char *params[] = {auth_ctx->user_slug, category};

    int qr = pquv_queue(pg, select_sql, 2, params, on_result, ctx);
    if (qr != 0)
    {
        printf("ERROR: Failed to queue query, result=%d\n", qr);
        free_ctx(ctx->pool);
        send_text(res, 500, "Failed to queue query");
        return;
    }

    int exec_result = pquv_execute(pg);
    if (exec_result != 0)
    {
        printf("ERROR: Failed to execute, result=%d\n", exec_result);
        free_ctx(ctx->pool);
        send_text(res, 500, "Failed to execute query");
        return;
    }
}

void on_result(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("ERROR: on_query_posts: Invalid context\n");
        if (ctx)
            free_ctx(ctx->pool);
        return;
    }

    if (!result)
    {
        printf("ERROR: Result is NULL\n");
        free_ctx(ctx->pool);
        return;
    }

    if (PQresultStatus(result) != PGRES_TUPLES_OK)
    {
        send_text(ctx->res, 500, "DB select failed");
        free_ctx(ctx->pool);
        return;
    }

    int rows = PQntuples(result);

    json_value_t *root = json_create_object();
    json_value_t *posts = json_create_array();

    // If no any rows, return an empty array
    if (rows == 0)
    {
        json_object_add(root, "posts", posts);
        size_t out_len = 0;
        char *out = json_serialize(root, &out_len);
        send_json(ctx->res, 200, out);
        json_free(root);
        json_serialize_free(out);
        free_ctx(ctx->pool);
        return;
    }

    for (int i = 0; i < rows; i++)
    {
        bool hidden = (PQgetvalue(result, i, PQfnumber(result, "is_hidden"))[0] == 't');

        if (!ctx->is_author && hidden)
        {
            continue;
        }

        json_value_t *obj = json_create_object();

        // Add string fields
        json_object_set_string(obj, "header", PQgetvalue(result, i, PQfnumber(result, "header")));
        json_object_set_string(obj, "slug", PQgetvalue(result, i, PQfnumber(result, "slug")));
        json_object_set_string(obj, "username", PQgetvalue(result, i, PQfnumber(result, "username")));
        json_object_set_string(obj, "created_at", PQgetvalue(result, i, PQfnumber(result, "created_at")));
        json_object_set_string(obj, "updated_at", PQgetvalue(result, i, PQfnumber(result, "updated_at")));
        json_object_set_string(obj, "categories", PQgetvalue(result, i, PQfnumber(result, "categories")));
        json_object_set_string(obj, "category_slugs", PQgetvalue(result, i, PQfnumber(result, "category_slugs")));
        json_object_set_string(obj, "category_ids", PQgetvalue(result, i, PQfnumber(result, "category_ids")));

        // Add integer fields
        json_object_set_number(obj, "reading_time", atoi(PQgetvalue(result, i, PQfnumber(result, "reading_time"))));
        json_object_set_number(obj, "author_id", atoi(PQgetvalue(result, i, PQfnumber(result, "author_id"))));

        // Add boolean field
        json_object_set_bool(obj, "is_hidden", strcmp(PQgetvalue(result, i, PQfnumber(result, "is_hidden")), "t") == 0);

        json_array_add(posts, obj);
    }

    json_object_add(root, "posts", posts);
    size_t out_len = 0;
    char *out = json_serialize(root, &out_len);
    send_json(ctx->res, 200, out);

    json_free(root);
    json_serialize_free(out);
    free_ctx(ctx->pool);
}
