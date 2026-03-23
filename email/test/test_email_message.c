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

  describe("parsing") {
    it("should parse simple plain text message") {
      const char *raw =
          "From: John Doe <john@example.com>\r\n"
          "To: Alice <alice@example.com>\r\n"
          "Subject: Hello World\r\n"
          "Message-ID: <msg-1@example.com>\r\n"
          "Content-Type: text/plain; charset=utf-8\r\n"
          "\r\n"
          "Plain body";
      mem_pool_t pool;
      mem_init(&pool, 8192);

      email_message_t *msg = email_message_parse(&pool, raw, strlen(raw));
      check(msg != NULL);
      check(msg->from != NULL);
      check_str_eq(msg->from->email, "john@example.com");
      check(msg->to != NULL);
      check_str_eq(msg->to->email, "alice@example.com");
      check_str_eq(msg->subject, "Hello World");
      check_str_eq(msg->message_id, "<msg-1@example.com>");
      check_str_eq(msg->text_body, "Plain body");
      check_null(msg->html_body);

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should parse multipart alternative message") {
      const char *raw =
          "From: Demo <sender@example.com>\r\n"
          "To: Receiver <receiver@example.com>\r\n"
          "Subject: Multipart Example\r\n"
          "Content-Type: multipart/alternative; boundary=\"outer\"\r\n"
          "\r\n"
          "--outer\r\n"
          "Content-Type: text/plain; charset=utf-8\r\n"
          "\r\n"
          "Plain section\r\n"
          "--outer\r\n"
          "Content-Type: text/html; charset=utf-8\r\n"
          "\r\n"
          "<html><body><p>HTML section</p></body></html>\r\n"
          "--outer--\r\n";
      mem_pool_t pool;
      mem_init(&pool, 8192);

      email_message_t *msg = email_message_parse(&pool, raw, strlen(raw));
      check(msg != NULL);
      check_str_eq(msg->subject, "Multipart Example");
      check_str_eq(msg->text_body, "Plain section");
      check_str_eq(msg->html_body, "<html><body><p>HTML section</p></body></html>");

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should parse nested multipart alternative message") {
      const char *raw =
          "From: Demo <sender@example.com>\r\n"
          "To: Receiver <receiver@example.com>\r\n"
          "Subject: Nested Multipart Example\r\n"
          "Content-Type: multipart/alternative; boundary=\"outer\"\r\n"
          "\r\n"
          "--outer\r\n"
          "Content-Type: multipart/alternative; boundary=\"inner\"\r\n"
          "\r\n"
          "--inner\r\n"
          "Content-Type: text/plain; charset=utf-8\r\n"
          "\r\n"
          "Nested plain section\r\n"
          "--inner\r\n"
          "Content-Type: text/html; charset=utf-8\r\n"
          "\r\n"
          "<html><body><p>Nested HTML section</p></body></html>\r\n"
          "--inner--\r\n"
          "--outer--\r\n";
      mem_pool_t pool;
      mem_init(&pool, 8192);

      email_message_t *msg = email_message_parse(&pool, raw, strlen(raw));
      check(msg != NULL);
      check_str_eq(msg->subject, "Nested Multipart Example");
      check_str_eq(msg->text_body, "Nested plain section");
      check_str_eq(msg->html_body,
                   "<html><body><p>Nested HTML section</p></body></html>");

      email_message_free(msg);
      mem_destroy(&pool);
    }

    it("should parse attachment metadata and payload") {
      const char *raw =
          "From: Demo <sender@example.com>\r\n"
          "To: Receiver <receiver@example.com>\r\n"
          "Subject: Attachment Example\r\n"
          "Content-Type: multipart/mixed; boundary=\"mix\"\r\n"
          "\r\n"
          "--mix\r\n"
          "Content-Type: text/plain; charset=utf-8\r\n"
          "\r\n"
          "Body text\r\n"
          "--mix\r\n"
          "Content-Type: application/octet-stream\r\n"
          "Content-Disposition: attachment; filename=\"note.txt\"\r\n"
          "Content-Transfer-Encoding: base64\r\n"
          "\r\n"
          "aGVsbG8=\r\n"
          "--mix--\r\n";
      mem_pool_t pool;
      mem_init(&pool, 8192);

      email_message_t *msg = email_message_parse(&pool, raw, strlen(raw));
      check(msg != NULL);
      check_str_eq(msg->text_body, "Body text");
      check_int_eq(msg->attachment_count, 1);
      check(msg->attachments != NULL);
      check_str_eq(msg->attachments->filename, "note.txt");
      check_str_eq(msg->attachments->content_type, "application/octet-stream");
      check_str_eq(msg->attachments->data, "hello");

      email_message_free(msg);
      mem_destroy(&pool);
    }
  }
}
