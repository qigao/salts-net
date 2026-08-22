#include "email/email_imap.h"
#include "email/email_message.h"
#include "email/email_pop3.h"
#include "email/email_smtp.h"
#include "tinytest.h"
#include "CoroNet.h"
#include <fmt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  char smtp_host[128];
  int smtp_port;
  char smtp_from[128];
  char smtp_to[128];
  char pop3_host[128];
  int pop3_port;
  char pop3_user[128];
  char pop3_pass[128];
  char imap_host[128];
  int imap_port;
  char imap_user[128];
  char imap_pass[128];
  char subject[256];
  char body[256];
} smtp4dev_test_config_t;

typedef struct {
  smtp4dev_test_config_t cfg;
  coro_context_t *ctx;
  int completed;
  int success;
  char error[512];
} smtp4dev_result_t;

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value = getenv(name);
  if (!value || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static int integration_enabled(void) {
  const char *value = getenv("SMTP4DEV_INTEGRATION");
  return value && strcmp(value, "1") == 0;
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0) {
    return;
  }
  if (!src) {
    dst[0] = '\0';
    return;
  }
  fmt(dst, dst_size, "{}", src);
}

static void fail_result(smtp4dev_result_t *result, const char *message) {
  result->success = 0;
  copy_string(result->error, sizeof(result->error), message);
}

static int message_matches(email_message_t *msg, const char *subject,
                           const char *body) {
  if (!msg || !msg->subject || !msg->text_body) {
    return 0;
  }
  if (strcmp(msg->subject, subject) != 0) {
    return 0;
  }
  return strstr(msg->text_body, body) != NULL;
}

static int send_unique_message(coro_context_t *ctx,
                               const smtp4dev_test_config_t *cfg,
                               smtp4dev_result_t *result) {
  smtp_config_t smtp_cfg = {0};
  smtp_client_t *smtp = NULL;
  mem_pool_t pool;
  email_message_t *msg = NULL;
  int rc = -1;

  smtp_cfg.host = (char *)cfg->smtp_host;
  smtp_cfg.port = cfg->smtp_port;
  smtp_cfg.use_tls = 0;
  smtp_cfg.use_starttls = 0;
  smtp_cfg.auth_method = SMTP_AUTH_NONE;
  smtp_cfg.timeout_ms = 30000;

  smtp = smtp_client_create(ctx, &smtp_cfg);
  if (!smtp) {
    fail_result(result, "smtp_client_create failed");
    return -1;
  }

  if (smtp_connect(smtp) != 0) {
    fmt(result->error, sizeof(result->error), "SMTP connect failed: {}", smtp_get_error(smtp));
    goto cleanup;
  }

  mem_init(&pool, 8192);
  msg = email_message_create(&pool);
  if (!msg) {
    fail_result(result, "email_message_create failed");
    mem_destroy(&pool);
    goto cleanup;
  }

  email_message_set_from(msg, "TurboNet Integration", cfg->smtp_from);
  email_message_add_to(msg, "TurboNet Recipient", cfg->smtp_to);
  email_message_set_subject(msg, cfg->subject);
  email_message_set_text_body(msg, cfg->body);
  email_message_set_html_body(msg, "<html><body><p>smtp4dev integration</p></body></html>");

  if (smtp_send_message(smtp, msg) != 0) {
    fmt(result->error, sizeof(result->error), "SMTP send failed: {}", smtp_get_error(smtp));
    email_message_free(msg);
    mem_destroy(&pool);
    goto cleanup;
  }

  email_message_free(msg);
  mem_destroy(&pool);
  rc = 0;

cleanup:
  if (smtp) {
    smtp_disconnect(smtp);
    smtp_client_free(smtp);
  }
  return rc;
}

