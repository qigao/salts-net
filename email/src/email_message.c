#include "email/email_message.h"
#include "mime_parser.h"
#include "mime_utils.h"
#include "mime_encoded_word.h"
#include "mime_rfc2231.h"
#include "mime_content_disposition.h"
#include "base64_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#endif

/* ── Helpers ───────────────────────────────────────────────────────── */

static char *generate_message_id(const char *domain) {
  char buf[256];
  time_t now = time(NULL);
  snprintf(buf, sizeof(buf), "<%ld.%d@%s>",
           (long)now, rand(), domain ? domain : "localhost");
  return strdup(buf);
}

static char *generate_boundary(void) {
  char buf[128];
  snprintf(buf, sizeof(buf), "----=_Part_%08x_%08x",
           (unsigned)time(NULL), (unsigned)rand());
  return strdup(buf);
}

/* ── Message Creation ──────────────────────────────────────────────── */

email_message_t *email_message_create(mem_pool_t *pool) {
  if (!pool) return NULL;

  email_message_t *msg = mem_alloc(pool, sizeof(email_message_t));
  if (!msg) return NULL;

  memset(msg, 0, sizeof(*msg));
  msg->pool = pool;
  msg->priority = EMAIL_PRIORITY_NORMAL;

  return msg;
}

void email_message_free(email_message_t *msg) {
  if (!msg) return;

  // Pool-based allocation, no individual frees needed
  // Just free non-pool allocations
  if (msg->smime_ctx) {
    mime_smime_ctx_free(msg->smime_ctx);
  }
}

/* ── Address Management ────────────────────────────────────────────── */

static mime_address_t *create_address(mem_pool_t *pool,
                                       const char *name,
                                       const char *email) {
  mime_address_t *addr = mem_alloc(pool, sizeof(mime_address_t));
  if (!addr) return NULL;

  memset(addr, 0, sizeof(*addr));

  if (name) {
    size_t len = strlen(name);
    addr->display_name = mem_alloc(pool, len + 1);
    memcpy(addr->display_name, name, len + 1);
  }

  if (email) {
    size_t len = strlen(email);
    addr->email = mem_alloc(pool, len + 1);
    memcpy(addr->email, email, len + 1);

    // Parse local_part and domain
    const char *at = strchr(email, '@');
    if (at) {
      size_t local_len = at - email;
      addr->local_part = mem_alloc(pool, local_len + 1);
      memcpy(addr->local_part, email, local_len);
      addr->local_part[local_len] = '\0';

      size_t domain_len = strlen(at + 1);
      addr->domain = mem_alloc(pool, domain_len + 1);
      memcpy(addr->domain, at + 1, domain_len + 1);
    }
  }

  return addr;
}

int email_message_set_from(email_message_t *msg,
                            const char *name,
                            const char *email) {
  if (!msg || !email) return -1;
  msg->from = create_address(msg->pool, name, email);
  return msg->from ? 0 : -1;
}

int email_message_add_to(email_message_t *msg,
                          const char *name,
                          const char *email) {
  if (!msg || !email) return -1;

  mime_address_t *addr = create_address(msg->pool, name, email);
  if (!addr) return -1;

  // Append to list
  if (!msg->to) {
    msg->to = addr;
  } else {
    mime_address_t *last = msg->to;
    while (last->next) last = last->next;
    last->next = addr;
  }

  return 0;
}

int email_message_add_cc(email_message_t *msg,
                          const char *name,
                          const char *email) {
  if (!msg || !email) return -1;

  mime_address_t *addr = create_address(msg->pool, name, email);
  if (!addr) return -1;

  if (!msg->cc) {
    msg->cc = addr;
  } else {
    mime_address_t *last = msg->cc;
    while (last->next) last = last->next;
    last->next = addr;
  }

  return 0;
}

int email_message_add_bcc(email_message_t *msg,
                           const char *name,
                           const char *email) {
  if (!msg || !email) return -1;

  mime_address_t *addr = create_address(msg->pool, name, email);
  if (!addr) return -1;

  if (!msg->bcc) {
    msg->bcc = addr;
  } else {
    mime_address_t *last = msg->bcc;
    while (last->next) last = last->next;
    last->next = addr;
  }

  return 0;
}

int email_message_set_reply_to(email_message_t *msg,
                                const char *name,
                                const char *email) {
  if (!msg || !email) return -1;
  msg->reply_to = create_address(msg->pool, name, email);
  return msg->reply_to ? 0 : -1;
}

