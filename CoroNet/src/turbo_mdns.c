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
#include "turbo_datagram_internal.h"
#include "turbo_coro_context.h"
#include "tlog.h"
#include "fmt.h"
#include "turbo_str.h"
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

static int mdns_name_contains_service(const char *name, const char *service_type);
static void mdns_extract_instance_name(char *full_name, const char *service_type,
                                       char *instance, size_t instance_size);

// =============================================================================
// Context definition
// =============================================================================

typedef struct timeout_post_s timeout_post_t;

struct mdns_ctx {
  coro_context_t   *ctx;         /* owning event-loop context */
  turbo_datagram_t *datagram;    /* native UDP socket */
  struct sockaddr_in mcast_addr; /* 224.0.0.251:5353 */
  char hostname[MDNS_MAX_NAME_LEN];
  char local_ip[16];
  char target_services[MDNS_MAX_SERVICES][MDNS_MAX_NAME_LEN];
  size_t target_service_count;

  /* Discovery */
  mdns_discover_cb discover_callback;
  void            *discover_userdata;
  turbo_thread_t   discover_thread;
  int              discover_thread_active;
  volatile int     discover_cancelled;
  volatile int     discover_post_pending;
  timeout_post_t      *discover_timeout_post;
  uint32_t         timer_ms;

  /* Publishing */
  mdns_service_t   published_services[MDNS_MAX_SERVICES];
  size_t           published_count;
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

  fmt(service_name,  sizeof(service_name),  "{}.local",      service->service_type);
  fmt(instance_name, sizeof(instance_name), "{}.{}.local",   service->instance, service->service_type);
  fmt(hostname_fqdn, sizeof(hostname_fqdn), "{}.local",      service->hostname);

  memset(buf, 0, 12);
  buf[2] = 0x84;
  buf[7] = 0x03;
  pos = 12;

  /* PTR */
  pos += encode_name(buf + pos, service_name);
  mdns_write_u16(buf, &pos, DNS_TYPE_PTR);
  mdns_write_u16(buf, &pos, DNS_CLASS_IN | DNS_CLASS_FLUSH);
  mdns_write_u32(buf, &pos, service->ttl);
  rdata_start = pos;  pos += 2;
  pos += encode_name(buf + pos, instance_name);
  { uint16_t rdlen = htons((uint16_t)(pos - rdata_start - 2));
    memcpy(buf + rdata_start, &rdlen, sizeof(rdlen)); }

