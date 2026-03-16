# TurboNet Email Module

Complete email client library with SMTP, POP3, and IMAP support.

## Features

### Protocols
- ✅ **SMTP** - Send emails with authentication (PLAIN/LOGIN)
- ✅ **POP3** - Receive emails with full mailbox management
- ⚠️ **IMAP** - Basic implementation (needs response parsing)

### Message Support
- RFC 2822 compliant message construction
- MIME multipart messages (text/html alternatives)
- File attachments (regular and inline)
- Base64 encoding for attachments
- S/MIME encryption and signing (via OpenSSL)
- Priority headers (high/normal/low)

### Integration
- Async I/O via CoroNet coroutines
- Zero-copy MIME parsing
- Memory pool allocation
- TLS/SSL support (SMTPS, POP3S, IMAPS, STARTTLS)

## Quick Start

### Send Email (SMTP)

```c
#include "email/email_smtp.h"
#include "email/email_message.h"
#include "CoroNet.h"

// Create context
coro_context_t *ctx = coro_context_create(NULL);

// Configure SMTP
smtp_config_t config = {0};
config.host = "smtp.gmail.com";
config.port = 587;
config.use_starttls = 1;
config.auth_method = SMTP_AUTH_PLAIN;
config.username = "your-email@gmail.com";
config.password = "your-app-password";

// Create client and connect
smtp_client_t *smtp = smtp_client_create(ctx, &config);
smtp_connect(smtp);

// Create message
mem_pool_t pool;
mem_init(&pool, 8192);

email_message_t *msg = email_message_create(&pool);
email_message_set_from(msg, "Sender", "sender@example.com");
email_message_add_to(msg, "Recipient", "recipient@example.com");
email_message_set_subject(msg, "Hello from TurboNet");
email_message_set_text_body(msg, "Plain text body");
email_message_set_html_body(msg, "<html><body><h1>HTML body</h1></body></html>");

// Send
smtp_send_message(smtp, msg);

// Cleanup
email_message_free(msg);
mem_destroy(&pool);
smtp_disconnect(smtp);
smtp_client_free(smtp);
coro_context_destroy(ctx);
```

### Receive Email (POP3)

```c
#include "email/email_pop3.h"
#include "CoroNet.h"

// Create context
coro_context_t *ctx = coro_context_create(NULL);

// Configure POP3
pop3_config_t config = {0};
config.host = "pop.gmail.com";
config.port = 995;
config.use_tls = 1;
config.username = "your-email@gmail.com";
config.password = "your-app-password";

// Create client and connect
pop3_client_t *pop3 = pop3_client_create(ctx, &config);
pop3_connect(pop3);

// Get mailbox stats
int total_size;
int msg_count = pop3_stat(pop3, &total_size);
printf("Mailbox: %d messages, %d bytes\n", msg_count, total_size);

// List messages
int count;
pop3_message_info_t *list = pop3_list(pop3, &count);
for (int i = 0; i < count; i++) {
  printf("Message %d: %d bytes\n", list[i].msg_num, list[i].size);
}
free(list);

// Retrieve message
email_message_t *msg = pop3_retrieve_message(pop3, 1);
if (msg) {
  printf("From: %s\n", msg->from->email);
  printf("Subject: %s\n", msg->subject);
  printf("Body: %s\n", msg->text_body);
  email_message_free(msg);
}

// Cleanup
pop3_disconnect(pop3);
pop3_client_free(pop3);
coro_context_destroy(ctx);
```

### Add Attachments

```c
// Regular attachment
const char *file_data = "..."; // File content
size_t file_size = ...;
email_message_add_attachment(msg, "document.pdf", "application/pdf",
                              file_data, file_size);

// Inline attachment (for HTML <img src="cid:logo">)
const char *image_data = "...";
size_t image_size = ...;
email_message_add_inline_attachment(msg, "logo", "logo.png", "image/png",
                                    image_data, image_size);
```

### S/MIME Encryption

```c
// Enable signing
email_message_enable_signing(msg, "cert.pem", "key.pem", NULL);

// Enable encryption
email_message_enable_encryption(msg, "recipient-cert.pem");
```

## API Reference

### SMTP Client