/* ── Content ───────────────────────────────────────────────────────── */

int email_message_set_subject(email_message_t *msg, const char *subject) {
  if (!msg || !subject) return -1;

  size_t len = strlen(subject);
  msg->subject = mem_alloc(msg->pool, len + 1);
  if (!msg->subject) return -1;

  memcpy(msg->subject, subject, len + 1);
  return 0;
}

int email_message_set_text_body(email_message_t *msg, const char *text) {
  if (!msg || !text) return -1;

  size_t len = strlen(text);
  msg->text_body = mem_alloc(msg->pool, len + 1);
  if (!msg->text_body) return -1;

  memcpy(msg->text_body, text, len + 1);
  return 0;
}

int email_message_set_html_body(email_message_t *msg, const char *html) {
  if (!msg || !html) return -1;

  size_t len = strlen(html);
  msg->html_body = mem_alloc(msg->pool, len + 1);
  if (!msg->html_body) return -1;

  memcpy(msg->html_body, html, len + 1);
  return 0;
}

void email_message_set_priority(email_message_t *msg,
                                 email_priority_t priority) {
  if (msg) msg->priority = priority;
}

/* ── Attachments ───────────────────────────────────────────────────── */

int email_message_add_attachment(email_message_t *msg,
                                  const char *filename,
                                  const char *content_type,
                                  const char *data,
                                  size_t data_len) {
  if (!msg || !filename || !data) return -1;

  email_attachment_t *att = mem_alloc(msg->pool, sizeof(email_attachment_t));
  if (!att) return -1;

  memset(att, 0, sizeof(*att));

  size_t fn_len = strlen(filename);
  att->filename = mem_alloc(msg->pool, fn_len + 1);
  memcpy(att->filename, filename, fn_len + 1);

  if (content_type) {
    size_t ct_len = strlen(content_type);
    att->content_type = mem_alloc(msg->pool, ct_len + 1);
    memcpy(att->content_type, content_type, ct_len + 1);
  } else {
    att->content_type = mem_alloc(msg->pool, 25);
    strcpy(att->content_type, "application/octet-stream");
  }

  att->data = data;
  att->data_len = data_len;
  att->inline_attachment = 0;

  // Append to list
  if (!msg->attachments) {
    msg->attachments = att;
  } else {
    email_attachment_t *last = msg->attachments;
    while (last->next) last = last->next;
    last->next = att;
  }

  msg->attachment_count++;
  return 0;
}

int email_message_add_inline_attachment(email_message_t *msg,
                                        const char *content_id,
                                        const char *filename,
                                        const char *content_type,
                                        const char *data,
                                        size_t data_len) {
  if (!msg || !content_id || !data) return -1;

  email_attachment_t *att = mem_alloc(msg->pool, sizeof(email_attachment_t));
  if (!att) return -1;

  memset(att, 0, sizeof(*att));

  if (filename) {
    size_t fn_len = strlen(filename);
    att->filename = mem_alloc(msg->pool, fn_len + 1);
    memcpy(att->filename, filename, fn_len + 1);
  }

  if (content_type) {
    size_t ct_len = strlen(content_type);
    att->content_type = mem_alloc(msg->pool, ct_len + 1);
    memcpy(att->content_type, content_type, ct_len + 1);
  }

  size_t cid_len = strlen(content_id);
  att->content_id = mem_alloc(msg->pool, cid_len + 1);
  memcpy(att->content_id, content_id, cid_len + 1);

  att->data = data;
  att->data_len = data_len;
  att->inline_attachment = 1;

  if (!msg->attachments) {
    msg->attachments = att;
  } else {
    email_attachment_t *last = msg->attachments;
    while (last->next) last = last->next;
    last->next = att;
  }

  msg->attachment_count++;
  return 0;
}

/* ── S/MIME ────────────────────────────────────────────────────────── */

int email_message_enable_signing(email_message_t *msg,
                                  const char *cert_path,
                                  const char *key_path,
                                  const char *key_password) {
  if (!msg || !cert_path || !key_path) return -1;

  if (!msg->smime_ctx) {
    msg->smime_ctx = mime_smime_ctx_create();
    if (!msg->smime_ctx) return -1;
  }

  if (mime_smime_load_cert(msg->smime_ctx, cert_path) != MIME_SMIME_OK) {
    return -1;
  }

  if (mime_smime_load_key(msg->smime_ctx, key_path, key_password) != MIME_SMIME_OK) {
    return -1;
  }

  msg->sign_message = 1;
  return 0;
}

