#include "iris.h"
#include "iris_app.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjwt/cjwt.h>

/**
 * Iris JWT Authentication Example
 */

const char *JWT_SECRET = "super-secret-key-for-iris-jwt";

void home_handler(Req *req, Res *res) {
    send_text(res, 200, "Iris JWT Example. Go to /login to get a token.");
}

void protected_handler(Req *req, Res *res) {
    void *claims_ptr = iris_jwt_get_claims(req);
    if (claims_ptr) {
        cjwt_t *jwt = (cjwt_t *)claims_ptr;
        
        // Example of accessing claims (requires cjwt structure knowledge)
        // Here we just print success.
        
        send_json(res, 200, "{\"status\":\"success\", \"message\":\"You have accessed a protected route!\"}");
    } else {
        send_json(res, 500, "{\"status\":\"error\", \"message\":\"JWT claims not found in context\"}");
    }
}

void login_handler(Req *req, Res *res) {
    // In a real app, you'd verify username/password here
    const char *claims = "{\"sub\":\"user_123\", \"name\":\"Iris User\", \"admin\":true}";
    char *token = iris_jwt_encode(JWT_SECRET, claims);
    
    if (token) {
        char response[512];
        snprintf(response, sizeof(response), "{\"token\":\"%s\"}", token);
        send_json(res, 200, response);
        free(token);
    } else {
        send_json(res, 500, "{\"error\":\"Failed to generate token\"}");
    }
}

// Guest Login Handler
void login_guest_handler(Req *req, Res *res) {
    // Guest user, admin = false
    const char *claims = "{\"sub\":\"guest_456\", \"name\":\"Guest User\", \"admin\":false}";
    char *token = iris_jwt_encode(JWT_SECRET, claims);
    
    if (token) {
        char response[512];
        snprintf(response, sizeof(response), "{\"token\":\"%s\"}", token);
        send_json(res, 200, response);
        free(token);
    } else {
        send_json(res, 500, "{\"error\":\"Failed to generate token\"}");
    }
}

// ACL Middleware: Checks if the user has "admin": true claim
int acl_middleware(Req *req, Res *res, Chain *chain) {
    void *claims_ptr = iris_jwt_get_claims(req);
    if (!claims_ptr) {
        // Did you forget to add jwt_middleware before this?
        send_json(res, 500, "{\"error\":\"Internal Server Error\", \"message\":\"No JWT context found\"}");
        return 1;
    }

    cjwt_t *jwt = (cjwt_t *)claims_ptr;
    json_value_t *admin_claim = json_object_get(jwt->private_claims, "admin");

    if (!(json_type(admin_claim) == JSON_BOOL) || !json_bool(admin_claim)) {
        send_json(res, 403, "{\"error\":\"Forbidden\", \"message\":\"Admin access required\"}");
        return 1;
    }

    // Access granted
    return next(chain, req, res);
}

void admin_handler(Req *req, Res *res) {
    send_json(res, 200, "{\"status\":\"success\", \"message\":\"Welcome, Admin!\"}");
}

int main() {
    // Initialize the router (required for legacy route registration macros)
    if (init_router() != 0) {
        fprintf(stderr, "Failed to initialize router\n");
        return 1;
    }

    // Set the global JWT secret
    iris_jwt_set_secret(JWT_SECRET);

    // Public route
    get("/", home_handler);

    // Login route (returns a JWT with admin:true)
    post("/login", login_handler);

    // Guest login route (returns a JWT with admin:false)
    post("/login/guest", login_guest_handler);

    // Protected route (requires valid JWT)
    get("/protected", use(iris_jwt_middleware), protected_handler);

    // Admin route (requires valid JWT AND admin claim)
    get("/admin", use(iris_jwt_middleware, acl_middleware), admin_handler);

    printf("Iris JWT server starting on http://localhost:8080\n");
    
    // Start the server
    iris_app_listen(NULL, 8080);

    return 0;
}
