#include "turbo_mdns.h"
#include "tlog.h"
#include <stb_sprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>


#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#define DNS_TYPE_A 1
#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33

#define DNS_CLASS_IN 1
#define DNS_CLASS_FLUSH 0x8000

struct mdns_ctx {
  uv_loop_t *loop;
  uv_udp_t socket;
  struct sockaddr_in mcast_addr;
  char hostname[MDNS_MAX_NAME_LEN];
  char local_ip[16];

  mdns_discover_cb discover_callback;
  void *discover_userdata;
  uv_timer_t discover_timer;
  char target_service[MDNS_MAX_NAME_LEN];

  mdns_service_t published_service;
  int is_publishing;
};

static void send_cleanup(uv_udp_send_t *req, int status) {
  (void)status;
  free(req);
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)handle;
  buf->base = malloc(suggested_size);
  buf->len = (unsigned long)suggested_size;
}

static void get_local_info(char *hostname, size_t hostname_len, char *ip, size_t ip_len) {
  if (uv_os_gethostname(hostname, &hostname_len) != 0) {
    strcpy(hostname, "localhost");
  }

  uv_interface_address_t *info;
  int count;
  strcpy(ip, "127.0.0.1");

  if (uv_interface_addresses(&info, &count) == 0) {
    for (int i = 0; i < count; i++) {
      if (info[i].address.address4.sin_family == AF_INET && !info[i].is_internal) {
        uv_ip4_name(&info[i].address.address4, ip, (int)ip_len);
        break;
      }
    }
    uv_free_interface_addresses(info, count);
  }
}

static size_t encode_name(uint8_t *buf, const char *name) {
  size_t pos = 0;
  const char *start = name;
  const char *dot;

  while ((dot = strchr(start, '.')) != NULL) {
    size_t len = dot - start;
    if (len > 63)
      return 0;
    buf[pos++] = (uint8_t)len;
    memcpy(buf + pos, start, len);
    pos += len;
    start = dot + 1;
  }

  size_t len = strlen(start);
  if (len > 0 && len <= 63) {
    buf[pos++] = (uint8_t)len;
    memcpy(buf + pos, start, len);
    pos += len;
  }

  buf[pos++] = 0;
  return pos;
}

static size_t decode_name(const uint8_t *packet, size_t packet_len, size_t offset, char *name,
                          size_t name_len) {
  size_t pos = 0;
  size_t jumped = 0;
  int jump_count = 0;

  while (offset < packet_len && packet[offset] != 0) {
    if (jump_count++ > 10)
      return 0;

    uint8_t len = packet[offset];

    if ((len & 0xC0) == 0xC0) {
      if (offset + 1 >= packet_len)
        return 0;
      if (!jumped)
        jumped = offset + 2;
      offset = ((len & 0x3F) << 8) | packet[offset + 1];
      continue;
    }

    if (len > 63 || offset + len + 1 > packet_len)
      return 0;

    offset++;
    if (pos + len + 1 >= name_len)
      return 0;

    if (pos > 0)
      name[pos++] = '.';
    memcpy(name + pos, packet + offset, len);
    pos += len;
    offset += len;
  }

  name[pos] = 0;
  return jumped ? jumped : offset + 1;
}

static size_t build_query(uint8_t *buf, const char *name, uint16_t type) {
  size_t pos = 0;

  memset(buf, 0, 12);
  buf[2] = 0x01;
  buf[5] = 0x01;
  pos = 12;

  pos += encode_name(buf + pos, name);

  *(uint16_t *)(buf + pos) = htons(type);
  pos += 2;
  *(uint16_t *)(buf + pos) = htons(DNS_CLASS_IN);
  pos += 2;

  return pos;
}