static int wait_for_pop3_message(coro_context_t *ctx,
                                 const smtp4dev_test_config_t *cfg,
                                 smtp4dev_result_t *result) {
  pop3_config_t pop3_cfg = {0};
  pop3_client_t *pop3 = NULL;
  int attempt;

  pop3_cfg.host = (char *)cfg->pop3_host;
  pop3_cfg.port = cfg->pop3_port;
  pop3_cfg.use_tls = 0;
  pop3_cfg.use_stls = 0;
  pop3_cfg.username = (char *)cfg->pop3_user;
  pop3_cfg.password = (char *)cfg->pop3_pass;
  pop3_cfg.timeout_ms = 30000;

  pop3 = pop3_client_create(ctx, &pop3_cfg);
  if (!pop3) {
    fail_result(result, "pop3_client_create failed");
    return -1;
  }

  if (pop3_connect(pop3) != 0) {
    fmt(result->error, sizeof(result->error), "POP3 connect failed: {}", pop3_get_error(pop3));
    pop3_client_free(pop3);
    return -1;
  }

  for (attempt = 0; attempt < 10; ++attempt) {
    int msg_count = pop3_stat(pop3, NULL);
    int index;

    if (msg_count < 0) {
      fmt(result->error, sizeof(result->error), "POP3 STAT failed: {}", pop3_get_error(pop3));
      pop3_disconnect(pop3);
      pop3_client_free(pop3);
      return -1;
    }

    for (index = msg_count; index >= 1; --index) {
      email_message_t *msg = pop3_retrieve_message(pop3, index);
      int matched = message_matches(msg, cfg->subject, cfg->body);
      if (msg) {
        email_message_free(msg);
      }
      if (matched) {
        pop3_disconnect(pop3);
        pop3_client_free(pop3);
        return 0;
      }
    }

    coro_sleep(ctx, 200);
  }

  fail_result(result, "POP3 did not return the sent message");
  pop3_disconnect(pop3);
  pop3_client_free(pop3);
  return -1;
}

static int wait_for_imap_message(coro_context_t *ctx,
                                 const smtp4dev_test_config_t *cfg,
                                 smtp4dev_result_t *result) {
  imap_config_t imap_cfg = {0};
  imap_client_t *imap = NULL;
  int attempt;

  (void)ctx;

  imap_cfg.host = (char *)cfg->imap_host;
  imap_cfg.port = cfg->imap_port;
  imap_cfg.use_tls = 0;
  imap_cfg.use_starttls = 0;
  imap_cfg.username = (char *)cfg->imap_user;
  imap_cfg.password = (char *)cfg->imap_pass;
  imap_cfg.timeout_ms = 30000;

  imap = imap_client_create(ctx, &imap_cfg);
  if (!imap) {
    fail_result(result, "imap_client_create failed");
    return -1;
  }

  if (imap_connect(imap) != 0) {
    fmt(result->error, sizeof(result->error), "IMAP connect failed: {}", imap_get_error(imap));
    imap_client_free(imap);
    return -1;
  }

  for (attempt = 0; attempt < 10; ++attempt) {
    imap_mailbox_t *mailbox = imap_select_mailbox(imap, "INBOX");
    int result_count = 0;
    int *results = NULL;
    int index;

    if (!mailbox) {
      fmt(result->error, sizeof(result->error), "IMAP SELECT failed: {}", imap_get_error(imap));
      imap_disconnect(imap);
      imap_client_free(imap);
      return -1;
    }
    free(mailbox->name);
    free(mailbox);

    results = imap_search(imap, "ALL", &result_count);
    if (!results) {
      fmt(result->error, sizeof(result->error), "IMAP SEARCH failed: {}", imap_get_error(imap));
      imap_disconnect(imap);
      imap_client_free(imap);
      return -1;
    }

    for (index = result_count - 1; index >= 0; --index) {
      email_message_t *msg = imap_fetch_message(imap, results[index]);
      int matched = message_matches(msg, cfg->subject, cfg->body);
      if (msg) {
        email_message_free(msg);
      }
      if (matched) {
        free(results);
        imap_disconnect(imap);
        imap_client_free(imap);
        return 0;
      }
    }

    free(results);
    coro_sleep(ctx, 200);
  }

  fail_result(result, "IMAP did not return the sent message");
  imap_disconnect(imap);
  imap_client_free(imap);
  return -1;
}

