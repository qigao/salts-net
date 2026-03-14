#include "handlers.h"
#include "context.h"

typedef struct
{
    mem_pool_t *pool;
    Res *res;
} ctx_t;

static void on_post_deleted(pg_async_t *pg, PGresult *result, void *data);

void del_post(Req *req, Res *res)
{
    const char *post_slug = get_params(req, "post");

    auth_context_t *auth_ctx = (auth_context_t *)get_context(req);

    if (!auth_ctx || !auth_ctx->is_author)
    {
        send_text(res, 401, "Not allowed");
        return;
    }

    // Create separate arena for async operation
    mem_pool_t *async_pool = malloc(sizeof(mem_pool_t)); if (!async_pool || mem_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    // Allocate login context
    ctx_t *ctx = mem_alloc(async_pool, sizeof(ctx_t));
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

    pg_async_t *pg = pquv_create(db, ctx);
    if (!pg)
    {
        free_ctx(ctx->pool);
        send_text(res, 500, "Database connection error");
        return;
    }

    const char *delete_sql =
        "DELETE FROM posts p "
        "WHERE p.author_id = $1 "
        "AND p.slug     = $2;";

    const char *params[] = {auth_ctx->id, post_slug};

    int qr = pquv_queue(pg, delete_sql, 2, params, on_post_deleted, ctx);
    if (qr != 0)
    {
        printf("ERROR: Failed to queue delete, result=%d\n", qr);
        free_ctx(ctx->pool);
        send_text(res, 500, "Failed to queue delete");
        return;
    }

    if (pquv_execute(pg) != 0)
    {
        printf("ERROR: Failed to execute delete\n");
        send_text(res, 500, "Failed to execute delete");
        free_ctx(ctx->pool);
        return;
    }
}

static void on_post_deleted(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("Invalid context\n");
        send_text(ctx->res, 400, "Invalid context");
        if (ctx)
            free_ctx(ctx->pool);
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_COMMAND_OK)
    {
        printf("Post could not be deleted: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Post could not be deleted");
        free_ctx(ctx->pool);
        return;
    }

    if (PQcmdTuples(result) == 0)
    {
        send_text(ctx->res, 404, "Post not found");
        free_ctx(ctx->pool);
        return;
    }

    send_text(ctx->res, 200, "Post deleted successfully");
    free_ctx(ctx->pool);
}
