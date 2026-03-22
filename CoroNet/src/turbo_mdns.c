/**
 * @file turbo_mdns.c
 * @brief mDNS service discovery and publishing.
 *
 * Uses turbo_datagram_t (native IOCP/epoll/kqueue) for all I/O
 * — zero libuv dependency.
 *
 * The "event loop" is the coro_context_t that the caller supplies.
 * Timer is implemented via a simple background thread that fires
 * a coro_post() into the context — same pattern as the existing
 * coro_sleep() implementation.
 */
#include "turbo_mdns.h"
#include "turbo_datagram.h"
#include "turbo_coro_context.h"
#include "tlog.h"
#include "turbo_str_view.h"
#include "turbo_thread.h"
#include "turbo_error.h"
#include <stb_sprintf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <ifaddrs.h>
  #include <net/if.h>
#endif

// =============================================================================
// DNS wire-format constants
// =============================================================================

#define DNS_TYPE_A   1
#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33

#define DNS_CLASS_IN    1
#define DNS_CLASS_FLUSH 0x8000

// =============================================================================
// Context definition
// =============================================================================

struct mdns_ctx {
  coro_context_t   *ctx;         /* owning event-loop context */
  turbo_datagram_t *datagram;    /* native UDP socket */
  struct sockaddr_in mcast_addr; /* 224.0.0.251:5353 */
  char hostname[MDNS_MAX_NAME_LEN];
  char local_ip[16];
  char target_service[128];

  /* Discovery */
  mdns_discover_cb discover_callback;
  void            *discover_userdata;
  /* Timer thread for discover timeout */
  turbo_thread_t   timer_thread;
  int              timer_thread_active;
  uint32_t         timer_ms;

  /* Publishing */
  mdns_service_t   published_service;
  int              is_publishing;
};

// =============================================================================
// Wire-format helpers (pure logic, no libuv)
// =============================================================================

static void mdns_write_u16(uint8_t *buf, size_t *pos, uint16_t value) {
  uint16_t net = htons(value);
  memcpy(buf + *pos, &net, sizeof(net));
  *pos += sizeof(net);
}

static void mdns_write_u32(uint8_t *buf, size_t *pos, uint32_t value) {
  uint32_t net = htonl(value);
  memcpy(buf + *pos, &net, sizeof(net));
  *pos += sizeof(net);
}

static uint16_t mdns_read_u16(const uint8_t *buf) {
  uint16_t value;
  memcpy(&value, buf, sizeof(value));
  return ntohs(value);
}

