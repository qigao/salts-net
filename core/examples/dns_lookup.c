/**
   完整的命令行工具，支持:
  - 自定义 DNS 服务器：-s 8.8.8.8
  - IPv4/IPv6 选择：-4 或 -6
  - 多域名查询：google.com github.com
  - 友好的输出格式

  使用示例:
  # 使用默认 DNS
  ./dns_lookup google.com

  # 使用自定义 DNS
  ./dns_lookup -s 8.8.8.8 -s 1.1.1.1 google.com github.com

  # 只查询 IPv4
  ./dns_lookup -4 -s 1.1.1.1 cloudflare.com

  # 只查询 IPv6
  ./dns_lookup -6 google.com

 *
 */
#include <ares.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

#define cwarn(fmt, ...) \
  do { \
    fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__); \
  } while (0)

#define MAX_SOCKETS 64
#define MAX_DNS_SERVERS 8

// Forward declarations
static void timer_cb(uv_timer_t* timer);
static void poll_cb(uv_poll_t* poll, int status, int events);

typedef struct
{
  uv_loop_t* loop;
  ares_channel channel;
  uv_timer_t timer;

  // Socket management - simple array
  struct
  {
    ares_socket_t fd;
    uv_poll_t poll;
    int events;
  } sockets[MAX_SOCKETS];

  int socket_count;
} uv_ares;

// DNS result callback
static void dns_callback(void* arg,
                         int status,
                         int timeouts,
                         struct hostent* host)
{
  const char* query = (const char*)arg;

  if (status != ARES_SUCCESS) {
    printf("❌ %s: %s\n", query ? query : "Unknown", ares_strerror(status));
    return;
  }

  printf("✅ %s -> %s\n", query ? query : "Unknown", host->h_name);
  char ip[INET6_ADDRSTRLEN];
  for (int i = 0; host->h_addr_list[i]; ++i) {
    inet_ntop(host->h_addrtype, host->h_addr_list[i], ip, sizeof(ip));
    printf("   %s\n", ip);
  }
}

// libuv poll callback - handles actual I/O
static void poll_cb(uv_poll_t* poll, int status, int events)
{
  uv_ares* ctx = (uv_ares*)poll->data;
  ares_socket_t rfd = ARES_SOCKET_BAD, wfd = ARES_SOCKET_BAD;

  if (events & UV_READABLE) {
    rfd = poll->socket;
  }
  if (events & UV_WRITABLE) {
    wfd = poll->socket;
  }

  ares_process_fd(ctx->channel, rfd, wfd);

  // Update timer for next timeout
  struct timeval tv;
  struct timeval* tvp = ares_timeout(ctx->channel, NULL, &tv);
  if (tvp && (tvp->tv_sec > 0 || tvp->tv_usec > 0)) {
    uint64_t timeout = tvp->tv_sec * 1000 + tvp->tv_usec / 1000;
    if (timeout > 0) {
      uv_timer_start(&ctx->timer, timer_cb, timeout, 0);
    }
  }
}

