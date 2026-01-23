#include "iris.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Iris JWT Authentication Example
 */

const char *JWT_SECRET = "super-secret-key-for-iris-jwt";

void protected_handler(Req *req, Res *res) {
    void *jwt = iris_jwt_get_claims(req);
    if (jwt) {
        // In a real app, you'd cast to cjwt_t* if you have access to the header,
        // or we'd provide more getters. For now, we just know it succeeded.
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

int main() {
    iris_app_t *app = iris_app_create();

    // Set the global JWT secret
    iris_jwt_set_secret(JWT_SECRET);

    // Public route
    iris_app_get(app, "/", (RequestHandler)[](Req *req, Res *res) {
        send_text(res, 200, "Iris JWT Example. Go to /login to get a token.");
    });

    // Login route (returns a JWT)
    iris_app_post(app, "/login", login_handler);

    // Protected route (requires JWT middleware)
    iris_app_get_mw(app, "/protected", use(iris_jwt_middleware), protected_handler);

    printf("Iris JWT server starting on http://localhost:8080\n");
    // Start the server (simplified for example)
    // iris_app_listen(app, "tcp://0.0.0.0:8080"); 

    iris_app_destroy(app);
    return 0;
}
