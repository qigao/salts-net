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
    turbo_arena_t *pool;
    Res *res;
    char *header;
    char *content;
    char *original_slug;
    char *new_slug;
    int reading_time;
    char *author_id;
    int created_at;
    int updated_at;
    bool is_hidden;
    int *category_ids;
    int category_count;
    int completed_categories;
    char *batch_sql;
    bool response_sent;
} ctx_t;

static void on_query_post_exists(pg_async_t *pg, PGresult *result, void *data);
static void on_check_new_slug(pg_async_t *pg, PGresult *result, void *data);
static void update_post(pg_async_t *pg, ctx_t *ctx);
static void on_post_updated(pg_async_t *pg, PGresult *result, void *data);
static void clear_post_categories(pg_async_t *pg, ctx_t *ctx, const char *post_id);
static void update_post_categories(pg_async_t *pg, ctx_t *ctx, const char *post_id);
static void on_old_categories_deleted(pg_async_t *pg, PGresult *result, void *data);
static void insert_new_categories(pg_async_t *pg, ctx_t *ctx);
static void on_categories_cleared(pg_async_t *pg, PGresult *result, void *data);
static void on_categories_inserted(pg_async_t *pg, PGresult *result, void *data);

void edit_post(Req *req, Res *res)
{
    auth_context_t *auth_ctx = (auth_context_t *)get_context(req);

    if (!auth_ctx || !auth_ctx->is_author)
    {
        send_text(res, 401, "Not allowed");
        return;
    }

    const char *slug = get_params(req, "post");
    if (!slug)
    {
        send_text(res, 400, "Post slug is required");
        return;
    }

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

    const char *header = json_string(jheader);
    const char *content = json_string(jcontent);
    bool is_hidden = jis_hidden ? (int)json_number(jis_hidden) : false;

    char *new_slug = slugify(header, NULL);
    if (!new_slug)
    {
        json_free(json);
        send_text(res, 500, "Memory allocation error in slugify");
        return;
    }

    int reading_time = compute_reading_time(content);

    // Create separate arena for async operation
    turbo_arena_t *async_pool = malloc(sizeof(turbo_arena_t)); if (!async_pool || turbo_arena_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        json_free(json);
        free(new_slug);
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    // Create context to hold all the data for async operation
    ctx_t *ctx = turbo_arena_alloc(async_pool, sizeof(ctx_t));
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

    Res *copy = copy_res(res);
    ctx->res = copy;
    ctx->header = turbo_arena_strdup(async_pool, header);
    ctx->content = strdup(content);
    ctx->original_slug = turbo_arena_strdup(async_pool, slug);
    ctx->new_slug = turbo_arena_strdup(async_pool, new_slug);
    ctx->reading_time = reading_time;
    ctx->updated_at = (int)time(NULL);
    ctx->is_hidden = is_hidden;
    ctx->response_sent = false;
    ctx->author_id = turbo_arena_strdup(async_pool, auth_ctx->id);

    free(new_slug);

    if (!ctx->header || !ctx->content || !ctx->original_slug || !ctx->new_slug || !ctx->author_id)
    {
        free_ctx(ctx->pool);
        json_free(json);
        send_text(res, 500, "Memory allocation failed");
        return;
    }

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

    // First, check if the post exists and the user has it
    const char *select_sql = "SELECT id, author_id FROM posts WHERE slug = $1";
    const char *params[] = {ctx->original_slug};

    int query_result = pquv_queue(pg, select_sql, 1, params, on_query_post_exists, ctx);
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

static void on_query_post_exists(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("on_query_post_exists: Invalid context\n");
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
        printf("on_query_post_exists: DB check failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Database check failed");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    if (PQntuples(result) == 0)
    {
        printf("on_query_post_exists: Post not found\n");
        send_text(ctx->res, 404, "Post not found");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    // Check if the post belongs to the user
    const char *post_author_id = PQgetvalue(result, 0, 1);
    if (strcmp(post_author_id, ctx->author_id) != 0)
    {
        printf("on_query_post_exists: User is not the author of this post\n");
        send_text(ctx->res, 403, "You can only edit your own posts");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    // If slug has change, check if the new slug has been occupied
    if (strcmp(ctx->original_slug, ctx->new_slug) != 0)
    {
        const char *check_slug_sql = "SELECT 1 FROM posts WHERE slug = $1 AND slug != $2";
        const char *check_params[] = {ctx->new_slug, ctx->original_slug};

        if (pquv_queue(pg, check_slug_sql, 2, check_params, on_check_new_slug, ctx) != 0)
        {
            if (!ctx->response_sent)
            {
                send_text(ctx->res, 500, "Failed to queue slug check query");
                ctx->response_sent = true;
            }
            free_ctx(ctx->pool);
            return;
        }
    }
    else
    {
        // If slug hasn't change, continue to update directly
        update_post(pg, ctx);
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

    if (ctx->response_sent)
    {
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_TUPLES_OK)
    {
        printf("on_check_new_slug: DB check failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Database check failed");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    if (PQntuples(result) > 0)
    {
        printf("on_check_new_slug: New slug already exists\n");
        send_text(ctx->res, 409, "A post with this title already exists");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    update_post(pg, ctx);
}

static void update_post(pg_async_t *pg, ctx_t *ctx)
{
    // Prepare parameters for update
    char reading_time_str[32], updated_at_str[32];
    char is_hidden_str[8];

    stbsp_snprintf(reading_time_str, sizeof(reading_time_str), "%d", ctx->reading_time);
    stbsp_snprintf(updated_at_str, sizeof(updated_at_str), "%d", ctx->updated_at);
    stbsp_snprintf(is_hidden_str, sizeof(is_hidden_str), "%s", ctx->is_hidden ? "true" : "false");

    const char *update_params[7] = {
        ctx->header,
        ctx->new_slug,
        ctx->content,
        reading_time_str,
        updated_at_str,
        is_hidden_str,
        ctx->original_slug // for WHERE condition
    };

    const char *update_sql =
        "UPDATE posts SET "
        "header = $1, "
        "slug = $2, "
        "content = $3, "
        "reading_time = $4, "
        "updated_at = to_timestamp($5), "
        "is_hidden = $6 "
        "WHERE slug = $7 "
        "RETURNING id;";

    if (pquv_queue(pg, update_sql, 7, update_params, on_post_updated, ctx) != 0)
    {
        if (!ctx->response_sent)
        {
            send_text(ctx->res, 500, "Failed to queue update query");
            ctx->response_sent = true;
        }
        free_ctx(ctx->pool);
        return;
    }
}

static void on_post_updated(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("on_post_updated: Invalid context\n");
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
        free_ctx(ctx->pool);
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_TUPLES_OK)
    {
        printf("on_post_updated: Update failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Post update failed");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    if (PQntuples(result) == 0)
    {
        printf("on_post_updated: No rows affected\n");
        send_text(ctx->res, 404, "Post not found or not updated");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    // If referenced with categories, update them
    if (ctx->category_ids && ctx->category_count > 0)
    {
        const char *post_id = PQgetvalue(result, 0, 0);
        update_post_categories(pg, ctx, post_id);
    }
    else
    {
        // If no categories sent, delete the existing categories and create new ones
        const char *post_id = PQgetvalue(result, 0, 0);
        clear_post_categories(pg, ctx, post_id);
    }
}

static void clear_post_categories(pg_async_t *pg, ctx_t *ctx, const char *post_id)
{
    const char *delete_sql = "DELETE FROM post_categories WHERE post_id = $1";
    const char *delete_params[] = {post_id};

    if (pquv_queue(pg, delete_sql, 1, delete_params, on_categories_cleared, ctx) != 0)
    {
        if (!ctx->response_sent)
        {
            send_text(ctx->res, 500, "Failed to queue category deletion");
            ctx->response_sent = true;
        }
        free_ctx(ctx->pool);
        return;
    }
}

static void update_post_categories(pg_async_t *pg, ctx_t *ctx, const char *post_id)
{
    const char *delete_sql = "DELETE FROM post_categories WHERE post_id = $1";
    const char *delete_params[] = {post_id};

    ctx->author_id = realloc(ctx->author_id, strlen(post_id) + 1);
    strcpy(ctx->author_id, post_id);

    if (pquv_queue(pg, delete_sql, 1, delete_params, on_old_categories_deleted, ctx) != 0)
    {
        if (!ctx->response_sent)
        {
            send_text(ctx->res, 500, "Failed to queue category deletion");
            ctx->response_sent = true;
        }
        free_ctx(ctx->pool);
        return;
    }
}

static void on_old_categories_deleted(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res || ctx->response_sent)
    {
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
    if (status != PGRES_COMMAND_OK)
    {
        printf("on_old_categories_deleted: Delete failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Failed to delete old categories");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    insert_new_categories(pg, ctx);
}

static void insert_new_categories(pg_async_t *pg, ctx_t *ctx)
{
    if (!ctx->category_ids || ctx->category_count == 0)
    {
        send_text(ctx->res, 200, "Post updated successfully");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    size_t sql_size = 256 + (ctx->category_count * 32);
    ctx->batch_sql = malloc(sql_size);
    if (!ctx->batch_sql)
    {
        send_text(ctx->res, 500, "Memory allocation failed");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    strcpy(ctx->batch_sql, "INSERT INTO post_categories (post_id, category_id) VALUES ");

    char **batch_params = malloc(ctx->category_count * 2 * sizeof(char *));
    if (!batch_params)
    {
        send_text(ctx->res, 500, "Memory allocation failed");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    for (int i = 0; i < ctx->category_count; i++)
    {
        char *category_str = malloc(16);
        if (!category_str)
        {
            for (int j = 0; j < i * 2; j++)
                free(batch_params[j]);
            free(batch_params);
            send_text(ctx->res, 500, "Memory allocation failed");
            ctx->response_sent = true;
            free_ctx(ctx->pool);
            return;
        }

        stbsp_snprintf(category_str, 16, "%d", ctx->category_ids[i]);

        batch_params[i * 2] = strdup(ctx->author_id);
        batch_params[i * 2 + 1] = category_str;

        if (i > 0)
            strcat(ctx->batch_sql, ", ");

        char value_part[32];
        stbsp_snprintf(value_part, sizeof(value_part), "($%d, $%d)", i * 2 + 1, i * 2 + 2);
        strcat(ctx->batch_sql, value_part);
    }

    if (pquv_queue(pg, ctx->batch_sql, ctx->category_count * 2, (const char **)batch_params, on_categories_inserted, ctx) != 0)
    {
        for (int i = 0; i < ctx->category_count * 2; i++)
            free(batch_params[i]);
        free(batch_params);

        if (!ctx->response_sent)
        {
            send_text(ctx->res, 500, "Failed to queue category insert");
            ctx->response_sent = true;
        }
        free_ctx(ctx->pool);
        return;
    }

    // Clean up batch_params
    for (int i = 0; i < ctx->category_count * 2; i++)
        free(batch_params[i]);
    free(batch_params);
}

static void on_categories_cleared(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res || ctx->response_sent)
    {
        if (ctx)
            free_ctx(ctx->pool);
        return;
    }

    send_text(ctx->res, 200, "Post updated successfully");
    ctx->response_sent = true;
    free_ctx(ctx->pool);
}

static void on_categories_inserted(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res || ctx->response_sent)
    {
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
    if (status != PGRES_COMMAND_OK)
    {
        printf("on_categories_inserted: Insert failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Failed to insert categories");
        ctx->response_sent = true;
        free_ctx(ctx->pool);
        return;
    }

    send_text(ctx->res, 200, "Post updated successfully");
    ctx->response_sent = true;
    free_ctx(ctx->pool);
}
