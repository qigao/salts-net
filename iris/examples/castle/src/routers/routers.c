#include "handlers.h"
#include "middlewares.h"

void register_routers(void)
{
    get("/user/:user/posts/filter", use(is_authors_self), get_posts_by_cat);

    get("/user/:user/posts/:post", use(is_authors_self), get_post);
    put("/user/:user/posts/:post", use(auth_only, is_authors_self), edit_post);
    del("/user/:user/posts/:post", use(auth_only, is_authors_self), del_post);

    del("/user/:user/categories/:category", use(auth_only, is_authors_self), del_category);
    put("/user/:user/categories/:category", use(auth_only, is_authors_self), edit_category);

    get("/user/:user/posts", use(is_authors_self), get_all_posts);
    get("/user/:user", use(is_authors_self), get_profile);

    post("/create/category", use(body_checker, auth_only), create_category);
    post("/create/post", use(body_checker, auth_only), create_post);
    post("/login", use(body_checker), login);
    post("/register", use(body_checker), add_user);

    get("/logout", logout);
    get("/users", get_all_users);
    get("/users-async", get_all_users_async);
    get("/", hello_world);
}
