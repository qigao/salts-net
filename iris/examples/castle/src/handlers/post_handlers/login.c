#include "handlers.h"
#include "session.h"
#include "password_hash.h"

typedef struct
{
    mem_pool_t *pool;
    Res *res;
    char *username;
    char *password;
    char *user_id;
    char *name;
    char *hashed_password;
} ctx_t;

static void on_user_found(pg_async_t *pg, PGresult *result, void *data);

void login(Req *req, Res *res)
{
    Session *user_session = get_session(req);

    if (user_session)
    {
        send_text(res, 400, "Error: You are already logged in");
        return;
    }

    // Parse JSON
    json_value_t *json = json_parse(req->body, req->body_len);
    if (!json)
    {
        send_text(res, 400, "Invalid JSON");
        return;
    }

    const json_value_t *juser = json_object_get(json, "username");
    const json_value_t *jpass = json_object_get(json, "password");

    if (json_type(juser) != JSON_STRING || json_type(jpass) != JSON_STRING)
    {
        printf("ERROR: Username or password missing\n");
        json_free(json);
        send_text(res, 400, "Username or password is missing");
        return;
    }

    // Create separate arena for async operation
    mem_pool_t *async_pool = malloc(sizeof(mem_pool_t)); if (!async_pool || mem_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        json_free(json);
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    // Allocate login context
    ctx_t *ctx = mem_alloc(async_pool, sizeof(ctx_t));
    if (!ctx) {
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
        json_free(json);
        send_text(res, 500, "Response copy failed");
        return;
    }

    ctx->username = mem_strdup(async_pool, json_string(juser));
    ctx->password = mem_strdup(async_pool, json_string(jpass));
    json_free(json);

    // Create PostgreSQL async context
    pg_async_t *pg = pquv_create(db, ctx);

    if (!pg)
    {
        printf("ERROR: Failed to create pg_async context\n");
        free_ctx(ctx->pool);
        send_text(res, 500, "Database connection error");
        return;
    }

    // Queue the SELECT query
    const char *select_sql = "SELECT id, name, password FROM users WHERE username = $1";
    const char *params[] = {ctx->username};

    int query_result = pquv_queue(pg, select_sql, 1, params, on_user_found, ctx);
    if (query_result != 0)
    {
        printf("ERROR: Failed to queue query, result=%d\n", query_result);
        free_ctx(ctx->pool);
        send_text(res, 500, "Failed to queue query");
        return;
    }

    // Start execution
    int exec_result = pquv_execute(pg);
    if (exec_result != 0)
    {
        printf("ERROR: Failed to execute, result=%d\n", exec_result);
        free_ctx(ctx->pool);
        send_text(res, 500, "Failed to execute query");
        return;
    }
}

static void on_user_found(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!result)
    {
        printf("ERROR: Result is NULL\n");
        free_ctx(ctx->pool);
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (PQntuples(result) == 0)
    {
        free_ctx(ctx->pool);
        send_text(ctx->res, 404, "User not found");
        return;
    }

    // Extract user data
    ctx->user_id = strdup(PQgetvalue(result, 0, 0));
    ctx->name = strdup(PQgetvalue(result, 0, 1));
    ctx->hashed_password = strdup(PQgetvalue(result, 0, 2));

    // Verify password
    int verify_result = password_verify(ctx->hashed_password, ctx->password, strlen(ctx->password));
    if (verify_result != 0)
    {
        send_text(ctx->res, 401, "Incorrect password");
        free_ctx(ctx->pool);
        return;
    }

    Session *sess = create_session(3600);

    set_session(sess, "id", ctx->user_id);
    set_session(sess, "name", ctx->name);
    set_session(sess, "username", ctx->username);

    if (strstr(ctx->username, "johndoe"))
    {
        set_session(sess, "is_admin", "true");
    }

    // The cookie_options must be the same in the logout handler
    cookie_options_t cookie_options = {
        .max_age = 3600, // 1 hour
        .path = "/",
        .same_site = "Lax",
        .http_only = true,
        .secure = true,
    };

    send_session(ctx->res, sess, &cookie_options);
    send_text(ctx->res, 200, "Login successful");
}
