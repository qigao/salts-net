#include "handlers.h"

// Callback structure to hold request/response context
typedef struct
{
    mem_pool_t *pool;
    Res *res;
} ctx_t;

static void users_result_callback(pg_async_t *pg, PGresult *result, void *data);

// Async version of get_all_users
void get_all_users_async(Req *req, Res *res)
{
    const char *sql = "SELECT id, name, username FROM users;";

    mem_pool_t *async_pool = malloc(sizeof(mem_pool_t)); if (!async_pool || mem_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    // Create context to pass to callback
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

    // Create async PostgreSQL context
    pg_async_t *pg = pquv_create(db, ctx);
    if (!pg)
    {
        printf("get_all_users_async: Failed to create pg_async context\n");
        send_text(res, 500, "Failed to create async context");
        free_ctx(ctx->pool);
        return;
    }

    // Queue the query
    int result = pquv_queue(pg, sql, 0, NULL, users_result_callback, ctx);
    if (result != 0)
    {
        printf("get_all_users_async: Failed to queue query\n");
        send_text(res, 500, "Failed to queue query");
        free_ctx(ctx->pool);
        return;
    }

    // Start execution (this will return immediately)
    result = pquv_execute(pg);
    if (result != 0)
    {
        printf("get_all_users_async: Failed to execute query\n");
        send_text(res, 500, "Failed to execute query");
        free_ctx(ctx->pool);
        return;
    }

    // Function returns here, callback will be called when query completes
}

// Callback function that processes the query result
static void users_result_callback(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("users_result_callback: Invalid context\n");
        return;
    }

    // Check result status
    ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_TUPLES_OK)
    {
        printf("users_result_callback: Query failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "DB select failed");
        free_ctx(ctx->pool);
        return;
    }

    int rows = PQntuples(result);
    json_value_t *json_array = json_create_array();

    for (int i = 0; i < rows; i++)
    {
        int id = atoi(PQgetvalue(result, i, 0));
        const char *name = PQgetvalue(result, i, 1);
        const char *username = PQgetvalue(result, i, 2);

        json_value_t *user_json = json_create_object();
        json_object_set_number(user_json, "id", id);
        json_object_set_string(user_json, "name", name);
        json_object_set_string(user_json, "username", username);

        json_array_add(json_array, user_json);
    }

    size_t json_string_len = 0;
    char *json_string = json_serialize(json_array, &json_string_len);
    send_json(ctx->res, 200, json_string);

    // Cleanup
    json_free(json_array);
    json_serialize_free(json_string);
    free_ctx(ctx->pool);

    printf("users_result_callback: Response sent successfully\n");
}
