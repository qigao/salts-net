#include "email/email_message.h"
#include "mime_parser.h"
#include "mime_utils.h"
#include "mime_encoded_word.h"
#include "mime_rfc2231.h"
#include "mime_content_disposition.h"
#include "base64_utils.h"
#include <fmt.h>
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
  fmt(buf, sizeof(buf), "<{}.{}@{}>", (long)now, rand(), domain ? domain : "localhost");
  return strdup(buf);
}

static char *generate_boundary(void) {
  char buf[128];
  fmt(buf, sizeof(buf), "----=_Part_{:08x}_{:08x}", (unsigned)time(NULL), (unsigned)rand());
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

/* RFC 5322 §2.2.3: soft limit 78, hard limit 998 characters per line.
 * field_offset: length already on the current line before this value starts
 * (e.g., strlen("Subject: ") == 9). Folds at WSP boundaries. */
static char *fold_header_value(mem_pool_t *pool, const char *value,
                               size_t field_offset) {
  size_t len = strlen(value);
  /* Fast path: value fits on one line together with the field name. */
  if (field_offset + len <= 78) {
    char *copy = mem_alloc(pool, len + 1);
    if (!copy) return NULL;
    memcpy(copy, value, len + 1);
    return copy;
  }

  /* Worst case: every character plus occasional CRLF+TAB. */
  char *out = mem_alloc(pool, len * 3 + 4);
  if (!out) return NULL;

  size_t col = field_offset; /* current column (0-based chars since last \n) */
  size_t in  = 0;
  size_t op  = 0;

  while (in < len) {
    /* Find next WSP-delimited token end */
    size_t tok_start = in;
    /* Skip leading WSP (preserve it) */
    while (in < len && (value[in] == ' ' || value[in] == '\t')) in++;
    /* Advance to end of token */
    while (in < len && value[in] != ' ' && value[in] != '\t') in++;
    size_t tok_len = in - tok_start;

    if (col + tok_len > 78 && col > 0) {
      /* Fold before this token: emit CRLF + TAB */
      out[op++] = '\r';
      out[op++] = '\n';
      out[op++] = '\t';
      col = 1; /* TAB counts as 1 */
      /* Skip the leading space of the token since we replaced it with fold */
      size_t skip = tok_start;
      while (skip < in && (value[skip] == ' ' || value[skip] == '\t')) skip++;
      tok_len = in - skip;
      tok_start = skip;
    }

    memcpy(out + op, value + tok_start, tok_len);
    op  += tok_len;
    col += tok_len;
  }
  out[op] = '\0';
  return out;
}

/* Append a formatted address to *result, folding at ", " boundaries.
 * field_len: byte length of "Field: " prefix (used to track column position). */
static void append_address_list(tstr_t *result, email_message_t *msg,
                                mime_address_t *addr, size_t field_len) {
  /* col tracks characters on the current line since (and including) "Field: " */
  size_t col = field_len;
  int first = 1;

  while (addr) {
    /* Build address token */
    tstr_t tok = tstr_new();
    if (addr->display_name && addr->display_name[0] != '\0') {
      char *enc = mime_encode_header_if_needed(
          msg->pool, addr->display_name, strlen(addr->display_name));
      if (enc && strncmp(enc, "=?", 2) == 0) {
        tok = tstr_append_format(tok, "{} <{}>", enc, addr->email);
      } else {
        tstr_t escaped = tstr_new();
        size_t dn_len = strlen(addr->display_name);
        for (size_t i = 0; i < dn_len; i++) {
          char c = addr->display_name[i];
          if (c == '"' || c == '\\') {
            escaped = tstr_append_format(escaped, "\\{}", c);
          } else {
            escaped = tstr_append_format(escaped, "{}", c);
          }
        }
        tok = tstr_append_format(tok, "\"{}\" <{}>", escaped, addr->email);
        tstr_free(escaped);
      }
    } else {
      tok = tstr_cat(tok, addr->email);
    }

    size_t tok_len = tstr_len(tok);

    if (!first) {
      /* Need ", " separator (2 chars). Fold if adding separator + token exceeds limit. */
      if (col + 2 + tok_len > 78) {
        /* Fold: emit ",\r\n\t" instead of ", " */
        *result = tstr_cat(*result, ",\r\n\t");
        col = 1; /* TAB */
      } else {
        *result = tstr_cat(*result, ", ");
        col += 2;
      }
    }

    *result = tstr_cat(*result, tok);
    col += tok_len;
    tstr_free(tok);
    first = 0;
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

  fmt(buf, sizeof(buf), "{}, {:02d} {} {:04d} {:02d}:{:02d}:{:02d} +0000", days[tm->tm_wday],
      tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);

  return buf;
}

tstr_t email_message_to_string(email_message_t *msg) {
  if (!msg || !msg->from) return NULL;

  tstr_t result = tstr_new();

  // Generate Message-ID if not set
  if (!msg->message_id && msg->from && msg->from->domain) {
    msg->message_id = generate_message_id(msg->from->domain);
  }

  // Date header
  result = tstr_cat(result, "Date: ");
  result = tstr_cat(result, format_date_rfc2822());
  result = tstr_cat(result, "\r\n");

  // From header ("From: " = 6 chars)
  if (msg->from) {
    result = tstr_cat(result, "From: ");
    append_address_list(&result, msg, msg->from, 6);
    result = tstr_cat(result, "\r\n");
  }

  // To header ("To: " = 4 chars)
  if (msg->to) {
    result = tstr_cat(result, "To: ");
    append_address_list(&result, msg, msg->to, 4);
    result = tstr_cat(result, "\r\n");
  }

  // Cc header ("Cc: " = 4 chars)
  if (msg->cc) {
    result = tstr_cat(result, "Cc: ");
    append_address_list(&result, msg, msg->cc, 4);
    result = tstr_cat(result, "\r\n");
  }

  // Subject header (encode non-ASCII with RFC 2047, then fold if needed)
  if (msg->subject) {
    result = tstr_cat(result, "Subject: ");
    char *enc_subj = mime_encode_header_if_needed(
        msg->pool, msg->subject, strlen(msg->subject));
    const char *subj_val = enc_subj ? enc_subj : msg->subject;
    /* encoded-word sequence already has spaces as fold points;
     * for plain ASCII apply WSP folding ("Subject: " = 9 chars). */
    char *folded = fold_header_value(msg->pool, subj_val, 9);
    result = tstr_cat(result, folded ? folded : subj_val);
    result = tstr_cat(result, "\r\n");
  }

  // Message-ID
  if (msg->message_id) {
    result = tstr_cat(result, "Message-ID: ");
    result = tstr_cat(result, msg->message_id);
    result = tstr_cat(result, "\r\n");
  }

  // Reply-To ("Reply-To: " = 10 chars)
  if (msg->reply_to) {
    result = tstr_cat(result, "Reply-To: ");
    append_address_list(&result, msg, msg->reply_to, 10);
    result = tstr_cat(result, "\r\n");
  }

  // In-Reply-To
  if (msg->in_reply_to) {
    result = tstr_append_format(result, "In-Reply-To: {}\r\n", msg->in_reply_to);
  }

  // References
  if (msg->references) {
    result = tstr_append_format(result, "References: {}\r\n", msg->references);
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
    result = tstr_append_format(result, "Content-Type: multipart/mixed; boundary=\"{}\"\r\n", boundary);
  } else {
    result = tstr_append_format(result, "Content-Type: multipart/alternative; boundary=\"{}\"\r\n", boundary);
  }

  result = tstr_cat(result, "\r\n");
  result = tstr_cat(result, "This is a multi-part message in MIME format.\r\n");

  // Text/HTML alternative
  if (has_text && has_html) {
    char *alt_boundary = generate_boundary();

    result = tstr_append_format(result, "\r\n--{}\r\n", boundary);
    result = tstr_append_format(result, "Content-Type: multipart/alternative; boundary=\"{}\"\r\n", alt_boundary);
    result = tstr_cat(result, "\r\n");

    // Text part
    result = tstr_append_format(result, "\r\n--{}\r\n", alt_boundary);
    result = tstr_cat(result, "Content-Type: text/plain; charset=utf-8\r\n");
    result = tstr_cat(result, "Content-Transfer-Encoding: 8bit\r\n");
    result = tstr_cat(result, "\r\n");
    result = tstr_cat(result, msg->text_body);
    result = tstr_cat(result, "\r\n");

    // HTML part
    result = tstr_append_format(result, "\r\n--{}\r\n", alt_boundary);
    result = tstr_cat(result, "Content-Type: text/html; charset=utf-8\r\n");
    result = tstr_cat(result, "Content-Transfer-Encoding: 8bit\r\n");
    result = tstr_cat(result, "\r\n");
    result = tstr_cat(result, msg->html_body);
    result = tstr_cat(result, "\r\n");

    result = tstr_append_format(result, "\r\n--{}--\r\n", alt_boundary);
    free(alt_boundary);
  } else if (has_text) {
    result = tstr_append_format(result, "\r\n--{}\r\n", boundary);
    result = tstr_cat(result, "Content-Type: text/plain; charset=utf-8\r\n");
    result = tstr_cat(result, "Content-Transfer-Encoding: 8bit\r\n");
    result = tstr_cat(result, "\r\n");
    result = tstr_cat(result, msg->text_body);
    result = tstr_cat(result, "\r\n");
  } else if (has_html) {
    result = tstr_append_format(result, "\r\n--{}\r\n", boundary);
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
      result = tstr_append_format(result, "\r\n--{}\r\n", boundary);
      result = tstr_append_format(result, "Content-Type: {}\r\n",
                           att->content_type ? att->content_type : "application/octet-stream");

      if (att->inline_attachment && att->content_id) {
        result = tstr_append_format(result, "Content-ID: <{}>\r\n", att->content_id);
        result = tstr_cat(result, "Content-Disposition: inline");
      } else {
        result = tstr_cat(result, "Content-Disposition: attachment");
      }

      if (att->filename) {
        char *enc_fn = mime_encode_rfc2231_filename(msg->pool, att->filename, strlen(att->filename));
        if (enc_fn) {
          result = tstr_append_format(result, "; filename*={}", enc_fn);
        } else {
          result = tstr_append_format(result, "; filename=\"{}\"", att->filename);
        }
      }
      result = tstr_cat(result, "\r\n");

      result = tstr_cat(result, "Content-Transfer-Encoding: base64\r\n");
      result = tstr_cat(result, "\r\n");

      // Base64 encode attachment data with line folding
      char *encoded = NULL;
      if (tn_base64_encode((uint8_t *)att->data, att->data_len, &encoded) == 0) {
        char *folded = mime_base64_fold(msg->pool, encoded, strlen(encoded));
        result = tstr_cat(result, folded ? folded : encoded);
        free(encoded);
      }
      result = tstr_cat(result, "\r\n");

      att = att->next;
    }
  }

  // Final boundary
  result = tstr_append_format(result, "\r\n--{}--\r\n", boundary);
  free(boundary);

  return result;
}

/* ── Parsing ───────────────────────────────────────────────────────── */

typedef struct {
  const char *content_type;
  size_t content_type_len;
  const char *content_transfer_encoding;
  size_t content_transfer_encoding_len;
  const char *content_disposition;
  size_t content_disposition_len;
  const char *content_id;
  size_t content_id_len;
} mime_part_headers_t;

static char *dup_range(mem_pool_t *pool, const char *data, size_t len) {
  char *result;
  if (!pool || !data) return NULL;

  result = mem_alloc(pool, len + 1);
  if (!result) return NULL;

  memcpy(result, data, len);
  result[len] = '\0';
  return result;
}

static void trim_ascii_whitespace(const char **data, size_t *len) {
  const char *start = *data;
  size_t length = *len;

  while (length > 0 && (*start == ' ' || *start == '\t' ||
                        *start == '\r' || *start == '\n')) {
    start++;
    length--;
  }

  while (length > 0 &&
         (start[length - 1] == ' ' || start[length - 1] == '\t' ||
          start[length - 1] == '\r' || start[length - 1] == '\n')) {
    length--;
  }

  *data = start;
  *len = length;
}

static int header_name_eq(const char *field, size_t field_len,
                          const char *expected) {
  size_t i;
  size_t expected_len = strlen(expected);
  if (field_len != expected_len) return 0;

  for (i = 0; i < field_len; i++) {
    if (tolower((unsigned char)field[i]) != tolower((unsigned char)expected[i])) {
      return 0;
    }
  }
  return 1;
}

static int split_headers_and_body(const char *raw_message, size_t len,
                                  const char **headers, size_t *headers_len,
                                  const char **body, size_t *body_len) {
  size_t i;
  if (!raw_message || !headers || !headers_len || !body || !body_len) return -1;

  for (i = 0; i + 3 < len; i++) {
    if (raw_message[i] == '\r' && raw_message[i + 1] == '\n' &&
        raw_message[i + 2] == '\r' && raw_message[i + 3] == '\n') {
      *headers = raw_message;
      *headers_len = i;
      *body = raw_message + i + 4;
      *body_len = len - i - 4;
      return 0;
    }
  }

  for (i = 0; i + 1 < len; i++) {
    if (raw_message[i] == '\n' && raw_message[i + 1] == '\n') {
      *headers = raw_message;
      *headers_len = i;
      *body = raw_message + i + 2;
      *body_len = len - i - 2;
      return 0;
    }
  }

  *headers = raw_message;
  *headers_len = len;
  *body = raw_message + len;
  *body_len = 0;
  return 0;
}

static void assign_address_list(mime_address_t **dest, mem_pool_t *pool,
                                const char *value, size_t value_len) {
  if (!dest || *dest || !value || value_len == 0) return;
  *dest = mime_parse_address_list(pool, value, value_len);
}

static void assign_decoded_header(char **dest, mem_pool_t *pool,
                                  const char *value, size_t value_len) {
  char *decoded;
  if (!dest || *dest || !value || value_len == 0) return;

  decoded = mime_decode_header(pool, value, value_len);
  if (decoded) {
    *dest = decoded;
  } else {
    *dest = dup_range(pool, value, value_len);
  }
}

static void parse_headers_block(email_message_t *msg, mem_pool_t *pool,
                                const char *headers, size_t headers_len,
                                mime_part_headers_t *part_headers,
                                int store_message_headers) {
  const char *cursor = headers;
  const char *end = headers + headers_len;

  if (part_headers) {
    memset(part_headers, 0, sizeof(*part_headers));
  }

  while (cursor < end) {
    const char *line_start = cursor;
    const char *line_end = memchr(line_start, '\n', (size_t)(end - line_start));
    size_t line_len;
    const char *colon;
    const char *field;
    const char *value;
    size_t field_len;
    size_t value_len;
    char *stored_value;

    if (!line_end) {
      line_end = end;
      cursor = end;
    } else {
      cursor = line_end + 1;
    }

    line_len = (size_t)(line_end - line_start);
    if (line_len > 0 && line_start[line_len - 1] == '\r') {
      line_len--;
    }
    if (line_len == 0) {
      continue;
    }

    if (*line_start == ' ' || *line_start == '\t') {
      continue;
    }

    colon = memchr(line_start, ':', line_len);
    if (!colon) {
      continue;
    }

    field = line_start;
    field_len = (size_t)(colon - field);
    value = colon + 1;
    value_len = line_len - field_len - 1;

    trim_ascii_whitespace(&field, &field_len);
    trim_ascii_whitespace(&value, &value_len);
    if (field_len == 0) {
      continue;
    }

    stored_value = dup_range(pool, value, value_len);
    if (!stored_value) {
      continue;
    }

    if (part_headers) {
      if (header_name_eq(field, field_len, "Content-Type")) {
        part_headers->content_type = stored_value;
        part_headers->content_type_len = strlen(stored_value);
      } else if (header_name_eq(field, field_len, "Content-Transfer-Encoding")) {
        part_headers->content_transfer_encoding = stored_value;
        part_headers->content_transfer_encoding_len = strlen(stored_value);
      } else if (header_name_eq(field, field_len, "Content-Disposition")) {
        part_headers->content_disposition = stored_value;
        part_headers->content_disposition_len = strlen(stored_value);
      } else if (header_name_eq(field, field_len, "Content-ID")) {
        part_headers->content_id = stored_value;
        part_headers->content_id_len = strlen(stored_value);
      }
    }

    if (!store_message_headers) {
      continue;
    }

    if (header_name_eq(field, field_len, "From")) {
      assign_address_list(&msg->from, pool, stored_value, strlen(stored_value));
    } else if (header_name_eq(field, field_len, "To")) {
      assign_address_list(&msg->to, pool, stored_value, strlen(stored_value));
    } else if (header_name_eq(field, field_len, "Cc")) {
      assign_address_list(&msg->cc, pool, stored_value, strlen(stored_value));
    } else if (header_name_eq(field, field_len, "Bcc")) {
      assign_address_list(&msg->bcc, pool, stored_value, strlen(stored_value));
    } else if (header_name_eq(field, field_len, "Reply-To")) {
      assign_address_list(&msg->reply_to, pool, stored_value, strlen(stored_value));
    } else if (header_name_eq(field, field_len, "Subject")) {
      assign_decoded_header(&msg->subject, pool, stored_value, strlen(stored_value));
    } else if (header_name_eq(field, field_len, "Message-ID")) {
      if (!msg->message_id) msg->message_id = stored_value;
    } else if (header_name_eq(field, field_len, "In-Reply-To")) {
      if (!msg->in_reply_to) msg->in_reply_to = stored_value;
    } else if (header_name_eq(field, field_len, "References")) {
      if (!msg->references) msg->references = stored_value;
    }
  }
}

static void trim_part_payload(const char **data, size_t *len) {
  const char *start = *data;
  size_t length = *len;

  while (length > 0 && (*start == '\r' || *start == '\n')) {
    start++;
    length--;
  }

  while (length > 0 &&
         (start[length - 1] == '\r' || start[length - 1] == '\n')) {
    length--;
  }

  *data = start;
  *len = length;
}

static int find_boundary_line(const char *data, size_t len,
                              const char *marker, size_t marker_len,
                              size_t start_offset) {
  size_t i;
  if (!data || !marker) return -1;

  for (i = start_offset; i + marker_len <= len; i++) {
    if (memcmp(data + i, marker, marker_len) != 0) {
      continue;
    }
    if (i == 0 || data[i - 1] == '\n') {
      return (int)i;
    }
  }
  return -1;
}

static void append_attachment(email_message_t *msg, const char *content_type,
                              const char *content_id,
                              const char *filename,
                              int inline_attachment,
                              const char *data, size_t data_len) {
  email_attachment_t *att;
  if (!msg || !data) return;

  att = mem_alloc(msg->pool, sizeof(*att));
  if (!att) return;
  memset(att, 0, sizeof(*att));

  att->filename = filename ? mem_strdup(msg->pool, filename) : NULL;
  att->content_type = content_type ? mem_strdup(msg->pool, content_type)
                                   : mem_strdup(msg->pool, "application/octet-stream");
  att->content_id = content_id ? mem_strdup(msg->pool, content_id) : NULL;
  att->data = data;
  att->data_len = data_len;
  att->inline_attachment = inline_attachment;

  if (!msg->attachments) {
    msg->attachments = att;
  } else {
    email_attachment_t *tail = msg->attachments;
    while (tail->next) tail = tail->next;
    tail->next = att;
  }
  msg->attachment_count++;
}

static void assign_decoded_body(char **dest, mem_pool_t *pool,
                                const char *data, size_t len,
                                mime_encoding_t encoding) {
  char *decoded = NULL;
  size_t decoded_len = 0;
  const char *trimmed = data;
  size_t trimmed_len = len;

  if (!dest || *dest || !data) return;

  trim_part_payload(&trimmed, &trimmed_len);
  if (trimmed_len == 0) {
    *dest = mem_strdup(pool, "");
    return;
  }

  if (mime_decode_body(pool, trimmed, trimmed_len, encoding,
                       &decoded, &decoded_len) == 0 && decoded) {
    decoded[decoded_len] = '\0';
    *dest = decoded;
    return;
  }

  *dest = dup_range(pool, trimmed, trimmed_len);
}

static void parse_mime_entity(email_message_t *msg, mem_pool_t *pool,
                              const char *raw_message, size_t len,
                              int store_message_headers, int depth);

static void parse_multipart_body(email_message_t *msg, mem_pool_t *pool,
                                 const char *body, size_t body_len,
                                 const char *boundary, size_t boundary_len,
                                 int depth) {
  char marker[MIME_MAX_BOUNDARY_LEN + 3];
  size_t marker_len;
  int boundary_pos;

  if (!body || !boundary || boundary_len == 0) return;

  marker[0] = '-';
  marker[1] = '-';
  memcpy(marker + 2, boundary, boundary_len);
  marker_len = boundary_len + 2;
  marker[marker_len] = '\0';

  boundary_pos = find_boundary_line(body, body_len, marker, marker_len, 0);
  while (boundary_pos >= 0) {
    size_t cursor = (size_t)boundary_pos + marker_len;
    int closing = 0;
    size_t part_start;
    int next_boundary;
    const char *part_ptr;
    size_t part_len;

    if (cursor + 1 < body_len && body[cursor] == '-' && body[cursor + 1] == '-') {
      break;
    }

    while (cursor < body_len && body[cursor] != '\n') {
      cursor++;
    }
    if (cursor < body_len) cursor++;

    part_start = cursor;
    next_boundary = find_boundary_line(body, body_len, marker, marker_len, part_start);
    if (next_boundary < 0) {
      break;
    }

    cursor = (size_t)next_boundary + marker_len;
    if (cursor + 1 < body_len && body[cursor] == '-' && body[cursor + 1] == '-') {
      closing = 1;
    }

    part_ptr = body + part_start;
    part_len = (size_t)next_boundary - part_start;
    trim_part_payload(&part_ptr, &part_len);
    if (part_len > 0) {
      parse_mime_entity(msg, pool, part_ptr, part_len, 0, depth + 1);
    }

    if (closing) {
      break;
    }
    boundary_pos = next_boundary;
  }
}

static void parse_singlepart_body(email_message_t *msg, mem_pool_t *pool,
                                  const mime_part_headers_t *part_headers,
                                  const char *body, size_t body_len) {
  mime_content_type_t type_info;
  mime_content_disposition_t disp_info;
  mime_encoding_t encoding = MIME_ENCODING_8BIT;
  const char *content_type = "text/plain";
  const char *content_id = NULL;
  const char *filename = NULL;
  int is_attachment = 0;
  int is_inline = 0;

  if (part_headers->content_transfer_encoding) {
    encoding = mime_parse_encoding(part_headers->content_transfer_encoding,
                                   part_headers->content_transfer_encoding_len);
  }

  if (part_headers->content_type) {
    content_type = part_headers->content_type;
    if (mime_parse_content_type(part_headers->content_type,
                                part_headers->content_type_len,
                                &type_info) != 0) {
      memset(&type_info, 0, sizeof(type_info));
    }
  } else {
    memset(&type_info, 0, sizeof(type_info));
  }

  if (part_headers->content_disposition &&
      mime_parse_content_disposition(part_headers->content_disposition,
                                     part_headers->content_disposition_len,
                                     &disp_info) == 0) {
    filename = mime_disposition_get_filename(pool, &disp_info);
    is_attachment = (disp_info.type == MIME_DISPOSITION_ATTACHMENT) ||
                    (filename != NULL);
    is_inline = (disp_info.type == MIME_DISPOSITION_INLINE);
  } else {
    memset(&disp_info, 0, sizeof(disp_info));
  }

  if (part_headers->content_id) {
    content_id = part_headers->content_id;
    if (part_headers->content_id_len >= 2 &&
        content_id[0] == '<' &&
        content_id[part_headers->content_id_len - 1] == '>') {
      content_id = dup_range(pool, content_id + 1,
                             part_headers->content_id_len - 2);
    } else {
      content_id = dup_range(pool, content_id, part_headers->content_id_len);
    }
  }

  if (!is_attachment && type_info.type && type_info.subtype) {
    if (type_info.type_len == 4 &&
        strncasecmp(type_info.type, "text", 4) == 0 &&
        type_info.subtype_len == 5 &&
        strncasecmp(type_info.subtype, "plain", 5) == 0) {
      assign_decoded_body(&msg->text_body, pool, body, body_len, encoding);
      return;
    }

    if (type_info.type_len == 4 &&
        strncasecmp(type_info.type, "text", 4) == 0 &&
        type_info.subtype_len == 4 &&
        strncasecmp(type_info.subtype, "html", 4) == 0) {
      assign_decoded_body(&msg->html_body, pool, body, body_len, encoding);
      return;
    }
  }

  {
    char *decoded = NULL;
    size_t decoded_len = 0;
    const char *payload = body;
    size_t payload_len = body_len;
    trim_part_payload(&payload, &payload_len);

    if (mime_decode_body(pool, payload, payload_len, encoding,
                         &decoded, &decoded_len) != 0 || !decoded) {
      decoded = dup_range(pool, payload, payload_len);
      decoded_len = decoded ? strlen(decoded) : 0;
    } else {
      decoded[decoded_len] = '\0';
    }

    append_attachment(msg, content_type, content_id, filename,
                      is_inline, decoded, decoded_len);
  }
}

static void parse_mime_entity(email_message_t *msg, mem_pool_t *pool,
                              const char *raw_message, size_t len,
                              int store_message_headers, int depth) {
  const char *headers = NULL;
  const char *body = NULL;
  size_t headers_len = 0;
  size_t body_len = 0;
  mime_part_headers_t part_headers;
  mime_content_type_t type_info;

  if (!msg || !pool || !raw_message || depth > MIME_MAX_NESTING) return;
  if (split_headers_and_body(raw_message, len, &headers, &headers_len, &body, &body_len) != 0) {
    return;
  }

  parse_headers_block(msg, pool, headers, headers_len, &part_headers, store_message_headers);

  if (part_headers.content_type &&
      mime_parse_content_type(part_headers.content_type,
                              part_headers.content_type_len,
                              &type_info) == 0 &&
      type_info.type &&
      type_info.subtype &&
      type_info.type_len == 9 &&
      strncasecmp(type_info.type, "multipart", 9) == 0 &&
      type_info.boundary != NULL &&
      type_info.boundary_len > 0) {
    parse_multipart_body(msg, pool, body, body_len,
                         type_info.boundary, type_info.boundary_len, depth);
    return;
  }

  parse_singlepart_body(msg, pool, &part_headers, body, body_len);
}

email_message_t *email_message_parse(mem_pool_t *pool,
                                      const char *raw_message,
                                      size_t len) {
  if (!pool || !raw_message) return NULL;

  email_message_t *msg = email_message_create(pool);
  if (!msg) return NULL;

  parse_mime_entity(msg, pool, raw_message, len, 1, 0);

  return msg;
}
