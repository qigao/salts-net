#ifndef SALTSNET_INSTALLED_PROTOCOL_CLIENTS_H
#define SALTSNET_INSTALLED_PROTOCOL_CLIENTS_H

#include <email/email_smtp.h>
#include <email/email_pop3.h>
#include <email/email_imap.h>
#include <ldap/ldap_client.h>
#include <snmp_client.h>

/* Compile and execute the same public lifecycle surface as C11 and C++17.
 * No protocol server is needed: the invalid stream authority fails admission,
 * and UDP creation sends no request. Protocol exchanges have dedicated tests. */
static int installed_protocol_clients(void) {
  char invalid_authority[] = "[localhost]";
  smtp_config_t smtp_config = {0};
  pop3_config_t pop3_config = {0};
  imap_config_t imap_config = {0};
  ldap_client_config_t ldap_config = {0};
  snmp_client_config_t snmp_config = {0};
  smtp_client_t *smtp;
  pop3_client_t *pop3;
  imap_client_t *imap;
  ldap_client_t *ldap;
  snmp_client_t *snmp;
  int failed = 0;

  smtp_config.host = invalid_authority;
  smtp_config.port = 25;
  smtp_config.timeout_ms = 100;
  pop3_config.host = invalid_authority;
  pop3_config.port = 110;
  pop3_config.timeout_ms = 100;
  imap_config.host = invalid_authority;
  imap_config.port = 143;
  imap_config.timeout_ms = 100;
  ldap_config.url = "ldap://127.0.0.1:389";
  ldap_config.timeout_ms = 100;
  snmp_config.host = "127.0.0.1";
  snmp_config.port = 161;
  snmp_config.community = "public";
  snmp_config.version = SNMP_VERSION_2C;
  snmp_config.timeout_ms = 100;
  snmp_config.recv_buffer_size = 1024u;

  smtp = smtp_client_create(&smtp_config);
  pop3 = pop3_client_create(&pop3_config);
  imap = imap_client_create(&imap_config);
  ldap = ldap_client_create(&ldap_config);
  snmp = snmp_client_create(&snmp_config);
  if (!smtp || !pop3 || !imap || !ldap || !snmp) failed = 1;
  if (smtp && smtp_connect(smtp) == 0) failed = 1;
  if (pop3 && pop3_connect(pop3) == 0) failed = 1;
  if (imap && imap_connect(imap) == 0) failed = 1;
  smtp_client_free(smtp);
  pop3_client_free(pop3);
  imap_client_free(imap);
  ldap_client_destroy(ldap);
  snmp_client_destroy(snmp);
  return failed;
}

#endif
