#include "db.h"
#include "connection.h"

PGconn *db = NULL;

static void exit_on_error(PGconn *conn, PGresult *res)
{
    if (res)
    {
        fprintf(stderr, "ERROR: %s\n", PQresultErrorMessage(res));
        PQclear(res);
    }
    else if (conn)
    {
        fprintf(stderr, "ERROR: %s\n", PQerrorMessage(conn));
    }
    if (conn)
        PQfinish(conn);
    exit(EXIT_FAILURE);
}

static int exec_sql(const char *sql)
{
    PGresult *res = PQexec(db, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        fprintf(stderr, "SQL failed: %s\n", PQresultErrorMessage(res));
        PQclear(res);
        return 1;
    }
    PQclear(res);
    return 0;
}

static int create_tables(void)
{
    const char *queries[] = {
        "CREATE TABLE IF NOT EXISTS users ("
        "  id SERIAL PRIMARY KEY, "
        "  name TEXT NOT NULL, "
        "  username TEXT NOT NULL, "
        "  password TEXT NOT NULL, "
        "  email TEXT NOT NULL, "
        "  about TEXT, "
        "  created_at TIMESTAMP WITHOUT TIME ZONE DEFAULT NOW()"
        ");",

        "CREATE TABLE IF NOT EXISTS posts ("
        "  id SERIAL PRIMARY KEY, "
        "  header TEXT NOT NULL, "
        "  slug TEXT NOT NULL, "
        "  content TEXT NOT NULL, "
        "  reading_time INTEGER NOT NULL, "
        "  author_id INTEGER NOT NULL REFERENCES users(id), "
        "  created_at TIMESTAMP WITHOUT TIME ZONE NOT NULL DEFAULT NOW(), "
        "  updated_at TIMESTAMP WITHOUT TIME ZONE NOT NULL DEFAULT NOW(), "
        "  is_hidden BOOLEAN NOT NULL DEFAULT FALSE"
        ");",

        "CREATE TABLE IF NOT EXISTS categories ("
        "  id SERIAL PRIMARY KEY, "
        "  category TEXT NOT NULL, "
        "  slug TEXT NOT NULL, "
        "  author_id INTEGER NOT NULL REFERENCES users(id)"
        ");",

        "CREATE TABLE IF NOT EXISTS post_categories ("
        "  post_id INTEGER NOT NULL REFERENCES posts(id) ON DELETE CASCADE, "
        "  category_id INTEGER NOT NULL REFERENCES categories(id) ON DELETE CASCADE, "
        "  PRIMARY KEY(post_id, category_id)"
        ");",
    };

    size_t count = sizeof(queries) / sizeof(queries[0]);
    for (size_t i = 0; i < count; ++i)
    {
        if (exec_sql(queries[i]) != 0)
        {
            fprintf(stderr, "Query %zu failed.\n", i + 1);
            return 1;
        }
    }

    printf("Tables and extensions created or already exist.\n");
    return 0;
}

int init_db(void)
{
    const char *conninfo = build_connection_string();

    db = PQconnectdb(conninfo);
    if (PQstatus(db) != CONNECTION_OK)
    {
        fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage(db));
        PQfinish(db);
        return 1;
    }
    printf("Database connection successful.\n");

    if (PQsetnonblocking(db, 1) != 0)
    { // for non-blocking async I/O
        fprintf(stderr, "Failed to set async connection to nonblocking mode\n");
        PQfinish(db);
        return 1;
    }

    printf("Async database connection successful.\n");

    if (create_tables() != 0)
    {
        printf("Tables cannot be created\n");
        return 1;
    }

    return 0;
}

void close_db(void)
{
    if (db)
    {
        PQfinish(db);
        db = NULL;
    }
}
