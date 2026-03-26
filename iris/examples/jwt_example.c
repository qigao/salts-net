#include "iris.h"
#include "tlog.h"
#include <cjwt/cjwt.h>
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * Iris JWT Authentication Example
 */

static const char JWT_SECRET[] = "super-secret-key-for-iris-jwt";

static void send_token_response(Res *res, const char *claims_json) {
    char *token = iris_jwt_encode(JWT_SECRET, claims_json);

    if (!token) {
        send_json(res, 500, "{\"error\":\"Failed to generate token\"}");
        return;
    }

    char response[512];
    fmt(response, sizeof(response), "{{\"token\":\"{}\"}}", token);
    send_json(res, 200, response);
    free(token);
}

static void home_handler(Req *req, Res *res) {
    (void)req;
    send_text(res, 200, "Iris JWT Example. Go to /login to get a token.");
}

static void protected_handler(Req *req, Res *res) {
    if (iris_jwt_get_claims(req)) {
        send_json(res, 200,
                  "{\"status\":\"success\",\"message\":\"You have accessed a protected route!\"}");
    } else {
        send_json(res, 500, "{\"status\":\"error\", \"message\":\"JWT claims not found in context\"}");
    }
}

static void login_handler(Req *req, Res *res) {
    (void)req;
    send_token_response(res, "{\"sub\":\"user_123\",\"name\":\"Iris User\",\"role\":\"admin\"}");
}

static void login_guest_handler(Req *req, Res *res) {
    (void)req;
    send_token_response(res, "{\"sub\":\"guest_456\",\"name\":\"Guest User\",\"role\":\"guest\"}");
}

static int acl_middleware(Req *req, Res *res, Chain *chain) {
    void *claims_ptr = iris_jwt_get_claims(req);
    if (!claims_ptr) {
        send_json(res, 500, "{\"error\":\"Internal Server Error\", \"message\":\"No JWT context found\"}");
        return 1;
    }

    cjwt_t *jwt = (cjwt_t *)claims_ptr;
    json_value_t *role_claim = json_object_get(jwt->private_claims, "role");

    if (!role_claim || json_type(role_claim) != JSON_STRING ||
        strcmp(json_string(role_claim), "admin") != 0) {
        send_json(res, 403, "{\"error\":\"Forbidden\", \"message\":\"Admin access required\"}");
        return 1;
    }

    return next(chain, req, res);
}

static void admin_handler(Req *req, Res *res) {
    (void)req;
    send_json(res, 200, "{\"status\":\"success\", \"message\":\"Welcome, Admin!\"}");
}

int main() {
    if (init_router() != 0) {
        TLOG_ERROR("Failed to initialize router");
        return 1;
    }

    iris_jwt_set_secret(JWT_SECRET);

    get("/", home_handler);
    post("/login", login_handler);
    post("/login/guest", login_guest_handler);
    get("/protected", use(iris_jwt_middleware), protected_handler);
    get("/admin", use(iris_jwt_middleware, acl_middleware), admin_handler);

    TLOG_INFO("Iris JWT server starting on http://localhost:8080");
    return iris_app_listen(NULL, 8080);
}