static size_t encode_name(uint8_t *buf, const char *name) {
  size_t pos = 0;
  const char *start = name;
  const char *dot;

  while ((dot = strchr(start, '.')) != NULL) {
    size_t len = (size_t)(dot - start);
    if (len > 63) return 0;
    buf[pos++] = (uint8_t)len;
    memcpy(buf + pos, start, len);
    pos   += len;
    start  = dot + 1;
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

static size_t decode_name(const uint8_t *packet, size_t packet_len,
                           size_t offset, char *name, size_t name_len) {
  size_t pos        = 0;
  size_t jumped     = 0;
  int    jump_count = 0;

  while (offset < packet_len && packet[offset] != 0) {
    if (jump_count++ > 10) return 0;

    uint8_t len = packet[offset];
    if ((len & 0xC0) == 0xC0) {
      if (offset + 1 >= packet_len) return 0;
      if (!jumped) jumped = offset + 2;
      offset = ((len & 0x3F) << 8) | packet[offset + 1];
      continue;
    }

    if (len > 63 || offset + len + 1 > packet_len) return 0;
    offset++;
    if (pos + len + 1 >= name_len) return 0;
    if (pos > 0) name[pos++] = '.';
    memcpy(name + pos, packet + offset, len);
    pos    += len;
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
  mdns_write_u16(buf, &pos, type);
  mdns_write_u16(buf, &pos, DNS_CLASS_IN);
  return pos;
}

static size_t build_ptr_response(uint8_t *buf, const mdns_service_t *service) {
  size_t pos = 0;
  char service_name[MDNS_MAX_NAME_LEN];
  char instance_name[MDNS_MAX_NAME_LEN];
  char hostname_fqdn[MDNS_MAX_NAME_LEN];
  size_t rdata_start;

  stbsp_snprintf(service_name,  sizeof(service_name),  "%s.local.",      service->service_type);
  stbsp_snprintf(instance_name, sizeof(instance_name), "%s.%s.local.",   service->instance, service->service_type);
  stbsp_snprintf(hostname_fqdn, sizeof(hostname_fqdn), "%s.local.",      service->hostname);

  memset(buf, 0, 12);
  buf[2] = 0x84;
  buf[7] = 0x03;
  pos = 12;

  /* PTR */
  pos += encode_name(buf + pos, service_name);
  mdns_write_u16(buf, &pos, DNS_TYPE_PTR);
  mdns_write_u16(buf, &pos, DNS_CLASS_IN | DNS_CLASS_FLUSH);
  mdns_write_u32(buf, &pos, service->ttl);
  rdata_start = pos + 2;  pos += 2;
  pos += encode_name(buf + pos, instance_name);
  { uint16_t rdlen = htons((uint16_t)(pos - rdata_start - 2));
    memcpy(buf + rdata_start, &rdlen, sizeof(rdlen)); }

  /* SRV */
  pos += encode_name(buf + pos, instance_name);
  mdns_write_u16(buf, &pos, DNS_TYPE_SRV);
  mdns_write_u16(buf, &pos, DNS_CLASS_IN | DNS_CLASS_FLUSH);
  mdns_write_u32(buf, &pos, service->ttl);
  rdata_start = pos + 2;  pos += 2;
  mdns_write_u16(buf, &pos, 0);  /* priority */
  mdns_write_u16(buf, &pos, 0);  /* weight */
  mdns_write_u16(buf, &pos, service->port);
  pos += encode_name(buf + pos, hostname_fqdn);
  { uint16_t rdlen = htons((uint16_t)(pos - rdata_start - 2));
    memcpy(buf + rdata_start, &rdlen, sizeof(rdlen)); }

  /* A */
  pos += encode_name(buf + pos, hostname_fqdn);
  mdns_write_u16(buf, &pos, DNS_TYPE_A);
  mdns_write_u16(buf, &pos, DNS_CLASS_IN | DNS_CLASS_FLUSH);
  mdns_write_u32(buf, &pos, service->ttl);
  mdns_write_u16(buf, &pos, 4);
  struct in_addr a4;
  inet_pton(AF_INET, service->ip, &a4);
  memcpy(buf + pos, &a4, 4);
  pos += 4;

  return pos;
}

// =============================================================================
// Platform: primary IPv4 + hostname (no libuv)
// =============================================================================

static void mdns_get_primary_ipv4(char *ip, size_t ip_len) {
  strcpy(ip, "127.0.0.1");

#ifdef _WIN32
  char host[256];
  if (gethostname(host, sizeof(host)) != 0) return;
  struct addrinfo hints = {0}, *res = NULL;
  hints.ai_family = AF_INET;
  if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
    inet_ntop(AF_INET,
              &((struct sockaddr_in *)res->ai_addr)->sin_addr,
              ip, (socklen_t)ip_len);
    freeaddrinfo(res);
  }
#else
  struct ifaddrs *ifa_list = NULL;
  if (getifaddrs(&ifa_list) != 0) return;
  for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (strcmp(ifa->ifa_name, "lo")  == 0) continue;
    if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
    if (!(ifa->ifa_flags & IFF_UP)) continue;
    inet_ntop(AF_INET,
              &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
              ip, (socklen_t)ip_len);
    break;
  }
  freeifaddrs(ifa_list);
#endif
}

static void get_local_info(char *hostname, size_t hostname_len, char *ip, size_t ip_len) {
  if (gethostname(hostname, (int)hostname_len) != 0)
    strcpy(hostname, "localhost");
  mdns_get_primary_ipv4(ip, ip_len);
}

// =============================================================================
// Packet parsing (response → discovery callbacks)
// =============================================================================

static int mdns_name_contains_service(const char *name, const char *service_type) {
  return tstr_v_contains(tstr_v_from_cstr(name), tstr_v_from_cstr(service_type));
}

static void mdns_extract_instance_name(char *full_name, const char *service_type,
                                       char *instance, size_t instance_size) {
  tstr_v full_v    = tstr_v_from_cstr(full_name);
  tstr_v service_v = tstr_v_from_cstr(service_type);
  size_t service_pos = tstr_v_find(full_v, service_v);

  if (service_pos == TSTR_V_NPOS || service_pos == 0) {
    instance[0] = '\0';
    return;
  }
  full_name[service_pos - 1] = '\0';
  stbsp_snprintf(instance, instance_size, "%s", full_name);
}

static void mdns_emit_discovery(mdns_ctx_t *ctx, mdns_service_t *service) {
  if (ctx && ctx->discover_callback && service)
    ctx->discover_callback(service, ctx->discover_userdata);
}

static void mdns_init_found_service(mdns_ctx_t *ctx, mdns_service_t *service) {
  memset(service, 0, sizeof(*service));
  stbsp_snprintf(service->service_type, sizeof(service->service_type),
                 "%s", ctx->target_service);
}

static void mdns_handle_ptr_record(mdns_ctx_t *ctx, mdns_service_t *service,
                                   const uint8_t *packet, size_t len,
                                   size_t offset, char *name) {
  char ptr_target[MDNS_MAX_NAME_LEN];
  decode_name(packet, len, offset, ptr_target, sizeof(ptr_target));
  TLOG_DEBUG("  PTR points to: {}", ptr_target);

  if (!mdns_name_contains_service(name, ctx->target_service)) return;
  mdns_extract_instance_name(ptr_target, ctx->target_service,
                             service->instance, sizeof(service->instance));
  if (service->instance[0] == '\0') return;

  stbsp_snprintf(service->hostname, sizeof(service->hostname), "%s", "unknown");
  stbsp_snprintf(service->ip,       sizeof(service->ip),       "%s", "0.0.0.0");
  service->port = 0;
  service->ttl  = 120;
  TLOG_DEBUG("  -> Found service instance: {}", service->instance);
  mdns_emit_discovery(ctx, service);
}

static void mdns_handle_srv_record(mdns_ctx_t *ctx, mdns_service_t *service,
                                   const uint8_t *packet, size_t len,
                                   size_t offset, char *name) {
  char hostname[MDNS_MAX_NAME_LEN];
  if (offset + 6 > len) return;

  decode_name(packet, len, offset + 6, hostname, sizeof(hostname));
  service->port = mdns_read_u16(packet + offset + 4);
  TLOG_DEBUG("  SRV: Port={}, Target={}", service->port, hostname);

  if (!mdns_name_contains_service(name, ctx->target_service)) return;
  mdns_extract_instance_name(name, ctx->target_service,
                             service->instance, sizeof(service->instance));
  stbsp_snprintf(service->hostname, sizeof(service->hostname), "%s", hostname);
  stbsp_snprintf(service->ip,       sizeof(service->ip),       "%s", "0.0.0.0");
  service->ttl = 120;
  TLOG_DEBUG("  -> Found SRV record for: {}:{}", hostname, service->port);
  mdns_emit_discovery(ctx, service);
}

static void mdns_handle_a_record(mdns_ctx_t *ctx, mdns_service_t *service,
                                  const uint8_t *packet, size_t offset,
                                  char *name) {
  char ip_str[16];
  stbsp_snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
                 packet[offset], packet[offset+1],
                 packet[offset+2], packet[offset+3]);
  TLOG_DEBUG("  A record: {} -> {}", name, ip_str);

  if (service->hostname[0] == '\0' ||
      !tstr_v_contains(tstr_v_from_cstr(name), tstr_v_from_cstr(service->hostname)))
    return;

  stbsp_snprintf(service->ip, sizeof(service->ip), "%s", ip_str);
  service->ttl = 120;
  TLOG_DEBUG("  -> Found A record: {}", ip_str);
  mdns_emit_discovery(ctx, service);
}

