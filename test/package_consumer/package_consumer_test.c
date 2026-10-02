#include <email/email_client.h>
#include <ice/salts_ice.h>
#include <ldap/ldap_client.h>
#include <mime_parser.h>
#include <salts_lb.h>
#include <salts_snmp.h>
#include <salts_tcp_proxy.h>
#include <uri_parser.h>


#include <string.h>

int main(void) {
  const salts_lb_config_t lb = salts_lb_config_default();
  const salts_tcp_proxy_config_t proxy = salts_tcp_proxy_config_default();
  const ice_config_t ice = ice_default_config();
  const ldap_client_config_t ldap = {"ldap://127.0.0.1:389", 1000};
  const email_client_t *email = NULL;

  if (lb.connection_capacity == 0u || proxy.session_capacity == 0u ||
      ice.gathering_timeout_ms <= 0 || ldap.timeout_ms != 1000 || email != NULL ||
      strcmp(snmp_version_t_to_string(SNMP_VERSION_3), "v3") != 0 ||
      strcmp(salts_proxy_protocol_to_string(proxy.protocol), "auto") != 0) {
    return 1;
  }


  return 0;
}
