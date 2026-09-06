#include "salts_tcp_proxy.h"

#include <salts/error_codes.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t salts_proxy_example_stop;

static void salts_proxy_example_signal(int signal_value) {
  (void)signal_value;
  salts_proxy_example_stop = 1;
}

static int salts_proxy_example_port(const char *text, uint16_t *out_port) {
  char *end = NULL;
  unsigned long value;
  if (!text || !out_port) return 0;
  value = strtoul(text, &end, 10);
  if (!end || *end != '\0' || value > UINT16_MAX) return 0;
  *out_port = (uint16_t)value;
  return 1;
}

int main(int argc, char **argv) {
  salts_tcp_proxy_config_t config = salts_tcp_proxy_config_default();
  salts_tcp_proxy_t *proxy = NULL;
  salts_proxy_protocol protocol;
  const char *host;
  uint16_t port;
  uint16_t bound_port;
  int status = SALTS_OK;

  if (argc < 4 || argc > 5 || !salts_proxy_example_port(argv[2], &port) ||
      !salts_proxy_protocol_from_string(argv[3], &protocol) ||
      (protocol == SALTS_PROXY_PROTOCOL_RAW && argc != 5) ||
      (protocol != SALTS_PROXY_PROTOCOL_RAW && argc != 4)) {
    fprintf(stderr, "usage: %s <bind-host> <bind-port> <auto|socks5|http_connect|raw> "
                    "[tcp://backend:port]\n",
            argv[0]);
    return 2;
  }
  host = argv[1];
  config.protocol = protocol;
  if (protocol == SALTS_PROXY_PROTOCOL_RAW) config.raw_backend_uri = argv[4];
  proxy = salts_tcp_proxy_create(&config);
  if (!proxy) {
    fprintf(stderr, "invalid proxy configuration or allocation failure\n");
    return 1;
  }
  status = salts_tcp_proxy_listen(proxy, host, port);
  if (status != SALTS_OK || salts_tcp_proxy_port(proxy, &bound_port) != SALTS_OK) {
    fprintf(stderr, "failed to listen: %d\n", status);
    (void)salts_tcp_proxy_stop(proxy);
    (void)salts_tcp_proxy_destroy(proxy);
    return 1;
  }
  (void)signal(SIGINT, salts_proxy_example_signal);
  (void)signal(SIGTERM, salts_proxy_example_signal);
  printf("SaltsNet TCP proxy listening on %s:%u (%s)\n", host, (unsigned int)bound_port,
         salts_proxy_protocol_to_string(protocol));
  while (!salts_proxy_example_stop) {
    size_t events = 0u;
    status = salts_tcp_proxy_poll(proxy, 100u, &events);
    if (status != SALTS_OK) break;
  }
  if (salts_tcp_proxy_stop(proxy) != SALTS_OK) status = SALTS_EIO;
  if (salts_tcp_proxy_destroy(proxy) != SALTS_OK) status = SALTS_EIO;
  return status == SALTS_OK ? 0 : 1;
}