  /* SRV */
  pos += encode_name(buf + pos, instance_name);
  mdns_write_u16(buf, &pos, DNS_TYPE_SRV);
  mdns_write_u16(buf, &pos, DNS_CLASS_IN | DNS_CLASS_FLUSH);
  mdns_write_u32(buf, &pos, service->ttl);
  rdata_start = pos;  pos += 2;
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

static void mdns_normalize_service(mdns_ctx_t *ctx, mdns_service_t *dst,
                                   const mdns_service_t *src) {
  *dst = *src;
  if (dst->ttl == 0) {
    dst->ttl = 120;
  }
  if (dst->hostname[0] == '\0') {
    strcpy(dst->hostname, ctx->hostname);
  }
  if (dst->ip[0] == '\0') {
    strcpy(dst->ip, ctx->local_ip);
  }
}

static int mdns_find_published_service(const mdns_ctx_t *ctx, const char *instance,
                                       const char *service_type) {
  size_t i;
  if (!ctx || !instance || !service_type) return -1;

  for (i = 0; i < ctx->published_count; i++) {
    const mdns_service_t *service = &ctx->published_services[i];
    if (strcmp(service->instance, instance) == 0 &&
        strcmp(service->service_type, service_type) == 0) {
      return (int)i;
    }
  }

  return -1;
}

static int mdns_find_target_service(const mdns_ctx_t *ctx, const char *name) {
  size_t i;
  if (!ctx || !name) return -1;

  for (i = 0; i < ctx->target_service_count; i++) {
    if (mdns_name_contains_service(name, ctx->target_services[i])) {
      return (int)i;
    }
  }

  return -1;
}

static int mdns_send_service_packet(mdns_ctx_t *ctx, const mdns_service_t *service,
                                    uint32_t ttl_override) {
  uint8_t pkt[1024];
  mdns_service_t outbound;
  size_t len;

  if (!ctx || !ctx->datagram || !service) return TURBO_EINVAL;

  outbound = *service;
  outbound.ttl = ttl_override;
  len = build_ptr_response(pkt, &outbound);
  return turbo_datagram_sendto(ctx->datagram,
                               (const struct sockaddr *)&ctx->mcast_addr,
                               (const char *)pkt, len);
}

static int mdns_service_matches_query(const mdns_service_t *service,
                                      const char *query_name, uint16_t qtype) {
  char our_service[MDNS_MAX_NAME_LEN];

  if (!service || !query_name || qtype != DNS_TYPE_PTR) return 0;

  fmt(our_service, sizeof(our_service), "{}.local", service->service_type);
  return strcmp(query_name, our_service) == 0;
}

static void mdns_clear_discovery_targets(mdns_ctx_t *ctx) {
  size_t i;
  if (!ctx) return;

  for (i = 0; i < MDNS_MAX_SERVICES; i++) {
    ctx->target_services[i][0] = '\0';
  }
  ctx->target_service_count = 0;
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
  fmt(instance, instance_size, "{}", full_name);
}

static void mdns_emit_discovery(mdns_ctx_t *ctx, mdns_service_t *service) {
  if (ctx && ctx->discover_callback && service)
    ctx->discover_callback(service, ctx->discover_userdata);
}

static mdns_service_t *mdns_get_found_service(mdns_service_t *services, size_t *count,
                                              const char *instance,
                                              const char *service_type) {
  size_t i;
  if (!services || !count || !instance || !service_type ||
      instance[0] == '\0' || service_type[0] == '\0') {
    return NULL;
  }

  for (i = 0; i < *count; i++) {
    if (strcmp(services[i].instance, instance) == 0 &&
        strcmp(services[i].service_type, service_type) == 0) {
      return &services[i];
    }
  }

  if (*count >= MDNS_MAX_SERVICES) {
    return NULL;
  }

  memset(&services[*count], 0, sizeof(services[*count]));
  strcpy(services[*count].instance, instance);
  strcpy(services[*count].service_type, service_type);
  strcpy(services[*count].hostname, "unknown");
  strcpy(services[*count].ip, "0.0.0.0");
  services[*count].ttl = 120;
  return &services[(*count)++];
}

static void mdns_handle_ptr_record(mdns_ctx_t *ctx, mdns_service_t *services,
                                   size_t *service_count, const uint8_t *packet,
                                   size_t len, size_t offset, char *name) {
  char ptr_target[MDNS_MAX_NAME_LEN];
  char instance[MDNS_MAX_NAME_LEN];
  mdns_service_t *service;
  int target_index;
  decode_name(packet, len, offset, ptr_target, sizeof(ptr_target));
  TLOG_DEBUG("  PTR points to: {}", ptr_target);

  target_index = mdns_find_target_service(ctx, name);
  if (target_index < 0) return;

  mdns_extract_instance_name(ptr_target, ctx->target_services[target_index],
                             instance, sizeof(instance));
  if (instance[0] == '\0') return;

  service = mdns_get_found_service(services, service_count, instance,
                                   ctx->target_services[target_index]);
  if (!service) return;
  TLOG_DEBUG("  -> Found service instance: {}", service->instance);
}

static void mdns_handle_srv_record(mdns_ctx_t *ctx, mdns_service_t *services,
                                   size_t *service_count, const uint8_t *packet,
                                   size_t len, size_t offset, char *name) {
  char hostname[MDNS_MAX_NAME_LEN];
  char instance[MDNS_MAX_NAME_LEN];
  mdns_service_t *service;
  int target_index;
  if (offset + 6 > len) return;

  decode_name(packet, len, offset + 6, hostname, sizeof(hostname));
  TLOG_DEBUG("  SRV: Port={}, Target={}", mdns_read_u16(packet + offset + 4), hostname);

  target_index = mdns_find_target_service(ctx, name);
  if (target_index < 0) return;

  mdns_extract_instance_name(name, ctx->target_services[target_index],
                             instance, sizeof(instance));
  if (instance[0] == '\0') return;

  service = mdns_get_found_service(services, service_count, instance,
                                   ctx->target_services[target_index]);
  if (!service) return;

  service->port = mdns_read_u16(packet + offset + 4);
  fmt(service->hostname, sizeof(service->hostname), "{}", hostname);
  service->ttl = 120;
  TLOG_DEBUG("  -> Found SRV record for: {}:{}", hostname, service->port);
}

static void mdns_handle_a_record(mdns_ctx_t *ctx, mdns_service_t *services,
                                 size_t service_count, const uint8_t *packet,
                                 size_t offset, char *name) {
  char ip_str[16];
  size_t i;
  (void)ctx;
  fmt(ip_str, sizeof(ip_str), "{}.{}.{}.{}",
                 packet[offset], packet[offset+1],
                 packet[offset+2], packet[offset+3]);
  TLOG_DEBUG("  A record: {} -> {}", name, ip_str);

  for (i = 0; i < service_count; i++) {
    if (services[i].hostname[0] != '\0' &&
        tstr_v_contains(tstr_v_from_cstr(name), tstr_v_from_cstr(services[i].hostname))) {
      fmt(services[i].ip, sizeof(services[i].ip), "{}", ip_str);
      services[i].ttl = 120;
      TLOG_DEBUG("  -> Found A record for {}: {}", services[i].instance, ip_str);
    }
  }
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

  mdns_service_t found_services[MDNS_MAX_SERVICES];
  size_t found_count = 0;
  memset(found_services, 0, sizeof(found_services));

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
      mdns_handle_ptr_record(ctx, found_services, &found_count, packet, len, offset, name);
    else if (type == DNS_TYPE_SRV)
      mdns_handle_srv_record(ctx, found_services, &found_count, packet, len, offset, name);
    else if (type == DNS_TYPE_A && rdlen == 4)
      mdns_handle_a_record(ctx, found_services, found_count, packet, offset, name);

    offset += rdlen;
  }

