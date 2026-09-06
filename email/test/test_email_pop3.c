#include "email/email_pop3.h"
#include "tinytest.h"
#include <salts/error_codes.h>

#include <stdlib.h>

spec("email_pop3") {
  it("validates raw retrieval arguments") {
    char *data = (char *)1;
    size_t len = 1u;
    check_equal(pop3_retrieve_raw(NULL, 1, &data, &len), SALTS_EINVAL);
    check_equal(pop3_retrieve_raw(NULL, 0, NULL, NULL), SALTS_EINVAL);
  }

  it("reports an absent socket when interrupted before connect") {
    pop3_config_t config = {0};
    pop3_client_t *client;
    config.host = "127.0.0.1";
    config.port = 110;
    client = pop3_client_create(&config);
    check_not_null(client);
    check_equal(pop3_interrupt(NULL, SALTS_ESHUTDOWN), SALTS_EINVAL);
    check_equal(pop3_interrupt(client, SALTS_ESHUTDOWN), SALTS_ENOTCONN);
    pop3_client_free(client);
  }

  it("validates configuration before allocating transport state") {
    pop3_config_t config = {0};
    check_null(pop3_client_create(NULL));
    check_null(pop3_client_create(&config));
    config.host = "127.0.0.1";
    config.port = 110;
    pop3_client_t *client = pop3_client_create(&config);
    check_not_null(client);
    pop3_client_free(client);
    config.use_tls = 1;
    config.use_stls = 1;
    check_null(pop3_client_create(&config));
  }
}
