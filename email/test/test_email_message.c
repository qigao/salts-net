/**
 * @file test_email_message.c
 * @brief Tests for email message construction
 */

#include "email/email_message.h"
#include "tinytest.h"
#include <string.h>

spec("email_message") {
  describe("message creation") {
    it("should create empty message") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      check(msg != NULL);
      check(msg->pool == &pool);
      check(msg->priority == EMAIL_PRIORITY_NORMAL);

      email_message_free(msg);
      mem_destroy(&pool);
    }
  }

  describe("address management") {
    it("should set from address") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      int ret = email_message_set_from(msg, "John Doe", "john@example.com");

      check(ret == 0);
      check(msg->from != NULL);
      check(strcmp(msg->from->display_name, "John Doe") == 0);
      check(strcmp(msg->from->email, "john@example.com") == 0);
      check(strcmp(msg->from->local_part, "john") == 0);
      check(strcmp(msg->from->domain, "example.com") == 0);

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should add multiple to recipients") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      email_message_add_to(msg, "Alice", "alice@example.com");
      email_message_add_to(msg, "Bob", "bob@example.com");

      check(msg->to != NULL);
      check(strcmp(msg->to->email, "alice@example.com") == 0);
      check(msg->to->next != NULL);
      check(strcmp(msg->to->next->email, "bob@example.com") == 0);

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should add cc recipients") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      email_message_add_cc(msg, "Charlie", "charlie@example.com");

      check(msg->cc != NULL);
      check(strcmp(msg->cc->email, "charlie@example.com") == 0);

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should add bcc recipients") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      email_message_add_bcc(msg, "Dave", "dave@example.com");

      check(msg->bcc != NULL);
      check(strcmp(msg->bcc->email, "dave@example.com") == 0);

      email_message_free(msg);
      mem_destroy(&pool);
    }
  }

  describe("content") {
    it("should set subject") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      email_message_set_subject(msg, "Test Subject");

      check(msg->subject != NULL);
      check(strcmp(msg->subject, "Test Subject") == 0);

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should set text body") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      email_message_set_text_body(msg, "Hello, World!");

      check(msg->text_body != NULL);
      check(strcmp(msg->text_body, "Hello, World!") == 0);

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should set html body") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      email_message_set_html_body(msg, "<html><body>Hello!</body></html>");

      check(msg->html_body != NULL);
      check(strcmp(msg->html_body, "<html><body>Hello!</body></html>") == 0);

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should set priority") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      email_message_set_priority(msg, EMAIL_PRIORITY_HIGH);

      check(msg->priority == EMAIL_PRIORITY_HIGH);

      email_message_free(msg);
      mem_destroy(&pool);
    }
  }

  describe("attachments") {
    it("should add file attachment") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      const char *data = "file content";
      int ret = email_message_add_attachment(msg, "test.txt", "text/plain",
                                              data, strlen(data));

      check(ret == 0);
      check(msg->attachment_count == 1);
      check(msg->attachments != NULL);
      check(strcmp(msg->attachments->filename, "test.txt") == 0);
      check(strcmp(msg->attachments->content_type, "text/plain") == 0);
      check(msg->attachments->inline_attachment == 0);

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should add inline attachment") {
      mem_pool_t pool;
      mem_init(&pool, 4096);

      email_message_t *msg = email_message_create(&pool);
      const char *data = "image data";
      int ret = email_message_add_inline_attachment(msg, "logo@example.com",
                                                     "logo.png", "image/png",
                                                     data, strlen(data));

      check(ret == 0);
      check(msg->attachment_count == 1);
      check(msg->attachments != NULL);
      check(strcmp(msg->attachments->content_id, "logo@example.com") == 0);
      check(msg->attachments->inline_attachment == 1);

      email_message_free(msg);
      mem_destroy(&pool);
    }
  }
}