static size_t build_ptr_response(uint8_t *buf, const mdns_service_t *service) {
  size_t pos = 0;
  char service_name[MDNS_MAX_NAME_LEN];
  char instance_name[MDNS_MAX_NAME_LEN];
  char hostname_fqdn[MDNS_MAX_NAME_LEN];
  size_t rdata_start;

  stbsp_snprintf(service_name, sizeof(service_name), "%s.local.", service->service_type);
  stbsp_snprintf(instance_name, sizeof(instance_name), "%s.%s.local.", service->instance,
                 service->service_type);
  stbsp_snprintf(hostname_fqdn, sizeof(hostname_fqdn), "%s.local.", service->hostname);

  memset(buf, 0, 12);
  buf[2] = 0x84;
  buf[7] = 0x03;
  pos = 12;

  /* PTR Record */
  pos += encode_name(buf + pos, service_name);
  *(uint16_t *)(buf + pos) = htons(DNS_TYPE_PTR);
  pos += 2;
  *(uint16_t *)(buf + pos) = htons(DNS_CLASS_IN | DNS_CLASS_FLUSH);
  pos += 2;
  *(uint32_t *)(buf + pos) = htonl(service->ttl);
  pos += 4;

  rdata_start = pos + 2;
  pos += 2;
  pos += encode_name(buf + pos, instance_name);
  *(uint16_t *)(buf + rdata_start) = htons((uint16_t)(pos - rdata_start - 2));

  /* SRV Record */
  pos += encode_name(buf + pos, instance_name);
  *(uint16_t *)(buf + pos) = htons(DNS_TYPE_SRV);
  pos += 2;
  *(uint16_t *)(buf + pos) = htons(DNS_CLASS_IN | DNS_CLASS_FLUSH);
  pos += 2;
  *(uint32_t *)(buf + pos) = htonl(service->ttl);
  pos += 4;

  rdata_start = pos + 2;
  pos += 2;
  *(uint16_t *)(buf + pos) = 0; /* priority */
  pos += 2;
  *(uint16_t *)(buf + pos) = 0; /* weight */
  pos += 2;
  *(uint16_t *)(buf + pos) = htons(service->port);
  pos += 2;
  pos += encode_name(buf + pos, hostname_fqdn);
  *(uint16_t *)(buf + rdata_start) = htons((uint16_t)(pos - rdata_start - 2));

  /* A Record */
  pos += encode_name(buf + pos, hostname_fqdn);
  *(uint16_t *)(buf + pos) = htons(DNS_TYPE_A);
  pos += 2;
  *(uint16_t *)(buf + pos) = htons(DNS_CLASS_IN | DNS_CLASS_FLUSH);
  pos += 2;
  *(uint32_t *)(buf + pos) = htonl(service->ttl);
  pos += 4;
  *(uint16_t *)(buf + pos) = htons(4);
  pos += 2;

  struct sockaddr_in addr;
  uv_ip4_addr(service->ip, 0, &addr);
  memcpy(buf + pos, &addr.sin_addr, 4);
  pos += 4;

  return pos;
}

