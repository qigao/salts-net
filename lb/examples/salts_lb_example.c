#include "salts_lb.h"

#include <salts/error_codes.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_running = 1;

static void stop_signal(int signal_number) {
  (void)signal_number;
  g_running = 0;
}

static ptrdiff_t tlv_frame(const void *data, size_t size, void *user) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t frame_size;
  (void)user;
  if (size < 3u) return 0;
  frame_size = 3u + ((size_t)bytes[1] << 8u) + bytes[2];
  return size >= frame_size ? (ptrdiff_t)frame_size : 0;
}

static const char *prefix_route(const void *data, size_t size, void *user) {
  (void)user;
  if (size >= 4u && memcmp(data, "API:", 4u) == 0) return "api";
  if (size >= 4u && memcmp(data, "WEB:", 4u) == 0) return "web";
  return NULL;
}

int main(int argc, char **argv) {
  salts_lb_config_t config = salts_lb_config_default();
  salts_lb_t *lb;
  uint16_t frontend_port = (uint16_t)(argc > 1 ? strtoul(argv[1], NULL, 10) : 8080u);
  uint16_t worker_port = (uint16_t)(argc > 2 ? strtoul(argv[2], NULL, 10) : 9090u);

  if (frontend_port == 0u || worker_port == 0u) {
    fprintf(stderr, "usage: salts_lb_example [frontend-port] [worker-port] [route|tlv]\n");
    return 1;
  }
  if (argc > 3 && strcmp(argv[3], "route") == 0) {
    config.route = prefix_route;
  } else if (argc > 3 && strcmp(argv[3], "tlv") == 0) {
    config.mode = SALTS_LB_MODE_REQUEST;
    config.frame = tlv_frame;
  }

  lb = salts_lb_create(&config);
  if (!lb || salts_lb_listen(lb, "0.0.0.0", frontend_port) != SALTS_OK ||
      salts_lb_accept_workers(lb, "0.0.0.0", worker_port) != SALTS_OK) {
    fprintf(stderr, "failed to initialize SaltsNet LB\n");
    if (lb) {
      (void)salts_lb_stop(lb);
      (void)salts_lb_destroy(lb);
    }
    return 1;
  }

  signal(SIGINT, stop_signal);
  signal(SIGTERM, stop_signal);
  printf("SaltsNet LB frontend :%u, workers :%u\n", frontend_port, worker_port);
  while (g_running) {
    size_t events = 0u;
    int status = salts_lb_poll(lb, 250u, &events);
    if (status != SALTS_OK) {
      fprintf(stderr, "LB poll failed: %d\n", status);
      g_running = 0;
    }
  }

  if (salts_lb_stop(lb) != SALTS_OK || salts_lb_destroy(lb) != SALTS_OK) return 1;
  return 0;
}
