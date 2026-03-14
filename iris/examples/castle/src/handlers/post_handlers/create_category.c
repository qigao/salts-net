#include "handlers.h"
#include "db.h"
#include "connection.h"
#include "context.h"
#include "utils.h"
#include <time.h>
#include "slugify.h"

typedef struct
{
    mem_pool_t *pool;
    Res *res;
    char *category;
    char *slug;
    char *author_id;
} ctx_t;

static void on_category_insert(pg_async_t *pg, PGresult *result, void *data);

void create_category(Req *req, Res *res)
{
    auth_context_t *auth_ctx = (auth_context_t *)get_context(req);

    json_value_t *json = json_parse(req->body, req->body_len);
    if (!json)
    {
        send_text(res, 400, "Invalid JSON");
        return;
    }

    const json_value_t *jcategory = json_object_get(json, "category");

    if (json_type(jcategory) != JSON_STRING)
    {
        json_free(json);
        send_text(res, 400, "Category field is missing");
        return;
    }

    const char *author_id = auth_ctx->id;
    const char *category = json_string(jcategory);

    char *slug = slugify(category, NULL);

    // Create separate arena for async operation
    mem_pool_t *async_pool = malloc(sizeof(mem_pool_t)); if (!async_pool || mem_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        free(slug);
        json_free(json);
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    // Allocate async context in the async arena
    ctx_t *ctx = mem_alloc(async_pool, sizeof(ctx_t));
    if (!ctx) {
        free(slug);
        json_free(json);
        send_text(res, 500, "Context allocation failed");
        return;
    }

    // Store arena reference
    ctx->pool = async_pool;

    ctx->res = arena_copy_res(async_pool, res);
    if (!ctx->res)
    {
        free_ctx(ctx->pool);
        free(slug);
        json_free(json);
        send_text(res, 500, "Response copy failed");
        return;
    }

    ctx->category = mem_strdup(async_pool, category);
    ctx->slug = mem_strdup(async_pool, slug);
    ctx->author_id = mem_strdup(async_pool, author_id);

    free(slug);
    json_free(json);

    if (!ctx->category || !ctx->slug || !ctx->author_id)
    {
        free_ctx(ctx->pool);
        send_text(res, 500, "Memory allocation failed");
        return;
    }

    pg_async_t *pg = pquv_create(db, ctx);
    if (!pg)
    {
        free_ctx(ctx->pool);
        send_text(res, 500, "Database connection error");
        return;
    }

    const char *conditional_insert_sql =
        "INSERT INTO categories (category, slug, author_id) "
        "SELECT $1, $2, $3 "
        "WHERE NOT EXISTS ("
        "    SELECT 1 FROM categories WHERE slug = $2"
        ");";

    const char *params[] = {ctx->category, ctx->slug, ctx->author_id};

    int query_result = pquv_queue(pg, conditional_insert_sql, 3, params, on_category_insert, ctx);
    if (query_result != 0)
    {
        printf("ERROR: Failed to queue query, result=%d\n", query_result);
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

    printf("create_category: Conditional INSERT query started\n");
}

static void on_category_insert(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("on_category_insert: Invalid context\n");
        if (ctx)
            free_ctx(ctx->pool);
        return;
    }

    if (!result)
    {
        printf("ERROR: Result is NULL\n");
        send_text(ctx->res, 500, "Database error");
        free_ctx(ctx->pool);
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_COMMAND_OK)
    {
        printf("on_category_insert: DB operation failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Database operation failed");
        free_ctx(ctx->pool);
        return;
    }

    // Check how many rows were affected
    char *affected_rows = PQcmdTuples(result);
    int rows_inserted = atoi(affected_rows);

    if (rows_inserted == 0)
    {
        // No rows inserted = category already exists
        printf("on_category_insert: Category '%s' already exists\n", ctx->category);
        send_text(ctx->res, 409, "This category already exists");
    }
    else if (rows_inserted == 1)
    {
        // One row inserted = success
        printf("on_category_insert: Category '%s' created successfully\n", ctx->category);
        send_text(ctx->res, 201, "Category created!");
    }
    else
    {
        // Unexpected result
        printf("on_category_insert: Unexpected result: %d rows affected\n", rows_inserted);
        send_text(ctx->res, 500, "Unexpected database result");
    }

    free_ctx(ctx->pool);
}