static void parse_response(mdns_ctx_t *ctx, const uint8_t *packet, size_t len) {
  if (len < 12) {
    TLOG_DEBUG("Packet too short: {} bytes", len);
    return;
  }

  uint16_t questions = ntohs(*(uint16_t *)(packet + 4));
  uint16_t answers = ntohs(*(uint16_t *)(packet + 6));
  uint16_t authority = ntohs(*(uint16_t *)(packet + 8));
  uint16_t additional = ntohs(*(uint16_t *)(packet + 10));

  TLOG_DEBUG("DNS Header: Questions={}, Answers={}, Authority={}, Additional={}", questions,
             answers, authority, additional);

  size_t offset = 12;
  int i;

  /* Skip questions */
  for (i = 0; i < questions; i++) {
    char name[MDNS_MAX_NAME_LEN];
    offset = decode_name(packet, len, offset, name, sizeof(name));
    if (offset == 0 || offset + 4 > len) {
      TLOG_DEBUG("Failed to parse question {}", i);
      return;
    }
    TLOG_DEBUG("Question {}: {}", i, name);
    offset += 4;
  }

  /* Parse all records and look for service information */
  mdns_service_t found_service = {0};
  strcpy(found_service.service_type, ctx->target_service);

  for (i = 0; i < answers + authority + additional; i++) {
    if (offset >= len)
      break;

    char name[MDNS_MAX_NAME_LEN];
    size_t name_end = decode_name(packet, len, offset, name, sizeof(name));
    if (name_end == 0 || name_end + 10 > len)
      break;

    uint16_t type = ntohs(*(uint16_t *)(packet + name_end));
    uint16_t rdlen = ntohs(*(uint16_t *)(packet + name_end + 8));

    TLOG_DEBUG("Record {}: Name='{}', Type={}, RDLen={}", i, name, type, rdlen);

    offset = name_end + 10;
    if (offset + rdlen > len)
      break;

    if (type == DNS_TYPE_PTR) {
      char ptr_target[MDNS_MAX_NAME_LEN];
      decode_name(packet, len, offset, ptr_target, sizeof(ptr_target));
      TLOG_DEBUG("  PTR points to: {}", ptr_target);

      if (strstr(name, ctx->target_service)) {
        /* Extract instance name from full service name */
        char *service_start = strstr(ptr_target, ctx->target_service);
        if (service_start && service_start > ptr_target) {
          *(service_start - 1) = 0; /* Remove the dot before service */
          strcpy(found_service.instance, ptr_target);

          /* Found PTR record, trigger callback immediately with what we have */
          if (ctx->discover_callback) {
            strcpy(found_service.hostname, "unknown");
            strcpy(found_service.ip, "0.0.0.0");
            found_service.port = 0;
            found_service.ttl = 120;
            TLOG_DEBUG("  -> Found service instance: {}", found_service.instance);
            ctx->discover_callback(&found_service, ctx->discover_userdata);
          }
        }
      }
    } else if (type == DNS_TYPE_SRV) {
      if (offset + 6 <= len) {
        uint16_t port = ntohs(*(uint16_t *)(packet + offset + 4));
        char hostname[MDNS_MAX_NAME_LEN];
        decode_name(packet, len, offset + 6, hostname, sizeof(hostname));

        TLOG_DEBUG("  SRV: Port={}, Target={}", port, hostname);

        if (strstr(name, ctx->target_service)) {
          /* Extract instance name from SRV record name */
          char *service_start = strstr(name, ctx->target_service);
          if (service_start && service_start > name) {
            *(service_start - 1) = 0;
            strcpy(found_service.instance, name);
          }
          strcpy(found_service.hostname, hostname);
          found_service.port = port;

          if (ctx->discover_callback) {
            strcpy(found_service.ip, "0.0.0.0"); /* Will be filled by A record if available */
            found_service.ttl = 120;
            TLOG_DEBUG("  -> Found SRV record for: {}:{}", hostname, port);
            ctx->discover_callback(&found_service, ctx->discover_userdata);
          }
        }
      }
    } else if (type == DNS_TYPE_A && rdlen == 4) {
      uint8_t *addr_bytes = (uint8_t *)(packet + offset);
      char ip_str[16];
      stbsp_snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", addr_bytes[0], addr_bytes[1],
                     addr_bytes[2], addr_bytes[3]);

      TLOG_DEBUG("  A record: {} -> {}", name, ip_str);

      /* If this A record matches our target hostname, update service info */
      if (strlen(found_service.hostname) > 0 && strstr(name, found_service.hostname)) {
        strcpy(found_service.ip, ip_str);
        if (ctx->discover_callback) {
          found_service.ttl = 120;
          TLOG_DEBUG("  -> Found A record: {}", ip_str);
          ctx->discover_callback(&found_service, ctx->discover_userdata);
        }
      }
    }

    offset += rdlen;
  }
}

