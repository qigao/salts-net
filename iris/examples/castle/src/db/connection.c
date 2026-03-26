#include "dotenv.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fmt.h>

char *build_connection_string(void)
{
    const char *db_host = getenv("DB_HOST");
    const char *db_port = getenv("DB_PORT");
    const char *db_name = getenv("DB_NAME");
    const char *db_user = getenv("DB_USER");
    const char *db_password = getenv("DB_PASSWORD");

    if (!db_host)
        db_host = "";
    if (!db_port)
        db_port = "";
    if (!db_name)
        db_name = "";
    if (!db_user)
        db_user = "";
    if (!db_password)
        db_password = "";

    static char conninfo[512];
    fmt(conninfo, sizeof(conninfo),
        "host={} port={} dbname={} user={} password={}",
        db_host, db_port, db_name, db_user, db_password);

    return conninfo;
}