static void parse_response(mdns_ctx_t *ctx, const uint8_t *packet, size_t len) {
  if (len < 12) { TLOG_DEBUG("Packet too short: {} bytes", len); return; }

  uint16_t questions  = mdns_read_u16(packet + 4);
  uint16_t answers    = mdns_read_u16(packet + 6);
  uint16_t authority  = mdns_read_u16(packet + 8);
  uint16_t additional = mdns_read_u16(packet + 10);

  TLOG_DEBUG("DNS Header: Questions={}, Answers={}, Authority={}, Additional={}",
             questions, answers, authority, additional);

  size_t offset = 12;

  for (int i = 0; i < (int)questions; i++) {
    char name[MDNS_MAX_NAME_LEN];
    offset = decode_name(packet, len, offset, name, sizeof(name));
    if (offset == 0 || offset + 4 > len) return;
    offset += 4;
  }

  mdns_service_t found_service;
  mdns_init_found_service(ctx, &found_service);

  for (int i = 0; i < (int)(answers + authority + additional); i++) {
    if (offset >= len) break;
    char   name[MDNS_MAX_NAME_LEN];
    size_t name_end = decode_name(packet, len, offset, name, sizeof(name));
    if (name_end == 0 || name_end + 10 > len) break;

    uint16_t type  = mdns_read_u16(packet + name_end);
    uint16_t rdlen = mdns_read_u16(packet + name_end + 8);
    TLOG_DEBUG("Record {}: Name='{}', Type={}, RDLen={}", i, name, type, rdlen);

    offset = name_end + 10;
    if (offset + rdlen > len) break;

    if (type == DNS_TYPE_PTR)
      mdns_handle_ptr_record(ctx, &found_service, packet, len, offset, name);
    else if (type == DNS_TYPE_SRV)
      mdns_handle_srv_record(ctx, &found_service, packet, len, offset, name);
    else if (type == DNS_TYPE_A && rdlen == 4)
      mdns_handle_a_record(ctx, &found_service, packet, offset, name);

    offset += rdlen;
  }
}

