#include "handlers.h"
#include "db.h"
#include "connection.h"
#include "context.h"
#include "utils.h"
#include <time.h>
#include "slugify.h"
#include <stb_sprintf.h>

typedef struct
{
    mem_pool_t *pool;
    Res *res;
    char *header;
    char *content;
    char *slug;
    int reading_time;
    char *author_id;
    int created_at;
    int updated_at;
    bool is_hidden;
    int *category_ids;
    int category_count;
    char *batch_sql;
    bool response_sent;
} ctx_t;

static void on_query_post(pg_async_t *pg, PGresult *result, void *data);
static void on_post_created(pg_async_t *pg, PGresult *result, void *data);
static void insert_post_result(pg_async_t *pg, PGresult *result, void *data);

void create_post(Req *req, Res *res)
{
    auth_context_t *auth_ctx = (auth_context_t *)get_context(req);

    json_value_t *json = json_parse(req->body, req->body_len);
    if (!json)
    {
        send_text(res, 400, "Invalid JSON");
        return;
    }

    const json_value_t *jheader = json_object_get(json, "header");
    const json_value_t *jcontent = json_object_get(json, "content");
    const json_value_t *jis_hidden = json_object_get(json, "is_hidden");

    if (json_type(jheader) != JSON_STRING || json_type(jcontent) != JSON_STRING)
    {
        json_free(json);
        send_text(res, 400, "Header or content is missing");
        return;
    }

    const char *author_id = auth_ctx->id;
    const char *header = json_string(jheader);
    const char *content = json_string(jcontent);
    bool is_hidden = jis_hidden ? (int)json_number(jis_hidden) : false;

    char *slug = slugify(header, NULL);
    if (!slug)
    {
        json_free(json);
        send_text(res, 500, "Memory allocation error in slugify");
        return;
    }

    int reading_time = compute_reading_time(content);

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

    ctx->header = mem_strdup(async_pool, header);
    ctx->content = mem_strdup(async_pool, content);
    ctx->slug = mem_strdup(async_pool, slug);
    ctx->reading_time = reading_time;
    ctx->author_id = mem_strdup(async_pool, author_id);
    ctx->created_at = (int)time(NULL);
    ctx->updated_at = ctx->created_at;
    ctx->is_hidden = is_hidden;
    ctx->response_sent = false;

    free(slug);

    if (!ctx->header || !ctx->content || !ctx->slug || !ctx->author_id)
    {
        free_ctx(ctx->pool);
        json_free(json);
        send_text(res, 500, "Memory allocation failed");
        return;
    }

    // Initialize defaults for categories
    ctx->category_ids = NULL;
    ctx->category_count = 0;

    // Process categories
    const json_value_t *jcategories = json_object_get(json, "categories");
    if (jcategories && json_type(jcategories) == JSON_ARRAY)
    {
        int n = (int)json_array_size(jcategories);
        if (n > 0)
        {
            ctx->category_ids = malloc(n * sizeof(int));
            if (!ctx->category_ids)
            {
                free_ctx(ctx->pool);
                json_free(json);
                send_text(res, 500, "Memory allocation failed for categories");
                return;
            }

            ctx->category_count = 0;
            for (int i = 0; i < n; i++)
            {
                const json_value_t *item = json_array_get(jcategories, i);
                if (json_type(item) == JSON_NUMBER)
                {
                    ctx->category_ids[ctx->category_count] = (int)json_number(item);
                    ctx->category_count++;
                }
            }

            if (ctx->category_count == 0)
            {
                free(ctx->category_ids);
                ctx->category_ids = NULL;
            }
        }
    }

    json_free(json);

    pg_async_t *pg = pquv_create(db, ctx);
    if (!pg)
    {
        free_ctx(ctx->pool);
        send_text(res, 500, "Database connection error");
        return;
    }

    const char *select_sql = "SELECT 1 FROM posts WHERE slug = $1";
    const char *params[] = {ctx->slug};

    int query_result = pquv_queue(pg, select_sql, 1, params, on_query_post, ctx);
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

static void on_query_post(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("on_query_post: Invalid context\n");
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

    if (ctx->response_sent)
    {
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_TUPLES_OK)
    {
        printf("on_query_post: DB check failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Database check failed");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    if (PQntuples(result) > 0)
    {
        printf("on_query_post: Post with this slug already exists\n");
        send_text(ctx->res, 409, "This post already exists");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }
    printf("on_query_post: No duplicate found, proceeding with insert\n");

    // Prepare parameters for insert
    char reading_time_str[32], created_at_str[32], updated_at_str[32];
    char is_hidden_str[8];

    stbsp_snprintf(reading_time_str, sizeof(reading_time_str), "%d", ctx->reading_time);
    stbsp_snprintf(created_at_str, sizeof(created_at_str), "%d", ctx->created_at);
    stbsp_snprintf(updated_at_str, sizeof(updated_at_str), "%d", ctx->updated_at);
    stbsp_snprintf(is_hidden_str, sizeof(is_hidden_str), "%s", ctx->is_hidden ? "true" : "false");

    const char *insert_params[8] = {
        ctx->header,
        ctx->slug,
        ctx->content,
        reading_time_str,
        ctx->author_id,
        created_at_str,
        updated_at_str,
        is_hidden_str};

    const char *insert_sql =
        "INSERT INTO posts "
        "(header, slug, content, reading_time, author_id, created_at, updated_at, is_hidden) "
        "VALUES ($1, $2, $3, $4, $5, to_timestamp($6), to_timestamp($7), $8) "
        "RETURNING id; ";

    if (pquv_queue(pg, insert_sql, 8, insert_params, on_post_created, ctx) != 0)
    {
        if (!ctx->response_sent)
        {
            send_text(ctx->res, 500, "Failed to queue insert query");
            ctx->response_sent = true;
        }
        free_ctx(ctx->pool);
        return;
    }

    printf("on_query_post: Insert operation queued\n");
}

static void on_post_created(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        if (ctx)
            free_ctx(ctx->pool);
        return;
    }

    if (ctx->response_sent)
    {
        return;
    }

    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK)
    {
        printf("on_post_created: Post insert failed\n");
        send_text(ctx->res, 500, "DB insert failed");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    int post_id = atoi(PQgetvalue(result, 0, 0));

    if (ctx->category_count == 0)
    {
        send_text(ctx->res, 201, "Post created successfully");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    size_t sql_len = 256 + (ctx->category_count * 32);
    char *batch_sql = malloc(sql_len);
    ctx->batch_sql = batch_sql;

    if (!batch_sql)
    {
        if (!ctx->response_sent)
        {
            send_text(ctx->res, 500, "Memory allocation failed");
            ctx->response_sent = true;
        }
        free_ctx(ctx->pool);
        return;
    }

    strcpy(batch_sql, "INSERT INTO post_categories (post_id, category_id) VALUES ");

    char temp_values[64];
    for (int i = 0; i < ctx->category_count; i++)
    {
        if (i > 0)
        {
            strcat(batch_sql, ", ");
        }
        stbsp_snprintf(temp_values, sizeof(temp_values), "(%d, %d)", post_id, ctx->category_ids[i]);
        strcat(batch_sql, temp_values);
    }
    strcat(batch_sql, " ON CONFLICT DO NOTHING;");

    printf("on_post_created: Executing batch SQL: %s\n", batch_sql);

    if (pquv_queue(pg, batch_sql, 0, NULL, insert_post_result, ctx) != 0)
    {
        if (!ctx->response_sent)
        {
            send_text(ctx->res, 500, "Failed to queue batch category insert");
            ctx->response_sent = true;
        }
        free_ctx(ctx->pool);
        return;
    }

    printf("on_post_created: Batch category insert queued for %d categories\n", ctx->category_count);
}

static void insert_post_result(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("insert_post_result: Invalid context\n");
        if (ctx)
            free_ctx(ctx->pool);
        return;
    }

    if (ctx->response_sent)
    {
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_COMMAND_OK)
    {
        printf("insert_post_result: Batch category insert failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Category insert failed");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    printf("insert_post_result: Post created successfully with all categories\n");
    send_text(ctx->res, 201, "Post created successfully");
    ctx->response_sent = true;
    free_ctx(ctx->pool);
}
