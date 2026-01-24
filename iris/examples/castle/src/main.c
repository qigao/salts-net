#include "server.h"
#include "cors.h"
#include "dotenv.h"
#include "db.h"
#include "routers.h"
#include "session.h"
#include "middlewares.h"

void destroy_app(void)
{
    close_db();
    reset_sessions();
    reset_router();
    reset_cors();
}

int main(void)
{
    dotenv_load("..", false);
    const char *port = getenv("PORT");
    const unsigned short PORT = (unsigned short)atoi(port);
    char *CORS_ORIGIN = getenv("CORS_ORIGIN");

    cors_t cors = {
        .origin = CORS_ORIGIN,
        .headers = "Content-Type, Authorization",
        .credentials = "true",
        .max_age = "86400",
        .enabled = false, // Delete this line if you want to enable the cors configuration
    };

    init_cors(&cors);
    if (init_router() != 0) {
        fprintf(stderr, "Failed to initialize router\n");
        return -1;
    }
    init_sessions();

    if (init_db() != 0)
    {
        fprintf(stderr, "Database initialization failed.\n");
        return 1;
    }

    hook(is_auth); // Global middleware

    register_routers();

    shutdown_hook(destroy_app);
    int result = ecewo(PORT);
    if (result != 0) {
        fprintf(stderr, "Server failed to start: %d\n", result);
        return result;
    }
    return 0;
}