  for (size_t i = 0; i < found_count; i++) {
    mdns_emit_discovery(ctx, &found_services[i]);
  }
}

// =============================================================================
// turbo_datagram_t receive callback
// =============================================================================

static int on_mdns_recv(void *handle, const mem_slice_t *slice, void *peer) {
  (void)peer;
  turbo_datagram_t *dg = (turbo_datagram_t *)handle;
  mdns_ctx_t *ctx = (mdns_ctx_t *)turbo_datagram_get_user_data(dg);
  if (!ctx) return 0;
  if (!slice) {
    int status = (dg->status != 0) ? dg->status : TURBO_EOF;
    TLOG_ERROR("mdns: recv failed: {:s}", turbo_strerror(status));
    ctx->discover_callback = NULL;
    ctx->discover_userdata = NULL;
    mdns_clear_discovery_targets(ctx);
    ctx->published_count = 0;
    if (ctx->discover_thread_active) {
      ctx->discover_cancelled = 1;
    }
    turbo_datagram_recv_stop(ctx->datagram);
    return 0;
  }
  if (slice->length == 0) return 0;

  TLOG_DEBUG("Received {} bytes from network", slice->length);

  if (ctx->discover_callback && ctx->target_service_count > 0)
    parse_response(ctx, (const uint8_t *)slice->data, slice->length);

  /* Handle queries when publishing */
  if (ctx->published_count > 0 && slice->length >= 12) {
    const uint8_t *pkt = (const uint8_t *)slice->data;
    uint16_t flags_field = mdns_read_u16(pkt + 2);
    if ((flags_field & 0x8000) == 0) {
      uint16_t questions   = mdns_read_u16(pkt + 4);
      size_t   query_offset = 12;
      int should_respond[MDNS_MAX_SERVICES] = {0};

      for (int q = 0; q < (int)questions; q++) {
        char query_name[MDNS_MAX_NAME_LEN];
        size_t i;
        query_offset = decode_name(pkt, slice->length, query_offset,
                                   query_name, sizeof(query_name));
        if (query_offset == 0 || query_offset + 4 > slice->length) break;

        uint16_t qtype = mdns_read_u16(pkt + query_offset);
        TLOG_DEBUG("Query for: {} (type {})", query_name, qtype);
        for (i = 0; i < ctx->published_count; i++) {
          if (mdns_service_matches_query(&ctx->published_services[i], query_name, qtype)) {
            should_respond[i] = 1;
            TLOG_DEBUG("  -> Matches our service type!");
          }
        }
        query_offset += 4;
      }

      for (size_t i = 0; i < ctx->published_count; i++) {
        if (should_respond[i]) {
          mdns_send_service_packet(ctx, &ctx->published_services[i],
                                   ctx->published_services[i].ttl);
        }
      }
    }
  }

  return 0;
}