int email_message_enable_encryption(email_message_t *msg,
                                     const char *recipient_cert_path) {
  if (!msg || !recipient_cert_path) return -1;

  if (!msg->smime_ctx) {
    msg->smime_ctx = mime_smime_ctx_create();
    if (!msg->smime_ctx) return -1;
  }

  if (mime_smime_load_cert(msg->smime_ctx, recipient_cert_path) != MIME_SMIME_OK) {
    return -1;
  }

  msg->encrypt_message = 1;
  return 0;
}

/* ── Serialization ─────────────────────────────────────────────────── */

static void append_address_list(tstr_t *result, mime_address_t *addr) {
  int first = 1;
  while (addr) {
    if (!first) *result = tstr_cat(*result, ", ");
    first = 0;

    if (addr->display_name) {
      *result = tstr_cat_fmt(*result, "\"%s\" <%s>", addr->display_name, addr->email);
    } else {
      *result = tstr_cat(*result, addr->email);
    }
    addr = addr->next;
  }
}

static char *format_date_rfc2822(void) {
  time_t now = time(NULL);
  struct tm *tm = gmtime(&now);
  static char buf[64];

  const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d +0000",
           days[tm->tm_wday], tm->tm_mday, months[tm->tm_mon],
           tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);

  return buf;
}