// =============================================================================
// turbo_datagram_t receive callback
// =============================================================================

static int on_mdns_recv(void *handle, const mem_slice_t *slice, void *peer) {
  (void)handle;
  (void)peer;
  mdns_ctx_t *ctx = (mdns_ctx_t *)turbo_datagram_get_user_data((turbo_datagram_t *)handle);
  if (!ctx || !slice || slice->length == 0) return 0;

  TLOG_DEBUG("Received {} bytes from network", slice->length);

  if (ctx->discover_callback)
    parse_response(ctx, (const uint8_t *)slice->data, slice->length);

  /* Handle queries when publishing */
  if (ctx->is_publishing && slice->length >= 12) {
    const uint8_t *pkt = (const uint8_t *)slice->data;
    uint16_t flags_field = mdns_read_u16(pkt + 2);
    if ((flags_field & 0x8000) == 0) {
      uint16_t questions   = mdns_read_u16(pkt + 4);
      size_t   query_offset = 12;
      int      should_respond = 0;
      char     our_service[MDNS_MAX_NAME_LEN];

      stbsp_snprintf(our_service, sizeof(our_service), "%s.local.",
                     ctx->published_service.service_type);

      for (int q = 0; q < (int)questions; q++) {
        char query_name[MDNS_MAX_NAME_LEN];
        query_offset = decode_name(pkt, slice->length, query_offset,
                                   query_name, sizeof(query_name));
        if (query_offset == 0 || query_offset + 4 > slice->length) break;

        uint16_t qtype = mdns_read_u16(pkt + query_offset);
        TLOG_DEBUG("Query for: {} (type {})", query_name, qtype);
        if (qtype == DNS_TYPE_PTR && strcmp(query_name, our_service) == 0) {
          should_respond = 1;
          TLOG_DEBUG("  -> Matches our service type!");
        }
        query_offset += 4;
      }

      if (should_respond) {
        uint8_t pkt_buf[1024];
        size_t  pkt_len = build_ptr_response(pkt_buf, &ctx->published_service);
        turbo_datagram_sendto(ctx->datagram,
                              (const struct sockaddr *)&ctx->mcast_addr,
                              (const char *)pkt_buf, pkt_len);
      }
    }
  }

  return 0;
}

