#include "handlers.h"
#include "session.h"
#include "password_hash.h"

// Structure to hold request context for async operations
typedef struct
{
    turbo_arena_t *pool;
    Res *res;
    char *name;
    char *username;
    char *password;
    char *email;
    char *about;
    char *hashpw;
} ctx_t;

// Callback functions
static void check_user_exists(pg_async_t *pg, PGresult *result, void *data);
static void add_user_result(pg_async_t *pg, PGresult *result, void *data);

void add_user(Req *req, Res *res)
{
    cJSON *json = cJSON_Parse(req->body);
    if (!json)
    {
        send_text(res, 400, "Invalid JSON");
        return;
    }

    cJSON *j_name = cJSON_GetObjectItem(json, "name");
    cJSON *j_username = cJSON_GetObjectItem(json, "username");
    cJSON *j_password = cJSON_GetObjectItem(json, "password");
    cJSON *j_email = cJSON_GetObjectItem(json, "email");
    cJSON *j_about = cJSON_GetObjectItem(json, "about");

    if (!cJSON_IsString(j_name) ||
        !cJSON_IsString(j_username) ||
        !cJSON_IsString(j_password) ||
        !cJSON_IsString(j_email))
    {
        cJSON_Delete(json);
        send_text(res, 400, "Missing or invalid fields");
        return;
    }

    const char *name = j_name->valuestring;
    const char *username = j_username->valuestring;
    const char *password = j_password->valuestring;
    const char *email = j_email->valuestring;
    const char *about = cJSON_IsString(j_about) ? j_about->valuestring : "";

    // Create separate arena for async operation
    turbo_arena_t *async_pool = malloc(sizeof(turbo_arena_t)); if (!async_pool || turbo_arena_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        cJSON_Delete(json);
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    // Create context to hold all the data for async operation
    ctx_t *ctx = turbo_arena_alloc(async_pool, sizeof(ctx_t));
    if (!ctx) {
        cJSON_Delete(json);
        send_text(res, 500, "Context allocation failed");
        return;
    }

    // Store arena reference
    ctx->pool = async_pool;

    ctx->res = arena_copy_res(async_pool, res);
    if (!ctx->res)
    {
        free_ctx(ctx->pool);;
        cJSON_Delete(json);
        send_text(res, 500, "Response copy failed");
        return;
    }

    // Copy all strings to context (they need to persist after this function returns)
    ctx->name = turbo_arena_strdup(async_pool, name);
    ctx->username = turbo_arena_strdup(async_pool, username);
    ctx->password = turbo_arena_strdup(async_pool, password);
    ctx->email = turbo_arena_strdup(async_pool, email);
    ctx->about = turbo_arena_strdup(async_pool, about);

    if (!ctx->name || !ctx->username || !ctx->password || !ctx->email || !ctx->about)
    {
        free_ctx(ctx->pool);;
        cJSON_Delete(json);
        send_text(res, 500, "Memory allocation failed");
        return;
    }

    // Hash the password
    ctx->hashpw = malloc(PASSWORD_HASH_STRBYTES);
    if (!ctx->hashpw)
    {
        free_ctx(ctx->pool);;
        cJSON_Delete(json);
        send_text(res, 500, "Memory allocation failed");
        return;
    }

    if (password_hash(ctx->hashpw, password, strlen(password)) != 0)
    {
        free_ctx(ctx->pool);;
        cJSON_Delete(json);
        send_text(res, 500, "Password hashing failed");
        return;
    }

    cJSON_Delete(json);

    // Create async PostgreSQL context
    pg_async_t *pg = pquv_create(db, ctx);
    if (!pg)
    {
        free_ctx(ctx->pool);;
        send_text(res, 500, "Failed to create async DB context");
        return;
    }

    // First check if username or email already exists
    const char *check_sql =
        "SELECT COUNT(*) FROM users WHERE username = $1 OR email = $2;";

    // Prepare parameters array for checking duplicates
    const char *check_params[2] = {
        ctx->username,
        ctx->email};

    // Queue the async check query
    if (pquv_queue(pg, check_sql, 2, check_params, check_user_exists, ctx) != 0)
    {
        free_ctx(ctx->pool);;
        send_text(res, 500, "Failed to queue database query");
        return;
    }

    // Start execution - this will return immediately and execute asynchronously
    if (pquv_execute(pg) != 0)
    {
        free_ctx(ctx->pool);;
        send_text(res, 500, "Failed to execute database query");
        return;
    }

    // Function returns immediately - the callback will handle the response
    printf("add_user: Async duplicate check started\n");
}

// Callback function that handles the duplicate check result
static void check_user_exists(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("check_user_exists: Invalid context\n");
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (status != PGRES_TUPLES_OK)
    {
        printf("check_user_exists: DB check failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "Database check failed");
        free_ctx(ctx->pool);;
        return;
    }

    // Get the count result
    char *count_str = PQgetvalue(result, 0, 0);
    int count = atoi(count_str);

    if (count > 0)
    {
        printf("check_user_exists: Username or email already exists\n");
        send_text(ctx->res, 409, "Username or email already exists");
        free_ctx(ctx->pool);;
        return;
    }

    printf("check_user_exists: No duplicates found, proceeding with insert\n");

    // No duplicates found, proceed with insert using the same pg context
    // Prepare the SQL insert query
    const char *insert_sql =
        "INSERT INTO users "
        "(name, username, password, email, about) "
        "VALUES ($1, $2, $3, $4, $5);";

    // Prepare parameters array for insert
    const char *insert_params[5] = {
        ctx->name,
        ctx->username,
        ctx->hashpw,
        ctx->email,
        ctx->about};

    // Queue the async insert query using the same pg context
    if (pquv_queue(pg, insert_sql, 5, insert_params, add_user_result, ctx) != 0)
    {
        send_text(ctx->res, 500, "Failed to queue insert query");
        free_ctx(ctx->pool);;
        return;
    }

    printf("check_user_exists: Insert operation queued\n");
}

// Callback function that handles the database insert result
static void add_user_result(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;

    if (!ctx || !ctx->res)
    {
        printf("add_user_result: Invalid context\n");
        return;
    }

    ExecStatusType status = PQresultStatus(result);

    if (status == PGRES_COMMAND_OK)
    {
        printf("add_user_result: User created successfully\n");
        send_text(ctx->res, 201, "User created!");
    }
    else
    {
        printf("add_user_result: DB insert failed: %s\n", PQresultErrorMessage(result));
        send_text(ctx->res, 500, "DB insert failed");
    }

    free_ctx(ctx->pool);;
}
