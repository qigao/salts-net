# TurboNet Email Module

Complete email client library with SMTP, POP3, and IMAP support.

## Features

### Protocols
- ✅ **SMTP** - Plain SMTP, SMTPS, and STARTTLS
- ✅ **POP3** - Plain POP3, POP3S, and STLS
- ✅ **IMAP** - Plain IMAP, IMAPS, and STARTTLS

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
- TLS/SSL configuration fields

## Quick Start

### Send Email (SMTP, recommended local path)

For local development, run a plain SMTP sink such as `smtp4dev` and point the example at it.
This matches the current SMTP implementation, which supports plain SMTP today while TLS and
STARTTLS upgrade are available through the same client config.

```c
#include "email/email_smtp.h"
#include "email/email_message.h"
#include "CoroNet.h"

// Create context
coro_context_t *ctx = coro_context_create(NULL);

// Configure SMTP
smtp_config_t config = {0};
config.host = "127.0.0.1";
config.port = 25;
config.use_tls = 0;
config.use_starttls = 0;
config.auth_method = SMTP_AUTH_NONE;
config.username = NULL;
config.password = NULL;

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

Environment overrides used by `examples/example_email_send.c`:

- `SMTP_HOST` default `127.0.0.1`
- `SMTP_PORT` default `25`
- `SMTP_FROM` default `sender@smtp4dev.local`
- `SMTP_TO` default `recipient@smtp4dev.local`
- `SMTP_TLS` default `0`
- `SMTP_STARTTLS` default `0`
- `TURBONET_TLS_CA_FILE` optional CA bundle loaded before the TLS handshake
- `TURBONET_TLS_CA_PATH` optional CA directory loaded before the TLS handshake

If you do want Gmail later, first finish the missing TLS/STARTTLS path in `email_smtp.c`.

### Receive Email (POP3, local smtp4dev path)

The current POP3 code path is plain TCP and expects `USER`/`PASS`.
In this workspace the local `smtp4dev` setup uses the test account `turbo` / `turbo`,
and the example defaults to those values unless you override them with environment variables.

```c
#include "email/email_pop3.h"
#include "CoroNet.h"

// Create context
coro_context_t *ctx = coro_context_create(NULL);

// Configure POP3
pop3_config_t config = {0};
config.host = "127.0.0.1";
config.port = 110;
config.use_tls = 0;
config.use_stls = 0;
config.username = getenv("POP3_USERNAME"); // defaults to "turbo"
config.password = getenv("POP3_PASSWORD"); // defaults to "turbo"

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

Environment overrides used by `examples/example_email_receive.c`:

- `POP3_HOST` default `127.0.0.1`
- `POP3_PORT` default `110`
- `POP3_USERNAME` default `turbo`
- `POP3_PASSWORD` default `turbo`
- `POP3_TLS` default `0`
- `POP3_STLS` default `0`
- `TURBONET_TLS_CA_FILE` optional CA bundle loaded before the TLS handshake
- `TURBONET_TLS_CA_PATH` optional CA directory loaded before the TLS handshake

### Access Email (IMAP, local smtp4dev path)

The IMAP path is now good enough for local `smtp4dev` smoke tests and simple TLS smoke tests.
Like POP3, the bundled local test setup uses `turbo` / `turbo` by default.

```c
imap_config_t config = {0};
config.host = "127.0.0.1";
config.port = 143;
config.use_tls = 0;
config.use_starttls = 0;
config.username = getenv("IMAP_USERNAME"); // defaults to "turbo"
config.password = getenv("IMAP_PASSWORD"); // defaults to "turbo"
```

Environment overrides used by `examples/example_email_imap.c`:

- `IMAP_HOST` default `127.0.0.1`
- `IMAP_PORT` default `143`
- `IMAP_USERNAME` default `turbo`
- `IMAP_PASSWORD` default `turbo`
- `IMAP_TLS` default `0`
- `IMAP_STARTTLS` default `0`
- `TURBONET_TLS_CA_FILE` optional CA bundle loaded before the TLS handshake
- `TURBONET_TLS_CA_PATH` optional CA directory loaded before the TLS handshake

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

- [ ] RFC 2047 encoded-word for non-ASCII subjects
- [ ] IMAP FETCH body parsing
- [ ] Multi-line SMTP/IMAP response handling

## License

Part of TurboNet project.
