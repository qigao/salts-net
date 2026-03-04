#include <stdlib.h>
#include <stdbool.h>
#include "middlewares.h"
#include "session.h"
#include "context.h"

int body_checker(Req *req, Res *res, Chain *chain)
{
    if (!req->body)
    {
        send_text(res, 400, "Missing request body");
        return 0;
    }

    return next(chain, req, res);
}

static bool string_to_bool(const char *str)
{
    if (!str)
        return false;
    return (strcmp(str, "true") == 0 || strcmp(str, "1") == 0);
}

int is_auth(Req *req, Res *res, Chain *chain)
{
    // Get the user session
    Session *session = get_session(req);

    auth_context_t *ctx = turbo_pool_alloc(req->arena, sizeof(auth_context_t));
    if (!ctx)
    {
        send_text(res, 500, "Internal Server Error");
        return 0;
    }

    memset(ctx, 0, sizeof(auth_context_t));

    if (session)
    {
        char *id = get_session_value(session, "id");
        char *name = get_session_value(session, "name");
        char *username = get_session_value(session, "username");
        char *is_admin_str = get_session_value(session, "is_admin");

        if (!id || !name || !username)
        {
            free(id);
            free(name);
            free(username);
            free(is_admin_str);

            send_text(res, 500, "Error: Incomplete session data");
            return 0;
        }

        // Copy strings into arena
        ctx->id = turbo_pool_strdup(req->arena, id);
        ctx->name = turbo_pool_strdup(req->arena, name);
        ctx->username = turbo_pool_strdup(req->arena, username);
        ctx->is_admin = string_to_bool(is_admin_str);

        free(id);
        free(name);
        free(username);
        free(is_admin_str);

        // Arena allocation check
        if (!ctx->id || !ctx->name || !ctx->username) {
            send_text(res, 500, "Memory allocation failed");
            return 0;
        }

        ctx->user_slug = turbo_pool_strdup(req->arena, username);
        if (!ctx->user_slug) {
            send_text(res, 500, "Memory allocation failed");
            return 0;
        }

        ctx->is_author = (strstr(ctx->username, "author") != NULL);
    }
    else
    {
        ctx->id = NULL;
        ctx->name = NULL;
        ctx->username = NULL;
        ctx->user_slug = NULL;
        ctx->is_admin = false;
        ctx->is_author = false;
    }

    set_context(req, ctx, sizeof(*ctx), NULL);

    // Continue to next handler in chain
    return next(chain, req, res);
}

int is_authors_self(Req *req, Res *res, Chain *chain)
{
    auth_context_t *auth_ctx = (auth_context_t *)get_context(req);

    const char *user_slug = get_params(req, "user");
    auth_ctx->user_slug = strdup(user_slug ? user_slug : "");

    if (!auth_ctx->user_slug)
    {
        send_text(res, 500, "Internal Server Error");
        return 0;
    }

    if (auth_ctx && auth_ctx->username)
    {
        if (strcmp(auth_ctx->user_slug, auth_ctx->username) == 0)
            auth_ctx->is_author = true;
    }

    return next(chain, req, res);
}

int auth_only(Req *req, Res *res, Chain *chain)
{
    auth_context_t *ctx = (auth_context_t *)get_context(req);

    if (!ctx || !ctx->id || !ctx->name || !ctx->username)
    {
        send_text(res, 401, "Not allowed");
        return 0;
    }

    return next(chain, req, res);
}