tstr_t email_message_to_string(email_message_t *msg) {
  if (!msg) return NULL;

  tstr_t result = tstr_new();

  // Generate Message-ID if not set
  if (!msg->message_id && msg->from && msg->from->domain) {
    msg->message_id = generate_message_id(msg->from->domain);
  }

  // Date header
  result = tstr_cat(result, "Date: ");
  result = tstr_cat(result, format_date_rfc2822());
  result = tstr_cat(result, "\r\n");

  // From header
  if (msg->from) {
    result = tstr_cat(result, "From: ");
    append_address_list(&result, msg->from);
    result = tstr_cat(result, "\r\n");
  }

  // To header
  if (msg->to) {
    result = tstr_cat(result, "To: ");
    append_address_list(&result, msg->to);
    result = tstr_cat(result, "\r\n");
  }

  // Cc header
  if (msg->cc) {
    result = tstr_cat(result, "Cc: ");
    append_address_list(&result, msg->cc);
    result = tstr_cat(result, "\r\n");
  }

  // Subject header (TODO: encode non-ASCII with RFC 2047)
  if (msg->subject) {
    result = tstr_cat(result, "Subject: ");
    result = tstr_cat(result, msg->subject);
    result = tstr_cat(result, "\r\n");
  }

  // Message-ID
  if (msg->message_id) {
    result = tstr_cat(result, "Message-ID: ");
    result = tstr_cat(result, msg->message_id);
    result = tstr_cat(result, "\r\n");
  }

  // Reply-To
  if (msg->reply_to) {
    result = tstr_cat(result, "Reply-To: ");
    append_address_list(&result, msg->reply_to);
    result = tstr_cat(result, "\r\n");
  }

  // In-Reply-To
  if (msg->in_reply_to) {
    result = tstr_cat_fmt(result, "In-Reply-To: %s\r\n", msg->in_reply_to);
  }

  // References
  if (msg->references) {
    result = tstr_cat_fmt(result, "References: %s\r\n", msg->references);
  }

  // Priority
  if (msg->priority == EMAIL_PRIORITY_HIGH) {
    result = tstr_cat(result, "X-Priority: 1\r\n");
    result = tstr_cat(result, "Importance: high\r\n");
  } else if (msg->priority == EMAIL_PRIORITY_LOW) {
    result = tstr_cat(result, "X-Priority: 5\r\n");
    result = tstr_cat(result, "Importance: low\r\n");
  }

  // MIME-Version
  result = tstr_cat(result, "MIME-Version: 1.0\r\n");

  // Determine content structure
  int has_html = (msg->html_body != NULL);
  int has_text = (msg->text_body != NULL);
  int has_attachments = (msg->attachment_count > 0);

  if (!has_html && !has_text && !has_attachments) {
    // Empty message
    result = tstr_cat(result, "Content-Type: text/plain; charset=utf-8\r\n");
    result = tstr_cat(result, "\r\n");
    return result;
  }

  if (!has_html && !has_attachments) {
    // Simple text message
    result = tstr_cat(result, "Content-Type: text/plain; charset=utf-8\r\n");
    result = tstr_cat(result, "Content-Transfer-Encoding: 8bit\r\n");
    result = tstr_cat(result, "\r\n");
    result = tstr_cat(result, msg->text_body);
    result = tstr_cat(result, "\r\n");
    return result;
  }

  // Multipart message
  char *boundary = generate_boundary();

  if (has_attachments) {
    result = tstr_cat_fmt(result, "Content-Type: multipart/mixed; boundary=\"%s\"\r\n", boundary);
  } else {
    result = tstr_cat_fmt(result, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n", boundary);
  }

  result = tstr_cat(result, "\r\n");
  result = tstr_cat(result, "This is a multi-part message in MIME format.\r\n");

  // Text/HTML alternative
  if (has_text && has_html) {
    char *alt_boundary = generate_boundary();

    result = tstr_cat_fmt(result, "\r\n--%s\r\n", boundary);
    result = tstr_cat_fmt(result, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n", alt_boundary);
    result = tstr_cat(result, "\r\n");

    // Text part
    result = tstr_cat_fmt(result, "\r\n--%s\r\n", alt_boundary);
    result = tstr_cat(result, "Content-Type: text/plain; charset=utf-8\r\n");
    result = tstr_cat(result, "Content-Transfer-Encoding: 8bit\r\n");
    result = tstr_cat(result, "\r\n");
    result = tstr_cat(result, msg->text_body);
    result = tstr_cat(result, "\r\n");

    // HTML part
    result = tstr_cat_fmt(result, "\r\n--%s\r\n", alt_boundary);
    result = tstr_cat(result, "Content-Type: text/html; charset=utf-8\r\n");
    result = tstr_cat(result, "Content-Transfer-Encoding: 8bit\r\n");
    result = tstr_cat(result, "\r\n");
    result = tstr_cat(result, msg->html_body);
    result = tstr_cat(result, "\r\n");

    result = tstr_cat_fmt(result, "\r\n--%s--\r\n", alt_boundary);
    free(alt_boundary);
  } else if (has_text) {
    result = tstr_cat_fmt(result, "\r\n--%s\r\n", boundary);
    result = tstr_cat(result, "Content-Type: text/plain; charset=utf-8\r\n");
    result = tstr_cat(result, "Content-Transfer-Encoding: 8bit\r\n");
    result = tstr_cat(result, "\r\n");
    result = tstr_cat(result, msg->text_body);
    result = tstr_cat(result, "\r\n");
  } else if (has_html) {
    result = tstr_cat_fmt(result, "\r\n--%s\r\n", boundary);
    result = tstr_cat(result, "Content-Type: text/html; charset=utf-8\r\n");
    result = tstr_cat(result, "Content-Transfer-Encoding: 8bit\r\n");
    result = tstr_cat(result, "\r\n");
    result = tstr_cat(result, msg->html_body);
    result = tstr_cat(result, "\r\n");
  }

  // Attachments
  if (has_attachments) {
    email_attachment_t *att = msg->attachments;
    while (att) {
      result = tstr_cat_fmt(result, "\r\n--%s\r\n", boundary);
      result = tstr_cat_fmt(result, "Content-Type: %s\r\n",
                           att->content_type ? att->content_type : "application/octet-stream");

      if (att->inline_attachment && att->content_id) {
        result = tstr_cat_fmt(result, "Content-ID: <%s>\r\n", att->content_id);
        result = tstr_cat(result, "Content-Disposition: inline");
      } else {
        result = tstr_cat(result, "Content-Disposition: attachment");
      }

      if (att->filename) {
        result = tstr_cat_fmt(result, "; filename=\"%s\"", att->filename);
      }
      result = tstr_cat(result, "\r\n");

      result = tstr_cat(result, "Content-Transfer-Encoding: base64\r\n");
      result = tstr_cat(result, "\r\n");

      // Base64 encode attachment data
      char *encoded = NULL;
      if (tn_base64_encode((uint8_t *)att->data, att->data_len, &encoded) == 0) {
        result = tstr_cat(result, encoded);
        free(encoded);
      }
      result = tstr_cat(result, "\r\n");

      att = att->next;
    }
  }

  // Final boundary
  result = tstr_cat_fmt(result, "\r\n--%s--\r\n", boundary);
  free(boundary);

  return result;
}

/* ── Parsing ───────────────────────────────────────────────────────── */

typedef struct {
  email_message_t *msg;
  mem_pool_t *pool;
  char *current_header_field;
  size_t current_header_field_len;
  int in_body;
} parse_context_t;

static int on_header_field(mime_parser_t *parser, const char *data, size_t len) {
  parse_context_t *ctx = (parse_context_t *)parser->data;

  // Store field name for later use
  ctx->current_header_field = mem_alloc(ctx->pool, len + 1);
  memcpy(ctx->current_header_field, data, len);
  ctx->current_header_field[len] = '\0';
  ctx->current_header_field_len = len;

  return 0;
}

static int on_header_value(mime_parser_t *parser, const char *data, size_t len) {
  parse_context_t *ctx = (parse_context_t *)parser->data;

  if (!ctx->current_header_field) return 0;

  char *value = mem_alloc(ctx->pool, len + 1);
  memcpy(value, data, len);
  value[len] = '\0';

  // Parse common headers
  if (strncasecmp(ctx->current_header_field, "From", 4) == 0) {
    // Parse "Name <email@example.com>"
    char *email_start = strchr(value, '<');
    if (email_start) {
      char *email_end = strchr(email_start, '>');
      if (email_end) {
        *email_end = '\0';
        char *name = value;
        char *email = email_start + 1;

        // Trim name
        while (*name == ' ') name++;
        char *name_end = email_start - 1;
        while (name_end > name && *name_end == ' ') name_end--;
        *(name_end + 1) = '\0';

        email_message_set_from(ctx->msg, name, email);
      }
    } else {
      email_message_set_from(ctx->msg, NULL, value);
    }
  }
  else if (strncasecmp(ctx->current_header_field, "To", 2) == 0) {
    // Simple parsing - just extract email
    char *email_start = strchr(value, '<');
    if (email_start) {
      char *email_end = strchr(email_start, '>');
      if (email_end) {
        *email_end = '\0';
        email_message_add_to(ctx->msg, NULL, email_start + 1);
      }
    } else {
      email_message_add_to(ctx->msg, NULL, value);
    }
  }
  else if (strncasecmp(ctx->current_header_field, "Subject", 7) == 0) {
    email_message_set_subject(ctx->msg, value);
  }
  else if (strncasecmp(ctx->current_header_field, "Message-ID", 10) == 0) {
    ctx->msg->message_id = mem_strdup(ctx->pool, value);
  }

  return 0;
}

static int on_headers_complete(mime_parser_t *parser) {
  parse_context_t *ctx = (parse_context_t *)parser->data;
  ctx->in_body = 1;
  return 0;
}

static int on_body(mime_parser_t *parser, const char *data, size_t len) {
  parse_context_t *ctx = (parse_context_t *)parser->data;

  if (!ctx->in_body) return 0;

  // Accumulate body data
  if (!ctx->msg->text_body) {
    ctx->msg->text_body = mem_alloc(ctx->pool, len + 1);
    memcpy(ctx->msg->text_body, data, len);
    ctx->msg->text_body[len] = '\0';
  } else {
    // Append to existing body
    size_t old_len = strlen(ctx->msg->text_body);
    char *new_body = mem_alloc(ctx->pool, old_len + len + 1);
    memcpy(new_body, ctx->msg->text_body, old_len);
    memcpy(new_body + old_len, data, len);
    new_body[old_len + len] = '\0';
    ctx->msg->text_body = new_body;
  }

  return 0;
}

email_message_t *email_message_parse(mem_pool_t *pool,
                                      const char *raw_message,
                                      size_t len) {
  if (!pool || !raw_message) return NULL;

  email_message_t *msg = email_message_create(pool);
  if (!msg) return NULL;

  parse_context_t ctx = {0};
  ctx.msg = msg;
  ctx.pool = pool;
  ctx.in_body = 0;

  mime_settings_t settings = {0};
  settings.on_header_field = on_header_field;
  settings.on_header_value = on_header_value;
  settings.on_headers_complete = on_headers_complete;
  settings.on_body = on_body;

  mime_parser_t parser;
  mime_parser_init(&parser, &settings, pool);
  parser.data = &ctx;

  mime_errno_t err = mime_parse(&parser, raw_message, len);
  if (err != MIME_OK) {
    return NULL;
  }

  return msg;
}