```c
smtp_client_t *smtp_client_create(coro_context_t *ctx, const smtp_config_t *config);
void smtp_client_free(smtp_client_t *client);

int smtp_connect(smtp_client_t *client);
void smtp_disconnect(smtp_client_t *client);

int smtp_send_message(smtp_client_t *client, email_message_t *msg);
int smtp_send_raw(smtp_client_t *client, const char *from_email,
                  const char **to_emails, int to_count,
                  const char *raw_message, size_t message_len);

const char *smtp_get_error(smtp_client_t *client);
int smtp_get_last_code(smtp_client_t *client);
```

### POP3 Client

```c
pop3_client_t *pop3_client_create(coro_context_t *ctx, const pop3_config_t *config);
void pop3_client_free(pop3_client_t *client);

int pop3_connect(pop3_client_t *client);
void pop3_disconnect(pop3_client_t *client);

int pop3_stat(pop3_client_t *client, int *total_size);
pop3_message_info_t *pop3_list(pop3_client_t *client, int *count);
char **pop3_uidl(pop3_client_t *client, int *count);

email_message_t *pop3_retrieve_message(pop3_client_t *client, int msg_num);
email_message_t *pop3_retrieve_headers(pop3_client_t *client, int msg_num);

int pop3_delete_message(pop3_client_t *client, int msg_num);
int pop3_reset(pop3_client_t *client);

const char *pop3_get_error(pop3_client_t *client);
```

### Email Message

```c
email_message_t *email_message_create(mem_pool_t *pool);
void email_message_free(email_message_t *msg);

// Addresses
int email_message_set_from(email_message_t *msg, const char *name, const char *email);
int email_message_add_to(email_message_t *msg, const char *name, const char *email);
int email_message_add_cc(email_message_t *msg, const char *name, const char *email);
int email_message_add_bcc(email_message_t *msg, const char *name, const char *email);
int email_message_set_reply_to(email_message_t *msg, const char *name, const char *email);

// Content
int email_message_set_subject(email_message_t *msg, const char *subject);
int email_message_set_text_body(email_message_t *msg, const char *text);
int email_message_set_html_body(email_message_t *msg, const char *html);
void email_message_set_priority(email_message_t *msg, email_priority_t priority);

// Attachments
int email_message_add_attachment(email_message_t *msg, const char *filename,
                                  const char *content_type, const char *data, size_t data_len);
int email_message_add_inline_attachment(email_message_t *msg, const char *content_id,
                                        const char *filename, const char *content_type,
                                        const char *data, size_t data_len);

// S/MIME
int email_message_enable_signing(email_message_t *msg, const char *cert_path,
                                  const char *key_path, const char *key_password);
int email_message_enable_encryption(email_message_t *msg, const char *recipient_cert_path);

// Serialization
tstr_t email_message_to_string(email_message_t *msg);
email_message_t *email_message_parse(mem_pool_t *pool, const char *raw_message, size_t len);
```

## Configuration

### SMTP Config

```c
typedef struct {
  char *host;               // SMTP server hostname
  int port;                 // Port (25, 587, 465)
  int use_tls;              // 1 for SMTPS (465)
  int use_starttls;         // 1 for STARTTLS (587)
  smtp_auth_method_t auth_method; // SMTP_AUTH_PLAIN, SMTP_AUTH_LOGIN
  char *username;
  char *password;
  int timeout_ms;           // Default 30000
} smtp_config_t;
```

### POP3 Config

```c
typedef struct {
  char *host;               // POP3 server hostname
  int port;                 // Port (110, 995)
  int use_tls;              // 1 for POP3S (995)
  int use_stls;             // 1 for STLS upgrade
  char *username;
  char *password;
  int timeout_ms;           // Default 30000
} pop3_config_t;
```

## Building

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## Dependencies

- **CoroNet** - Async coroutine networking
- **mime_parser** - RFC 2822/MIME parsing
- **OpenSSL** - TLS/SSL and S/MIME
- **turbo_utils** - String utilities and Base64

## Examples

See `examples/` directory:
- `example_email_send.c` - SMTP sending
- `example_email_receive.c` - POP3 receiving

## TODO

- [ ] Complete IMAP response parsing
- [ ] TLS upgrade implementation (`coro_socket_upgrade_tls`)
- [ ] RFC 2047 encoded-word for non-ASCII subjects
- [ ] Message parsing (`email_message_parse`)
- [ ] IMAP FETCH body parsing
- [ ] Multi-line SMTP/IMAP response handling

## License

Part of TurboNet project.
