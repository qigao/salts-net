/**
 * @file example_email_imap.c
 * @brief Example: Access email via IMAP
 */

#include "email/email_client.h"
#include "email/email_message.h"
#include "email/email_imap.h"
#include "CoroNet.h"
#include <stdio.h>
#include <stdlib.h>

// IMAP operations must run inside a coroutine
static void imap_test_coro(coro_t *co, void *arg) {
  coro_context_t *ctx = (coro_context_t *)arg;

  // Configure IMAP
  imap_config_t imap_config = {0};

  // Option 1: Gmail (requires app password)
  // imap_config.host = "imap.gmail.com";
  // imap_config.port = 993;
  // imap_config.use_tls = 1;
  // imap_config.username = "your-email@gmail.com";
  // imap_config.password = "your-app-password";

  // Option 2: Outlook/Hotmail
  // imap_config.host = "outlook.office365.com";
  // imap_config.port = 993;
  // imap_config.use_tls = 1;
  // imap_config.username = "your-email@outlook.com";
  // imap_config.password = "your-password";

  // Option 3: Test server (update with valid credentials)
  imap_config.host = "imap.gmail.com";
  imap_config.port = 993;
  imap_config.use_tls = 1;
  imap_config.use_starttls = 0;
  imap_config.username = "your-email@gmail.com";
  imap_config.password = "your-app-password";
  imap_config.timeout_ms = 30000;

  // Create IMAP client
  imap_client_t *imap = imap_client_create(ctx, &imap_config);
  if (!imap) {
    fprintf(stderr, "Failed to create IMAP client\n");
    return;
  }

  // Connect
  printf("Connecting to IMAP server...\n");
  if (imap_connect(imap) != 0) {
    fprintf(stderr, "IMAP connect failed: %s\n", imap_get_error(imap));
    imap_client_free(imap);
    return;
  }

  printf("Connected successfully!\n");

  // List mailboxes
  printf("\nListing mailboxes...\n");
  int mailbox_count = 0;
  char **mailboxes = imap_list_mailboxes(imap, "", "*", &mailbox_count);
  if (mailboxes) {
    printf("Found %d mailboxes:\n", mailbox_count);
    for (int i = 0; i < mailbox_count; i++) {
      printf("  - %s\n", mailboxes[i]);
      free(mailboxes[i]);
    }
    free(mailboxes);
  }

  // Select INBOX
  printf("\nSelecting INBOX...\n");
  imap_mailbox_t *inbox = imap_select_mailbox(imap, "INBOX");
  if (inbox) {
    printf("INBOX selected:\n");
    printf("  Messages: %d\n", inbox->exists);
    printf("  Recent: %d\n", inbox->recent);
    printf("  Unseen: %d\n", inbox->unseen);
    printf("  UIDNEXT: %d\n", inbox->uidnext);
    printf("  UIDVALIDITY: %d\n", inbox->uidvalidity);
    free(inbox->name);
    free(inbox);
  } else {
    fprintf(stderr, "Failed to select INBOX: %s\n", imap_get_error(imap));
  }

  // Search for all messages
  printf("\nSearching for all messages...\n");
  int search_count = 0;
  int *results = imap_search(imap, "ALL", &search_count);
  if (results) {
    printf("Found %d messages\n", search_count);

    // Fetch first message if exists
    if (search_count > 0) {
      printf("\nFetching message %d...\n", results[0]);

      // Get message info first
      imap_message_info_t *info = imap_fetch_info(imap, results[0]);
      if (info) {
        printf("Message info:\n");
        printf("  Seq: %d\n", info->seq_num);
        printf("  UID: %d\n", info->uid);
        printf("  Size: %d bytes\n", info->size);
        printf("  Flags: %s\n", info->flags ? info->flags : "(none)");
        free(info->flags);
        free(info->envelope);
        free(info);
      }

      // Fetch full message
      email_message_t *msg = imap_fetch_message(imap, results[0]);
      if (msg) {
        printf("\nMessage content:\n");
        printf("From: %s\n", msg->from ? msg->from->email : "(unknown)");
        printf("Subject: %s\n", msg->subject ? msg->subject : "(no subject)");
        printf("Body preview: %.200s...\n",
               msg->text_body ? msg->text_body : "(no text body)");

        email_message_free(msg);
      } else {
        fprintf(stderr, "Failed to fetch message: %s\n", imap_get_error(imap));
      }
    }

    free(results);
  } else {
    fprintf(stderr, "Search failed: %s\n", imap_get_error(imap));
  }

  // Cleanup
  imap_disconnect(imap);
  imap_client_free(imap);
}

int main(void) {
  // Create coroutine context
  coro_context_t *ctx = coro_context_create(NULL);
  if (!ctx) {
    fprintf(stderr, "Failed to create coroutine context\n");
    return 1;
  }

  // Spawn coroutine
  if (coro_context_spawn(ctx, imap_test_coro, ctx) != 0) {
    fprintf(stderr, "Failed to spawn coroutine\n");
    coro_context_destroy(ctx);
    return 1;
  }

  // Run event loop
  coro_context_run(ctx, TURBO_RUN_DEFAULT);

  // Cleanup
  coro_context_destroy(ctx);

  return 0;
}