static void on_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                    const struct sockaddr *addr, unsigned flags) {
  (void)flags;

  if (nread > 0) {
    mdns_ctx_t *ctx = (mdns_ctx_t *)handle->data;

    /* Debug: Print packet info */
    if (ctx->discover_callback) {
      TLOG_DEBUG("Received {} bytes from network", nread);
      parse_response(ctx, (uint8_t *)buf->base, nread);
    }

    /* Handle queries when publishing */
    if (ctx->is_publishing && nread >= 12) {
      uint16_t flags_field = ntohs(*(uint16_t *)(buf->base + 2));
      if ((flags_field & 0x8000) == 0) { /* This is a query, not a response */

        /* Parse the query to see if it matches our service */
        uint16_t questions = ntohs(*(uint16_t *)(buf->base + 4));
        size_t query_offset = 12;
        int should_respond = 0;

        for (int q = 0; q < questions; q++) {
          char query_name[MDNS_MAX_NAME_LEN];
          query_offset = decode_name((uint8_t *)buf->base, nread, query_offset, query_name,
                                     sizeof(query_name));
          if (query_offset == 0 || (query_offset + 4) > (size_t)nread)
            break;

          uint16_t qtype = ntohs(*(uint16_t *)(buf->base + query_offset));
          TLOG_DEBUG("Query for: {} (type {})", query_name, qtype);

          /* Check if this query is for our service */
          char our_service[MDNS_MAX_NAME_LEN];
          stbsp_snprintf(our_service, sizeof(our_service), "%s.local.",
                         ctx->published_service.service_type);

          if (qtype == DNS_TYPE_PTR && strcmp(query_name, our_service) == 0) {
            should_respond = 1;
            TLOG_DEBUG("  -> This matches our service type!");
          }

          query_offset += 4;
        }

        if (should_respond) {
          uint8_t response[1024];
          size_t response_len = build_ptr_response(response, &ctx->published_service);

          uv_buf_t send_buf = uv_buf_init((char *)response, (unsigned int)response_len);
          uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t));
          TLOG_DEBUG("Sending response to matching query ({} bytes)", response_len);
          uv_udp_send(req, handle, &send_buf, 1, addr, send_cleanup);
        }
      }
    }
  }

  if (buf->base)
    free(buf->base);
}

static void on_discover_timeout(uv_timer_t *timer) {
  mdns_ctx_t *ctx = (mdns_ctx_t *)timer->data;
  ctx->discover_callback = NULL;
  ctx->discover_userdata = NULL;
  uv_timer_stop(timer);
}

mdns_ctx_t *mdns_create(void *loop) {
  mdns_ctx_t *ctx = calloc(1, sizeof(mdns_ctx_t));
  if (!ctx)
    return NULL;

  ctx->loop = (uv_loop_t *)loop;

  size_t hostname_len = sizeof(ctx->hostname);
  size_t ip_len = sizeof(ctx->local_ip);
  get_local_info(ctx->hostname, hostname_len, ctx->local_ip, ip_len);

  if (uv_udp_init(ctx->loop, &ctx->socket) != 0) {
    free(ctx);
    return NULL;
  }

  ctx->socket.data = ctx;

  struct sockaddr_in bind_addr;
  uv_ip4_addr("0.0.0.0", MDNS_PORT, &bind_addr);

  if (uv_udp_bind(&ctx->socket, (const struct sockaddr *)&bind_addr, UV_UDP_REUSEADDR) != 0) {
    uv_close((uv_handle_t *)&ctx->socket, NULL);
    free(ctx);
    return NULL;
  }

  if (uv_udp_set_membership(&ctx->socket, MDNS_MCAST_ADDR, NULL, UV_JOIN_GROUP) != 0) {
    uv_close((uv_handle_t *)&ctx->socket, NULL);
    free(ctx);
    return NULL;
  }

  uv_udp_recv_start(&ctx->socket, alloc_buffer, on_recv);

  uv_ip4_addr(MDNS_MCAST_ADDR, MDNS_PORT, &ctx->mcast_addr);

  return ctx;
}

static void on_mdns_close(uv_handle_t *handle) {
  mdns_ctx_t *ctx = (mdns_ctx_t *)handle->data;
  if (ctx) {
    free(ctx);
  }
}