// =============================================================================
// Discover-timeout thread
// =============================================================================

typedef struct { mdns_ctx_t *ctx; uint32_t ms; } timer_arg_t;

static void timer_expire_post(void *arg1, void *arg2) {
  (void)arg2;
  mdns_ctx_t *ctx = (mdns_ctx_t *)arg1;
  ctx->discover_callback = NULL;
  ctx->discover_userdata = NULL;
}

static void discover_timer_thread(void *arg) {
  timer_arg_t *ta = (timer_arg_t *)arg;
  turbo_sleep_ms(ta->ms);
  if (ta->ctx->discover_callback && ta->ctx->ctx)
    coro_post(ta->ctx->ctx, timer_expire_post, ta->ctx, NULL);
  ta->ctx->timer_thread_active = 0;
  free(ta);
}

// =============================================================================
// Public API
// =============================================================================

mdns_ctx_t *mdns_create(void *loop) {
  if (!loop) {
    TLOG_ERROR("mdns_create: coro_context_t* required (loop must not be NULL)");
    return NULL;
  }

  mdns_ctx_t *ctx = calloc(1, sizeof(mdns_ctx_t));
  if (!ctx) return NULL;

  ctx->ctx = (coro_context_t *)loop;

  size_t hostname_len = sizeof(ctx->hostname);
  get_local_info(ctx->hostname, hostname_len, ctx->local_ip, sizeof(ctx->local_ip));

  /* Create native datagram */
  ctx->datagram = turbo_datagram_create(ctx->ctx, TURBO_DATAGRAM_UDP4);
  if (!ctx->datagram) { free(ctx); return NULL; }

  turbo_datagram_set_user_data(ctx->datagram, ctx);

  /* Bind to 0.0.0.0:5353 with SO_REUSEADDR */
  if (turbo_datagram_bind(ctx->datagram, "0.0.0.0", MDNS_PORT) != 0) {
    TLOG_ERROR("mdns: bind failed");
    turbo_datagram_destroy(ctx->datagram);
    free(ctx);
    return NULL;
  }

  /* Join multicast group */
  if (turbo_datagram_join_multicast(ctx->datagram, MDNS_MCAST_ADDR, NULL) != 0) {
    TLOG_ERROR("mdns: join_multicast failed");
    turbo_datagram_destroy(ctx->datagram);
    free(ctx);
    return NULL;
  }

  /* Start receiving */
  if (turbo_datagram_recv_start(ctx->datagram, on_mdns_recv) != 0) {
    TLOG_ERROR("mdns: recv_start failed");
    turbo_datagram_leave_multicast(ctx->datagram, MDNS_MCAST_ADDR, NULL);
    turbo_datagram_destroy(ctx->datagram);
    free(ctx);
    return NULL;
  }

  struct sockaddr_in *ma = &ctx->mcast_addr;
  ma->sin_family = AF_INET;
  ma->sin_port   = htons(MDNS_PORT);
  inet_pton(AF_INET, MDNS_MCAST_ADDR, &ma->sin_addr);

  return ctx;
}

