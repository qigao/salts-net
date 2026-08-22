#include "email/email_pop3.h"
#include "tinytest.h"
#include "turbo_error.h"

#include <stdlib.h>

spec("email_pop3") {
  it("validates raw retrieval arguments") {
    char *data = (char *)1;
    size_t len = 1u;
    check_equal(pop3_retrieve_raw(NULL, 1, &data, &len), TURBO_EINVAL);
    check_equal(pop3_retrieve_raw(NULL, 0, NULL, NULL), TURBO_EINVAL);
  }

  it("reports an absent socket when interrupted before connect") {
    pop3_config_t config = {0};
    coro_context_t *ctx = coro_context_create(NULL);
    pop3_client_t *client;
    check_not_null(ctx);
    config.host = "127.0.0.1";
    config.port = 110;
    client = pop3_client_create(ctx, &config);
    check_not_null(client);
    check_equal(pop3_interrupt(NULL, TURBO_ESHUTDOWN), TURBO_EINVAL);
    check_equal(pop3_interrupt(client, TURBO_ESHUTDOWN), TURBO_ENOTCONN);
    pop3_client_free(client);
    coro_context_destroy(ctx);
  }
}