// =============================================================================
// Discover-timeout thread
// =============================================================================

typedef struct { mdns_ctx_t *ctx; uint32_t ms; } timer_arg_t;
struct timeout_post_s { mdns_ctx_t *ctx; };

static void timer_expire_post(void *arg1, void *arg2) {
  (void)arg2;
  timeout_post_t *post = (timeout_post_t *)arg1;
  mdns_ctx_t *ctx;
  if (!post) return;

  ctx = post->ctx;
  if (!ctx) {
    free(post);
    return;
  }

  ctx->discover_callback = NULL;
  ctx->discover_userdata = NULL;
  ctx->discover_cancelled = 0;
  ctx->discover_post_pending = 0;
  ctx->discover_timeout_post = NULL;
  mdns_clear_discovery_targets(ctx);
  free(post);
}

static void discover_timer_thread(void *arg) {
  timer_arg_t *ta = (timer_arg_t *)arg;
  uint32_t waited = 0;
  uint32_t step = 10;

  while (waited < ta->ms && !ta->ctx->discover_cancelled) {
    uint32_t sleep_ms = step;
    if (sleep_ms > ta->ms - waited) {
      sleep_ms = ta->ms - waited;
    }
    turbo_sleep_ms(sleep_ms);
    waited += sleep_ms;
  }

  if (!ta->ctx->discover_cancelled && ta->ctx->discover_callback && ta->ctx->ctx) {
    timeout_post_t *post = (timeout_post_t *)calloc(1, sizeof(*post));
    if (!post) {
      TLOG_ERROR("mdns: discover timeout alloc failed");
      ta->ctx->discover_callback = NULL;
      ta->ctx->discover_userdata = NULL;
      ta->ctx->discover_cancelled = 0;
      mdns_clear_discovery_targets(ta->ctx);
    } else {
      ta->ctx->discover_post_pending = 1;
      ta->ctx->discover_timeout_post = post;
      post->ctx = ta->ctx;
      int rc = coro_post(ta->ctx->ctx, timer_expire_post, post, NULL);
      if (rc != 0) {
        ta->ctx->discover_post_pending = 0;
        ta->ctx->discover_timeout_post = NULL;
        TLOG_ERROR("mdns: discover timeout post failed");
        timer_expire_post(post, NULL);
      }
    }
  }

  ta->ctx->discover_thread_active = 0;
  ta->ctx->discover_cancelled = 0;
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
    turbo_datagram_set_user_data(ctx->datagram, NULL);
    turbo_datagram_destroy(ctx->datagram);
    ctx->datagram = NULL;
    free(ctx);
    return NULL;
  }

  /* Join multicast group */
  if (turbo_datagram_join_multicast(ctx->datagram, MDNS_MCAST_ADDR, NULL) != 0) {
    TLOG_ERROR("mdns: join_multicast failed");
    turbo_datagram_set_user_data(ctx->datagram, NULL);
    turbo_datagram_destroy(ctx->datagram);
    ctx->datagram = NULL;
    free(ctx);
    return NULL;
  }

  /* Start receiving */
  if (turbo_datagram_recv_start(ctx->datagram, on_mdns_recv) != 0) {
    TLOG_ERROR("mdns: recv_start failed");
    turbo_datagram_leave_multicast(ctx->datagram, MDNS_MCAST_ADDR, NULL);
    turbo_datagram_set_user_data(ctx->datagram, NULL);
    turbo_datagram_destroy(ctx->datagram);
    ctx->datagram = NULL;
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
  size_t i;
  if (!ctx) return;

  ctx->discover_callback = NULL;
  ctx->discover_userdata = NULL;
  mdns_clear_discovery_targets(ctx);

  if (ctx->discover_thread_active) {
    ctx->discover_cancelled = 1;
    turbo_thread_join(&ctx->discover_thread);
    ctx->discover_thread_active = 0;
    ctx->discover_cancelled = 0;
  }

  if (ctx->discover_timeout_post) {
    ctx->discover_timeout_post->ctx = NULL;
    ctx->discover_timeout_post = NULL;
    ctx->discover_post_pending = 0;
  }

  if (!ctx->datagram) {
    free(ctx);
    return;
  }

  /* Send goodbye packet */
  for (i = 0; i < ctx->published_count; i++) {
    (void)mdns_send_service_packet(ctx, &ctx->published_services[i], 0);
  }
  ctx->published_count = 0;

  turbo_datagram_set_user_data(ctx->datagram, NULL);
  turbo_datagram_recv_stop(ctx->datagram);
  turbo_datagram_leave_multicast(ctx->datagram, MDNS_MCAST_ADDR, NULL);
  turbo_datagram_destroy(ctx->datagram);
  ctx->datagram = NULL;
  free(ctx);
}

