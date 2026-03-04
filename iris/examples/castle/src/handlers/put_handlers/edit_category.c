#include "handlers.h"
#include "db.h"
#include "connection.h"
#include "context.h"
#include "utils.h"
#include <time.h>
#include "slugify.h"

typedef struct
{
    turbo_pool_t *pool;
    Res *res;
    char *category;
    char *original_slug;
    char *new_slug;
    char *author_id;
} ctx_t;

static void on_query_category(pg_async_t *pg, PGresult *result, void *data);
static void on_check_new_slug(pg_async_t *pg, PGresult *result, void *data);
static void update_category(pg_async_t *pg, ctx_t *ctx);
static void on_category_updated(pg_async_t *pg, PGresult *result, void *data);

void edit_category(Req *req, Res *res)
{
    auth_context_t *auth_ctx = (auth_context_t *)get_context(req);

    if (!auth_ctx || !auth_ctx->is_author)
    {
        send_text(res, 401, "Not allowed");
        return;
    }

    const char *slug = get_params(req, "category");
    if (!slug)
    {
        send_text(res, 400, "Category slug is required");
        return;
    }

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

    char *new_slug = slugify(category, NULL);
    if (!new_slug)
    {
        json_free(json);
        send_text(res, 500, "Memory allocation error in slugify");
        return;
    }

    // Create separate arena for async operation
    turbo_pool_t *async_pool = malloc(sizeof(turbo_pool_t)); if (!async_pool || turbo_pool_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        json_free(json);
        free(new_slug);
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    // Create context to hold all the data for async operation
    ctx_t *ctx = turbo_pool_alloc(async_pool, sizeof(ctx_t));
    if (!ctx) {
        json_free(json);
        free(new_slug);
        send_text(res, 500, "Context allocation failed");
        return;
    }

    // Store arena reference
    ctx->pool = async_pool;

    ctx->res = arena_copy_res(async_pool, res);
    if (!ctx->res)
    {
        free_ctx(ctx->pool);
        json_free(json);
        free(new_slug);
        send_text(res, 500, "Response copy failed");
        return;
    }

    ctx->category = turbo_pool_strdup(async_pool, category);
    ctx->original_slug = turbo_pool_strdup(async_pool, slug);
    ctx->new_slug = turbo_pool_strdup(async_pool, new_slug);
    ctx->author_id = turbo_pool_strdup(async_pool, author_id);

    free(new_slug);

    if (!ctx->category || !ctx->author_id)
    {
        free_ctx(ctx->pool);
        json_free(json);
        send_text(res, 500, "Memory allocation failed");
        return;
    }

    json_free(json);

    pg_async_t *pg = pquv_create(db, ctx);
    if (!pg)
    {
        free_ctx(ctx->pool);
        send_text(res, 500, "Database connection error");
        return;
    }

    const char *select_sql = "SELECT id, author_id FROM categories WHERE slug = $1";
    const char *params[] = {ctx->original_slug};

    int query_result = pquv_queue(pg, select_sql, 1, params, on_query_category, ctx);
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
}

static void on_query_category(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("on_query_category: Invalid context\n");
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

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_TUPLES_OK)
    {
        printf("on_query_category: DB check failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Database check failed");
        free_ctx(ctx->pool);
        return;
    }

    if (PQntuples(result) == 0)
    {
        printf("on_query_category: Category not found\n");
        send_text(ctx->res, 404, "Category not found");
        free_ctx(ctx->pool);
        return;
    }

    if (strcmp(ctx->original_slug, ctx->new_slug) != 0)
    {
        const char *check_slug_sql = "SELECT 1 FROM categories WHERE slug = $1 AND slug != $2";
        const char *check_params[] = {ctx->new_slug, ctx->original_slug};

        if (pquv_queue(pg, check_slug_sql, 2, check_params, on_check_new_slug, ctx) != 0)
        {
            send_text(ctx->res, 500, "Failed to queue slug check query");
            free_ctx(ctx->pool);
            return;
        }
    }
    else
    {
        // If slug hasn't change, continue to update directly
        update_category(pg, ctx);
    }
}

static void on_check_new_slug(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("on_check_new_slug: Invalid context\n");
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

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_TUPLES_OK)
    {
        printf("on_check_new_slug: DB check failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Database check failed");
        free_ctx(ctx->pool);
        return;
    }

    if (PQntuples(result) > 0)
    {
        printf("on_check_new_slug: New slug already exists\n");
        send_text(ctx->res, 409, "A category with this title already exists");
        free_ctx(ctx->pool);
        return;
    }

    update_category(pg, ctx);
}

static void update_category(pg_async_t *pg, ctx_t *ctx)
{
    const char *update_params[3] = {
        ctx->category,
        ctx->new_slug,
        ctx->original_slug // for WHERE condition
    };

    const char *update_sql =
        "UPDATE categories SET "
        "category = $1, "
        "slug = $2 "
        "WHERE slug = $3 "
        "RETURNING id;";

    if (pquv_queue(pg, update_sql, 3, update_params, on_category_updated, ctx) != 0)
    {
        send_text(ctx->res, 500, "Failed to queue update query");
        free_ctx(ctx->pool);
        return;
    }
}

static void on_category_updated(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("on_category_updated: Invalid context\n");
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

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_TUPLES_OK)
    {
        printf("on_category_updated: Update failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Category update failed");
        free_ctx(ctx->pool);
        return;
    }

    if (PQntuples(result) == 0)
    {
        printf("on_category_updated: No rows affected\n");
        send_text(ctx->res, 404, "Category not found or not updated");
        free_ctx(ctx->pool);
        return;
    }

    send_text(ctx->res, 200, "Category updated successfully");
    free_ctx(ctx->pool);
}