// Timer callback for c-ares timeouts
static void timer_cb(uv_timer_t* timer)
{
  uv_ares* ctx = (uv_ares*)timer->data;
  ares_process_fd(ctx->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
}

// Find socket in our array
static int find_socket(uv_ares* ctx, ares_socket_t fd)
{
  for (int i = 0; i < ctx->socket_count; i++) {
    if (ctx->sockets[i].fd == fd) {
      return i;
    }
  }
  return -1;
}

// Socket state callback - the real integration point
static void uv_ares_sock_state_cb(void* data,
                                  ares_socket_t socket_fd,
                                  int readable,
                                  int writable)
{
  uv_ares* ctx = (uv_ares*)data;
  int idx = find_socket(ctx, socket_fd);

  if (!readable && !writable) {
    // Close socket
    if (idx >= 0) {
      uv_poll_stop(&ctx->sockets[idx].poll);
      uv_close((uv_handle_t*)&ctx->sockets[idx].poll, NULL);

      // Remove from array - simple swap with last
      ctx->sockets[idx] = ctx->sockets[--ctx->socket_count];
    }
    return;
  }

  // Add or update socket
  if (idx < 0) {
    if (ctx->socket_count >= MAX_SOCKETS) {
      return;  // Full
    }
    idx = ctx->socket_count++;
    ctx->sockets[idx].fd = socket_fd;

    uv_poll_init_socket(ctx->loop, &ctx->sockets[idx].poll, socket_fd);
    ctx->sockets[idx].poll.data = ctx;
  }

  // Update events
  int events = 0;
  if (readable) {
    events |= UV_READABLE;
  }
  if (writable) {
    events |= UV_WRITABLE;
  }

  if (events != ctx->sockets[idx].events) {
    ctx->sockets[idx].events = events;
    uv_poll_start(&ctx->sockets[idx].poll, events, poll_cb);
  }
}

static void usage(const char* prog)
{
  printf("Usage: %s [options] <domain1> [domain2] ...\n", prog);
  printf("Options:\n");
  printf("  -4           IPv4 only (default: both)\n");
  printf("  -6           IPv6 only\n");
  printf("  -s <server>  DNS server (can be repeated, max %d)\n",
         MAX_DNS_SERVERS);
  printf("  -h           Show this help\n");
  printf("\nExamples:\n");
  printf("  %s google.com\n", prog);
  printf("  %s -s 8.8.8.8 -s 1.1.1.1 google.com github.com\n", prog);
  printf("  %s -6 -s 2001:4860:4860::8888 google.com\n", prog);
}

int main(int argc, char* argv[])
{
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  uv_loop_t* loop = uv_default_loop();
  uv_ares ctx;
  memset(&ctx, 0, sizeof(ctx));

  // Parse command line
  char* dns_servers[MAX_DNS_SERVERS];
  char* domains[32];
  int dns_count = 0, domain_count = 0;
  int ipv4_only = 0, ipv6_only = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "-4") == 0) {
      ipv4_only = 1;
    } else if (strcmp(argv[i], "-6") == 0) {
      ipv6_only = 1;
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      if (dns_count < MAX_DNS_SERVERS) {
        dns_servers[dns_count++] = argv[++i];
      }
    } else if (argv[i][0] != '-') {
      if (domain_count < 32) {
        domains[domain_count++] = argv[i];
      }
    }
  }

  if (domain_count == 0) {
    printf("Error: No domains specified\n");
    usage(argv[0]);
    return 1;
  }

  int status;
  if ((status = ares_library_init(ARES_LIB_INIT_ALL)) != ARES_SUCCESS) {
    printf("Error: ares_library_init: %s\n", ares_strerror(status));
    return 1;
  }

  ctx.loop = loop;

  struct ares_options options = {0};
  options.sock_state_cb_data = &ctx;
  options.sock_state_cb = uv_ares_sock_state_cb;

  // Set up DNS servers if specified
  struct in_addr dns_addrs[MAX_DNS_SERVERS];
  if (dns_count > 0) {
    int valid_dns = 0;
    for (int i = 0; i < dns_count; i++) {
      if (inet_pton(AF_INET, dns_servers[i], &dns_addrs[valid_dns]) == 1) {
        cwarn("Using DNS server: %s", dns_servers[i]);
        valid_dns++;
      } else {
        cwarn("Invalid DNS server: %s", dns_servers[i]);
      }
    }
    if (valid_dns > 0) {
      options.servers = dns_addrs;
      options.nservers = valid_dns;
    }
  }

  uv_timer_init(loop, &ctx.timer);
  ctx.timer.data = &ctx;

  int init_flags = ARES_OPT_SOCK_STATE_CB;
  if (dns_count > 0) {
    init_flags |= ARES_OPT_SERVERS;
  }

  if ((status = ares_init_options(&ctx.channel, &options, init_flags))
      != ARES_SUCCESS)
  {
    printf("Error: ares_init_options: %s\n", ares_strerror(status));
    return 1;
  }

  // Execute queries
  for (int i = 0; i < domain_count; i++) {
    if (!ipv6_only) {
      ares_gethostbyname(
          ctx.channel, domains[i], AF_INET, dns_callback, domains[i]);
    }
    if (!ipv4_only) {
      ares_gethostbyname(
          ctx.channel, domains[i], AF_INET6, dns_callback, domains[i]);
    }
  }

  uv_run(loop, UV_RUN_DEFAULT);

  // Cleanup
  ares_destroy(ctx.channel);
  ares_library_cleanup();

  return 0;
}
