#ifndef DB_H
#define DB_H

#include <libpq-fe.h>

extern PGconn *db;

int init_db(void);
void close_db(void);

#endif