void mdns_destroy(mdns_ctx_t *ctx) {
  if (!ctx) return;

  /* Send goodbye packet */
  if (ctx->is_publishing) {
    uint8_t pkt[1024];
    mdns_service_t goodbye = ctx->published_service;
    goodbye.ttl = 0;
    size_t len = build_ptr_response(pkt, &goodbye);
    turbo_datagram_sendto(ctx->datagram,
                          (const struct sockaddr *)&ctx->mcast_addr,
                          (const char *)pkt, len);
  }

  turbo_datagram_recv_stop(ctx->datagram);
  turbo_datagram_leave_multicast(ctx->datagram, MDNS_MCAST_ADDR, NULL);
  turbo_datagram_close(ctx->datagram);
  /* datagram is heap-allocated by turbo_datagram_create; destroy frees it */
  free(ctx);
}

int mdns_publish(mdns_ctx_t *ctx, const mdns_service_t *service) {
  if (!ctx || !service) return TURBO_EINVAL;

  ctx->published_service = *service;
  if (ctx->published_service.ttl == 0)
    ctx->published_service.ttl = 120;
  if (strlen(ctx->published_service.hostname) == 0)
    strcpy(ctx->published_service.hostname, ctx->hostname);
  if (strlen(ctx->published_service.ip) == 0)
    strcpy(ctx->published_service.ip, ctx->local_ip);

  ctx->is_publishing = 1;

  uint8_t pkt[1024];
  size_t  len = build_ptr_response(pkt, &ctx->published_service);
  return turbo_datagram_sendto(ctx->datagram,
                               (const struct sockaddr *)&ctx->mcast_addr,
                               (const char *)pkt, len);
}

int mdns_unpublish(mdns_ctx_t *ctx, const char *instance, const char *service_type) {
  (void)instance;
  (void)service_type;
  if (!ctx || !ctx->is_publishing) return TURBO_EINVAL;

  ctx->is_publishing = 0;
  mdns_service_t goodbye = ctx->published_service;
  goodbye.ttl = 0;
  uint8_t pkt[1024];
  size_t  len = build_ptr_response(pkt, &goodbye);
  return turbo_datagram_sendto(ctx->datagram,
                               (const struct sockaddr *)&ctx->mcast_addr,
                               (const char *)pkt, len);
}

int mdns_discover(mdns_ctx_t *ctx, const char *service_type,
                  mdns_discover_cb callback, void *userdata,
                  uint32_t timeout_ms) {
  if (!ctx || !service_type || !callback) return TURBO_EINVAL;

  strcpy(ctx->target_service, service_type);
  ctx->discover_callback = callback;
  ctx->discover_userdata = userdata;

  /* Send PTR query */
  uint8_t query[512];
  char    query_name[MDNS_MAX_NAME_LEN];
  stbsp_snprintf(query_name, sizeof(query_name), "%s.local.", service_type);
  size_t len = build_query(query, query_name, DNS_TYPE_PTR);
  int rc = turbo_datagram_sendto(ctx->datagram,
                                  (const struct sockaddr *)&ctx->mcast_addr,
                                  (const char *)query, len);
  if (rc != 0) return rc;

  /* Start timeout in a tiny background thread */
  if (timeout_ms > 0 && ctx->ctx) {
    timer_arg_t *ta = malloc(sizeof(*ta));
    if (ta) {
      ta->ctx = ctx;
      ta->ms  = timeout_ms;
      ctx->timer_thread_active = 1;
      if (turbo_thread_create(&ctx->timer_thread, discover_timer_thread, ta) != 0) {
        free(ta);
        ctx->timer_thread_active = 0;
      } else {
        turbo_thread_destroy(&ctx->timer_thread);
      }
    }
  }

  return 0;
}

const char *mdns_get_local_hostname(void) {
  static char hostname[MDNS_MAX_NAME_LEN];
  if (gethostname(hostname, sizeof(hostname)) != 0)
    strcpy(hostname, "localhost");
  return hostname;
}

const char *mdns_get_local_ip(void) {
  static char ip[16] = "127.0.0.1";
  mdns_get_primary_ipv4(ip, sizeof(ip));
  return ip;
}