static void smtp4dev_roundtrip_coro(coro_t *co, void *arg) {
  smtp4dev_result_t *result = (smtp4dev_result_t *)arg;
  coro_context_t *ctx = result->ctx;

  (void)co;

  if (send_unique_message(ctx, &result->cfg, result) != 0) {
    result->completed = 1;
    return;
  }

  if (wait_for_pop3_message(ctx, &result->cfg, result) != 0) {
    result->completed = 1;
    return;
  }

  if (wait_for_imap_message(ctx, &result->cfg, result) != 0) {
    result->completed = 1;
    return;
  }

  result->success = 1;
  result->completed = 1;
}

static void init_test_config(smtp4dev_test_config_t *cfg) {
  time_t now = time(NULL);

  memset(cfg, 0, sizeof(*cfg));
  copy_string(cfg->smtp_host, sizeof(cfg->smtp_host),
              env_or_default("SMTP_HOST", "127.0.0.1"));
  cfg->smtp_port = atoi(env_or_default("SMTP_PORT", "25"));
  copy_string(cfg->smtp_from, sizeof(cfg->smtp_from),
              env_or_default("SMTP_FROM", "sender@smtp4dev.local"));
  copy_string(cfg->smtp_to, sizeof(cfg->smtp_to),
              env_or_default("SMTP_TO", "recipient@smtp4dev.local"));

  copy_string(cfg->pop3_host, sizeof(cfg->pop3_host),
              env_or_default("POP3_HOST", "127.0.0.1"));
  cfg->pop3_port = atoi(env_or_default("POP3_PORT", "110"));
  copy_string(cfg->pop3_user, sizeof(cfg->pop3_user),
              env_or_default("POP3_USERNAME", "turbo"));
  copy_string(cfg->pop3_pass, sizeof(cfg->pop3_pass),
              env_or_default("POP3_PASSWORD", "turbo"));

  copy_string(cfg->imap_host, sizeof(cfg->imap_host),
              env_or_default("IMAP_HOST", "127.0.0.1"));
  cfg->imap_port = atoi(env_or_default("IMAP_PORT", "143"));
  copy_string(cfg->imap_user, sizeof(cfg->imap_user),
              env_or_default("IMAP_USERNAME", "turbo"));
  copy_string(cfg->imap_pass, sizeof(cfg->imap_pass),
              env_or_default("IMAP_PASSWORD", "turbo"));

  fmt(cfg->subject, sizeof(cfg->subject), "TurboNet smtp4dev integration {}", (long long)now);
  fmt(cfg->body, sizeof(cfg->body), "smtp4dev integration body {}", (long long)now);
}

spec("email_integration") {
  describe("smtp4dev roundtrip") {
    it("should send and retrieve a message through smtp pop3 and imap") {
      smtp4dev_result_t result;
      coro_context_t *ctx;
      int rc;

      if (!integration_enabled()) {
        fprintf(stderr,
                "Skipping smtp4dev integration: set SMTP4DEV_INTEGRATION=1 to enable.\n");
        check(1);
        return;
      }

      memset(&result, 0, sizeof(result));
      init_test_config(&result.cfg);

      ctx = coro_context_create(NULL);
      check_not_null(ctx);
      result.ctx = ctx;

      rc = coro_context_spawn(ctx, smtp4dev_roundtrip_coro, &result);
      check_equal(rc, 0);

      coro_context_run(ctx, TURBO_RUN_DEFAULT);
      coro_context_destroy(ctx);

      check(result.completed);
      check(result.success, "%s", result.error[0] ? result.error : "integration failed");
    }
  }
}
