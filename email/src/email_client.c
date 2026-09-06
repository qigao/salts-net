#include "email/email_client.h"
#include <stdlib.h>
#include <string.h>

email_client_t *email_client_create_smtp(const smtp_config_t *config) {
  if (!config) return NULL;

  email_client_t *client = calloc(1, sizeof(email_client_t));
  if (!client) return NULL;

  client->smtp = smtp_client_create(config);

  if (!client->smtp) {
    free(client);
    return NULL;
  }

  return client;
}

email_client_t *email_client_create_imap(const imap_config_t *config) {
  if (!config) return NULL;

  email_client_t *client = calloc(1, sizeof(email_client_t));
  if (!client) return NULL;

  client->imap = imap_client_create(config);

  if (!client->imap) {
    free(client);
    return NULL;
  }

  return client;
}

email_client_t *email_client_create_pop3(const pop3_config_t *config) {
  if (!config) return NULL;

  email_client_t *client = calloc(1, sizeof(email_client_t));
  if (!client) return NULL;

  client->pop3 = pop3_client_create(config);

  if (!client->pop3) {
    free(client);
    return NULL;
  }

  return client;
}

email_client_t *email_client_create_full(const smtp_config_t *smtp_config,
                                         const imap_config_t *imap_config) {
  if (!smtp_config || !imap_config) return NULL;

  email_client_t *client = calloc(1, sizeof(email_client_t));
  if (!client) return NULL;

  client->smtp = smtp_client_create(smtp_config);
  client->imap = imap_client_create(imap_config);

  if (!client->smtp || !client->imap) {
    email_client_free(client);
    return NULL;
  }

  return client;
}

void email_client_free(email_client_t *client) {
  if (!client) return;

  if (client->smtp) smtp_client_free(client->smtp);
  if (client->imap) imap_client_free(client->imap);
  if (client->pop3) pop3_client_free(client->pop3);

  free(client);
}

int email_send_simple(email_client_t *client, const char *from_name, const char *from_email,
                      const char *to_name, const char *to_email, const char *subject,
                      const char *body) {
  if (!client || !client->smtp) return -1;

  mem_pool_t pool;
  mem_init(&pool, 4096);

  email_message_t *msg = email_message_create(&pool);
  if (!msg) {
    mem_destroy(&pool);
    return -1;
  }

  email_message_set_from(msg, from_name, from_email);
  email_message_add_to(msg, to_name, to_email);
  email_message_set_subject(msg, subject);
  email_message_set_text_body(msg, body);

  int result = smtp_send_message(client->smtp, msg);

  email_message_free(msg);
  mem_destroy(&pool);

  return result;
}

int email_send_html(email_client_t *client, const char *from_name, const char *from_email,
                    const char *to_name, const char *to_email, const char *subject,
                    const char *text_body, const char *html_body) {
  if (!client || !client->smtp) return -1;

  mem_pool_t pool;
  mem_init(&pool, 8192);

  email_message_t *msg = email_message_create(&pool);
  if (!msg) {
    mem_destroy(&pool);
    return -1;
  }

  email_message_set_from(msg, from_name, from_email);
  email_message_add_to(msg, to_name, to_email);
  email_message_set_subject(msg, subject);
  email_message_set_text_body(msg, text_body);
  email_message_set_html_body(msg, html_body);

  int result = smtp_send_message(client->smtp, msg);

  email_message_free(msg);
  mem_destroy(&pool);

  return result;
}