int mdns_publish(mdns_ctx_t *ctx, const mdns_service_t *service) {
  return mdns_publish_many(ctx, service, service ? 1 : 0);
}

int mdns_publish_many(mdns_ctx_t *ctx, const mdns_service_t *services, size_t count) {
  size_t i;
  int rc;
  if (!ctx) return TURBO_EINVAL;
  if (!services || count == 0) return TURBO_EINVAL;

  for (i = 0; i < count; i++) {
    mdns_service_t normalized;
    int index;

    if (services[i].instance[0] == '\0' || services[i].service_type[0] == '\0') {
      return TURBO_EINVAL;
    }

    index = mdns_find_published_service(ctx, services[i].instance, services[i].service_type);
    if (index < 0) {
      if (ctx->published_count >= MDNS_MAX_SERVICES) {
        return TURBO_ENOBUFS;
      }
      index = (int)ctx->published_count++;
    }

    mdns_normalize_service(ctx, &normalized, &services[i]);
    ctx->published_services[index] = normalized;

    rc = mdns_send_service_packet(ctx, &ctx->published_services[index],
                                  ctx->published_services[index].ttl);
    if (rc != 0) {
      return rc;
    }
  }

  return 0;
}

int mdns_unpublish(mdns_ctx_t *ctx, const char *instance, const char *service_type) {
  size_t index;
  if (!ctx || !instance || !service_type || ctx->published_count == 0) return TURBO_EINVAL;

  index = (size_t)mdns_find_published_service(ctx, instance, service_type);
  if (index >= ctx->published_count) return TURBO_ENOENT;

  (void)mdns_send_service_packet(ctx, &ctx->published_services[index], 0);
  if (index + 1 < ctx->published_count) {
    memmove(&ctx->published_services[index], &ctx->published_services[index + 1],
            (ctx->published_count - index - 1) * sizeof(ctx->published_services[0]));
  }
  ctx->published_count--;
  return 0;
}