void mdns_destroy(mdns_ctx_t *ctx) {
  if (!ctx)
    return;

  if (ctx->is_publishing) {
    ctx->published_service.ttl = 0;
    uint8_t goodbye[1024];
    size_t len = build_ptr_response(goodbye, &ctx->published_service);

    uv_buf_t buf = uv_buf_init((char *)goodbye, (unsigned int)len);
    uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t)); /* Malloc instead of stack */
    if (req) {
      uv_udp_send(req, &ctx->socket, &buf, 1, (const struct sockaddr *)&ctx->mcast_addr,
                  send_cleanup);
    }
  }

  /* Make sure handle->data points to ctx so we can free it in callback */
  ctx->socket.data = ctx;
  uv_close((uv_handle_t *)&ctx->socket, on_mdns_close);
  /* Do not free(ctx) here, wait for callback */
}

int mdns_publish(mdns_ctx_t *ctx, const mdns_service_t *service) {
  if (!ctx || !service)
    return -1;

  ctx->published_service = *service;
  if (ctx->published_service.ttl == 0) {
    ctx->published_service.ttl = 120;
  }

  if (strlen(ctx->published_service.hostname) == 0) {
    strcpy(ctx->published_service.hostname, ctx->hostname);
  }

  if (strlen(ctx->published_service.ip) == 0) {
    strcpy(ctx->published_service.ip, ctx->local_ip);
  }

  ctx->is_publishing = 1;

  uint8_t announcement[1024];
  size_t len = build_ptr_response(announcement, &ctx->published_service);

  uv_buf_t buf = uv_buf_init((char *)announcement, (unsigned int)len);
  uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t));
  return uv_udp_send(req, &ctx->socket, &buf, 1, (const struct sockaddr *)&ctx->mcast_addr,
                     send_cleanup);
}

int mdns_unpublish(mdns_ctx_t *ctx, const char *instance, const char *service_type) {
  (void)instance;
  (void)service_type;

  if (!ctx || !ctx->is_publishing)
    return -1;

  ctx->published_service.ttl = 0;
  uint8_t goodbye[1024];
  size_t len = build_ptr_response(goodbye, &ctx->published_service);

  uv_buf_t buf = uv_buf_init((char *)goodbye, (unsigned int)len);
  uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t));
  ctx->is_publishing = 0;

  return uv_udp_send(req, &ctx->socket, &buf, 1, (const struct sockaddr *)&ctx->mcast_addr,
                     send_cleanup);
}

int mdns_discover(mdns_ctx_t *ctx, const char *service_type, mdns_discover_cb callback,
                  void *userdata, uint32_t timeout_ms) {
  if (!ctx || !service_type || !callback)
    return -1;

  strcpy(ctx->target_service, service_type);
  ctx->discover_callback = callback;
  ctx->discover_userdata = userdata;

  uint8_t query[512];
  char query_name[MDNS_MAX_NAME_LEN];
  stbsp_snprintf(query_name, sizeof(query_name), "%s.local.", service_type);

  size_t len = build_query(query, query_name, DNS_TYPE_PTR);

  uv_buf_t buf = uv_buf_init((char *)query, (unsigned int)len);
  uv_udp_send_t *req = malloc(sizeof(uv_udp_send_t));

  if (timeout_ms > 0) {
    uv_timer_init(ctx->loop, &ctx->discover_timer);
    ctx->discover_timer.data = ctx;
    uv_timer_start(&ctx->discover_timer, on_discover_timeout, timeout_ms, 0);
  }

  return uv_udp_send(req, &ctx->socket, &buf, 1, (const struct sockaddr *)&ctx->mcast_addr,
                     send_cleanup);
}

const char *mdns_get_local_hostname(void) {
  static char hostname[MDNS_MAX_NAME_LEN];
  size_t len = sizeof(hostname);

  if (uv_os_gethostname(hostname, &len) != 0) {
    strcpy(hostname, "localhost");
  }
  return hostname;
}

const char *mdns_get_local_ip(void) {
  static char ip[16] = "127.0.0.1";

  uv_interface_address_t *info;
  int count;

  if (uv_interface_addresses(&info, &count) == 0) {
    int i;
    for (i = 0; i < count; i++) {
      if (info[i].address.address4.sin_family == AF_INET && !info[i].is_internal) {
        uv_ip4_name(&info[i].address.address4, ip, sizeof(ip));
        break;
      }
    }
    uv_free_interface_addresses(info, count);
  }

  return ip;
}
