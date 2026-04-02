#ifndef HANDLERS_H
#define HANDLERS_H

#include "iris.h"
#include <turbo_parser.h>
#include "pquv.h"
#include "db.h" // extern PGconn *db;
#include "connection.h"
#include "utils.h"

void hello_world(Req *req, Res *res);
void get_all_users(Req *req, Res *res);
void get_all_users_async(Req *req, Res *res);
void add_user(Req *req, Res *res);
void login(Req *req, Res *res);
void logout(Req *req, Res *res);
void create_post(Req *req, Res *res);
void get_post(Req *req, Res *res);
void get_all_posts(Req *req, Res *res);
void create_category(Req *req, Res *res);
void get_profile(Req *req, Res *res);
void get_posts_by_cat(Req *req, Res *res);
void del_post(Req *req, Res *res);
void del_category(Req *req, Res *res);
void edit_post(Req *req, Res *res);
void edit_category(Req *req, Res *res);

#endif