int mdns_unpublish_all(mdns_ctx_t *ctx) {
  size_t i;
  if (!ctx) return TURBO_EINVAL;

  for (i = 0; i < ctx->published_count; i++) {
    (void)mdns_send_service_packet(ctx, &ctx->published_services[i], 0);
  }
  ctx->published_count = 0;
  return 0;
}

int mdns_discover(mdns_ctx_t *ctx, const char *service_type,
                  mdns_discover_cb callback, void *userdata,
                  uint32_t timeout_ms) {
  const char *services[1];
  if (!service_type) return TURBO_EINVAL;
  services[0] = service_type;
  return mdns_discover_many(ctx, services, 1, callback, userdata, timeout_ms);
}

int mdns_discover_many(mdns_ctx_t *ctx, const char *const *service_types,
                       size_t count, mdns_discover_cb callback, void *userdata,
                       uint32_t timeout_ms) {
  int rc;
  size_t i;

  if (!ctx || !service_types || count == 0 || !callback) return TURBO_EINVAL;

  if (timeout_ms > 0) {
    if (ctx->discover_thread_active) {
      ctx->discover_cancelled = 1;
      turbo_thread_join(&ctx->discover_thread);
      ctx->discover_thread_active = 0;
    }
    ctx->discover_cancelled = 0;
  } else if (ctx->discover_thread_active) {
    ctx->discover_cancelled = 1;
    turbo_thread_join(&ctx->discover_thread);
    ctx->discover_thread_active = 0;
    ctx->discover_cancelled = 0;
  }

  if (ctx->discover_timeout_post) {
    ctx->discover_timeout_post->ctx = NULL;
    ctx->discover_timeout_post = NULL;
    ctx->discover_post_pending = 0;
  }

  mdns_clear_discovery_targets(ctx);
  if (count > MDNS_MAX_SERVICES) return TURBO_ENOBUFS;
  for (i = 0; i < count; i++) {
    if (!service_types[i] || service_types[i][0] == '\0' ||
        strlen(service_types[i]) >= MDNS_MAX_NAME_LEN) {
      mdns_clear_discovery_targets(ctx);
      return TURBO_EINVAL;
    }
    strcpy(ctx->target_services[i], service_types[i]);
  }
  ctx->target_service_count = count;
  ctx->discover_callback = callback;
  ctx->discover_userdata = userdata;
  ctx->timer_ms = timeout_ms;

  /* Send PTR query */
  for (i = 0; i < count; i++) {
    uint8_t query[512];
    char    query_name[MDNS_MAX_NAME_LEN];
    size_t len;

    fmt(query_name, sizeof(query_name), "{}.local", service_types[i]);
    len = build_query(query, query_name, DNS_TYPE_PTR);
    rc = turbo_datagram_sendto(ctx->datagram,
                               (const struct sockaddr *)&ctx->mcast_addr,
                               (const char *)query, len);
    if (rc != 0) {
      ctx->discover_callback = NULL;
      ctx->discover_userdata = NULL;
      mdns_clear_discovery_targets(ctx);
      return rc;
    }
  }

  if (timeout_ms > 0) {
    timer_arg_t *ta = (timer_arg_t *)malloc(sizeof(*ta));
    if (!ta) {
      ctx->discover_callback = NULL;
      ctx->discover_userdata = NULL;
      return TURBO_ENOMEM;
    }
    ta->ctx = ctx;
    ta->ms = timeout_ms;
    ctx->discover_thread_active = 1;
    rc = turbo_thread_create(&ctx->discover_thread, discover_timer_thread, ta);
    if (rc != 0) {
      ctx->discover_thread_active = 0;
      ctx->discover_callback = NULL;
      ctx->discover_userdata = NULL;
      free(ta);
      return rc;
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
