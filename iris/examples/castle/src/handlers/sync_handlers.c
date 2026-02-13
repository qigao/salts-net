#include "handlers.h"
#include "db.h" // extern PGconn *db;
#include "session.h"
#include "context.h"
#include "slugify.h"

void hello_world(Req *req, Res *res)
{
    json_value_t *json = json_create_object();
    json_object_set_string(json, "message", "Hello World!");
    size_t json_string_len = 0;
    char *json_string = json_serialize(json, &json_string_len);
    send_json(res, 200, json_string);
    json_free(json);
    json_serialize_free(json_string);
}

void get_all_users(Req *req, Res *res)
{
    const char *sql = "SELECT id, name, username FROM users;";

    PGresult *resPQ = PQexec(db, sql);
    if (PQresultStatus(resPQ) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "DB select failed: %s", PQerrorMessage(db));
        PQclear(resPQ);
        send_text(res, 500, "DB select failed");
        return;
    }

    int rows = PQntuples(resPQ);
    json_value_t *json_array = json_create_array();

    for (int i = 0; i < rows; i++)
    {
        int id = atoi(PQgetvalue(resPQ, i, 0));
        const char *name = PQgetvalue(resPQ, i, 1);
        const char *username = PQgetvalue(resPQ, i, 2);

        json_value_t *user_json = json_create_object();
        json_object_set_number(user_json, "id", id);
        json_object_set_string(user_json, "name", name);
        json_object_set_string(user_json, "username", username);

        json_array_add(json_array, user_json);
    }

    PQclear(resPQ);

    size_t json_string_len = 0;
    char *json_string = json_serialize(json_array, &json_string_len);
    send_json(res, 200, json_string);

    json_free(json_array);
    json_serialize_free(json_string);
}

void logout(Req *req, Res *res)
{
    Session *sess = get_session(req);

    if (!sess)
    {
        send_text(res, 400, "You have to login");
    }
    else
    {
        // The cookie_options should be the same as what we use in the login handler
        cookie_options_t cookie_options = {
            .max_age = 3600, // 1 hour
            .path = "/",
            .same_site = "Lax",
            .http_only = true,
            .secure = true,
        };

        destroy_session(res, sess, &cookie_options);
        send_text(res, 302, "Logged out");
    }
}
