#ifndef MIDDLEWARES_H
#define MIDDLEWARES_H

#include "iris.h"

int body_checker(Req *req, Res *res, Chain *chain);
int is_auth(Req *req, Res *res, Chain *chain);
int auth_only(Req *req, Res *res, Chain *chain);
int is_authors_self(Req *req, Res *res, Chain *chain);

#endif
