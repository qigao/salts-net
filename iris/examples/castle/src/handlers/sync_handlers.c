#include "handlers.h"
#include "db.h" // extern PGconn *db;
#include "session.h"
#include "context.h"
#include "slugify.h"

void hello_world(Req *req, Res *res)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "message", "Hello World!");
    char *json_string = cJSON_PrintUnformatted(json);
    send_json(res, 200, json_string);
    cJSON_Delete(json);
    free(json_string);
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
    cJSON *json_array = cJSON_CreateArray();

    for (int i = 0; i < rows; i++)
    {
        int id = atoi(PQgetvalue(resPQ, i, 0));
        const char *name = PQgetvalue(resPQ, i, 1);
        const char *username = PQgetvalue(resPQ, i, 2);

        cJSON *user_json = cJSON_CreateObject();
        cJSON_AddNumberToObject(user_json, "id", id);
        cJSON_AddStringToObject(user_json, "name", name);
        cJSON_AddStringToObject(user_json, "username", username);

        cJSON_AddItemToArray(json_array, user_json);
    }

    PQclear(resPQ);

    char *json_string = cJSON_PrintUnformatted(json_array);
    send_json(res, 200, json_string);

    cJSON_Delete(json_array);
    free(json_string);
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
