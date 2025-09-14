#include "iris.h"
#include "iris_app.h"
#include "middleware.h"
#include "error_recovery.h"
#include <stdio.h>
#include <stdlib.h>

/* Legacy global pointer - points to default app's route trie for backward compatibility */
route_trie_t *global_route_trie = NULL;

/* Initialize router using default app */
int init_router(void) {
    iris_app_t *app = iris_app_default();
    if (!app) {
        iris_error_context_t ctx = IRIS_ERROR_CONTEXT(IRIS_ERROR_SERVER_INIT_FAILED, -1,
                                                      "Failed to create default iris app");
        iris_recovery_action_t action = iris_handle_error(&ctx, IRIS_RECOVERY_GRACEFUL_SHUTDOWN);
        return (action == IRIS_RECOVERY_GRACEFUL_SHUTDOWN) ? -1 : 0;
    }
    /* Update legacy global pointer */
    global_route_trie = app->route_trie;
    return 0;
}

void reset_router(void) {
    /* Reset the default app */
    iris_app_reset_default();
    global_route_trie = NULL;

    /* Also reset legacy global middleware */
    reset_middleware();
}
