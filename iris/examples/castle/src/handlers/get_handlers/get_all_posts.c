#include "handlers.h"
#include "context.h"

typedef struct
{
    turbo_arena_t *pool;
    Res *res;
    bool is_author;
} ctx_t;

static void posts_result_callback(pg_async_t *pg, PGresult *result, void *data);

void get_all_posts(Req *req, Res *res)
{
    auth_context_t *auth_ctx = (auth_context_t *)get_context(req);

    turbo_arena_t *async_pool = malloc(sizeof(turbo_arena_t)); if (!async_pool || turbo_arena_init(async_pool, 65536) != 0) { if (async_pool) free(async_pool); async_pool = NULL; }
    if (!async_pool) {
        send_text(res, 500, "Arena allocation failed");
        return;
    }

    // Create context to pass to callback
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

    // Create async PostgreSQL context
    pg_async_t *pg = pquv_create(db, ctx);
    if (!pg)
    {
        printf("get_all_posts: Failed to create async context\n");
        send_text(res, 500, "Failed to create async context");
        free_ctx(ctx->pool);
        return;
    }

    char *sql;

    if (auth_ctx->is_author)
    {
        sql =
            "SELECT p.id, p.header, p.slug, p.content, p.reading_time, "
            "       p.author_id, u.username, p.created_at, p.updated_at, p.is_hidden, "
            "       COALESCE(string_agg(c.category, ','), '') as categories, "
            "       COALESCE(string_agg(c.slug, ','), '') as category_slugs, "
            "       COALESCE(string_agg(c.id::text, ','), '') as category_ids "
            "FROM posts p "
            "JOIN users u ON p.author_id = u.id "
            "LEFT JOIN post_categories pc ON p.id = pc.post_id "
            "LEFT JOIN categories c ON pc.category_id = c.id "
            "WHERE u.username = $1 "
            "GROUP BY p.id, u.username "
            "ORDER BY p.created_at DESC";
    }
    else
    {
        sql =
            "SELECT p.id, p.header, p.slug, p.content, p.reading_time, "
            "       p.author_id, u.username, p.created_at, p.updated_at, p.is_hidden, "
            "       COALESCE(string_agg(c.category, ','), '') as categories, "
            "       COALESCE(string_agg(c.slug, ','), '') as category_slugs, "
            "       COALESCE(string_agg(c.id::text, ','), '') as category_ids "
            "FROM posts p "
            "JOIN users u ON p.author_id = u.id "
            "LEFT JOIN post_categories pc ON p.id = pc.post_id "
            "LEFT JOIN categories c ON pc.category_id = c.id "
            "WHERE u.username = $1 "
            "  AND p.is_hidden = FALSE "
            "GROUP BY p.id, u.username "
            "ORDER BY p.created_at DESC";
    }

    const char *params[] = {auth_ctx->user_slug};

    // Queue the query and execute
    if (pquv_queue(pg, sql, 1, params, posts_result_callback, ctx) != 0 ||
        pquv_execute(pg) != 0)
    {
        send_text(res, 500, "Failed to queue or execute query");
        free_ctx(ctx->pool);
        return;
    }

    // Function returns here, callback will be called when query completes
}

// Callback function that processes the query result
static void posts_result_callback(pg_async_t *pg, PGresult *result, void *data)
{
    ctx_t *ctx = (ctx_t *)data;
    if (!ctx || !ctx->res)
        return;

    if (PQresultStatus(result) != PGRES_TUPLES_OK)
    {
        send_text(ctx->res, 500, "DB select failed");
        free_ctx(ctx->pool);
        return;
    }

    int rows = PQntuples(result);
    json_value_t *root = json_create_object();
    json_value_t *posts = json_create_array();

    for (int i = 0; i < rows; i++)
    {
        bool hidden = (PQgetvalue(result, i, PQfnumber(result, "is_hidden"))[0] == 't');
        if (!ctx->is_author && hidden)
        {
            // If not author and post is hidden, skip that post
            continue;
        }

        json_value_t *obj = json_create_object();

        // Add string fields
        json_object_set_string(obj, "header", PQgetvalue(result, i, PQfnumber(result, "header")));
        json_object_set_string(obj, "slug", PQgetvalue(result, i, PQfnumber(result, "slug")));
        json_object_set_string(obj, "content", PQgetvalue(result, i, PQfnumber(result, "content")));
        json_object_set_string(obj, "username", PQgetvalue(result, i, PQfnumber(result, "username")));
        json_object_set_string(obj, "created_at", PQgetvalue(result, i, PQfnumber(result, "created_at")));
        json_object_set_string(obj, "updated_at", PQgetvalue(result, i, PQfnumber(result, "updated_at")));

        // Add integer fields
        json_object_set_number(obj, "reading_time", atoi(PQgetvalue(result, i, PQfnumber(result, "reading_time"))));
        json_object_set_number(obj, "author_id", atoi(PQgetvalue(result, i, PQfnumber(result, "author_id"))));

        // Add boolean field
        json_object_set_bool(obj, "is_hidden", strcmp(PQgetvalue(result, i, PQfnumber(result, "is_hidden")), "t") == 0);

        char *categories_str = PQgetvalue(result, i, PQfnumber(result, "categories"));
        char *category_slugs_str = PQgetvalue(result, i, PQfnumber(result, "category_slugs"));
        char *category_ids_str = PQgetvalue(result, i, PQfnumber(result, "category_ids"));

        json_value_t *categories_array = json_create_array();

        if (strlen(categories_str) > 0)
        {
            char *categories_copy = strdup(categories_str);
            char *slugs_copy = strdup(category_slugs_str);
            char *ids_copy = strdup(category_ids_str);

            char *cat_tok, *slug_tok, *id_tok;
            char *cat_saveptr, *slug_saveptr, *id_saveptr;

            cat_tok = strtok_r(categories_copy, ",", &cat_saveptr);
            slug_tok = strtok_r(slugs_copy, ",", &slug_saveptr);
            id_tok = strtok_r(ids_copy, ",", &id_saveptr);

            while (cat_tok && slug_tok && id_tok)
            {
                json_value_t *category_obj = json_create_object();
                json_object_set_number(category_obj, "id", atoi(id_tok));
                json_object_set_string(category_obj, "category", cat_tok);
                json_object_set_string(category_obj, "slug", slug_tok);
                json_array_add(categories_array, category_obj);

                cat_tok = strtok_r(NULL, ",", &cat_saveptr);
                slug_tok = strtok_r(NULL, ",", &slug_saveptr);
                id_tok = strtok_r(NULL, ",", &id_saveptr);
            }

            free(categories_copy);
            free(slugs_copy);
            free(ids_copy);
        }

        json_object_add(obj, "categories", categories_array);
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
