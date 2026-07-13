/**
 * @file turbo_datagram_multicast_internal.h
 * @brief Shared multicast membership parsing for datagram backends.
 */

#ifndef TURBO_DATAGRAM_MULTICAST_INTERNAL_H
#define TURBO_DATAGRAM_MULTICAST_INTERNAL_H

#include "CoroNet/turbo_datagram.h"
#include "turbo_error.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

static inline int turbo_datagram_parse_interface_index(const char *iface,
                                                        unsigned int *index) {
  unsigned int value;
  const unsigned char *cursor;

  if (!index) {
    return TURBO_EINVAL;
  }
  if (!iface || iface[0] == '\0') {
    *index = 0;
    return 0;
  }

  value = 0;
  cursor = (const unsigned char *)iface;
  while (*cursor) {
    unsigned int digit;

    if (*cursor < '0' || *cursor > '9') {
      return TURBO_EINVAL;
    }
    digit = (unsigned int)(*cursor - '0');
    if (value > (UINT_MAX - digit) / 10U) {
      return TURBO_ERANGE;
    }
    value = value * 10U + digit;
    cursor++;
  }

  *index = value;
  return 0;
}

static inline int turbo_datagram_prepare_ipv4_membership(
    const char *group, const char *iface, struct ip_mreq *request) {
  struct in_addr group_addr;
  struct in_addr iface_addr;
  uint32_t group_host_order;

  if (!group || group[0] == '\0' || !request) {
    return TURBO_EINVAL;
  }
  if (inet_pton(AF_INET, group, &group_addr) != 1) {
    return TURBO_EINVAL;
  }

  group_host_order = ntohl(group_addr.s_addr);
  if ((group_host_order & UINT32_C(0xf0000000)) != UINT32_C(0xe0000000)) {
    return TURBO_EINVAL;
  }

  memset(request, 0, sizeof(*request));
  request->imr_multiaddr = group_addr;
  if (iface && iface[0] != '\0') {
    if (inet_pton(AF_INET, iface, &iface_addr) != 1) {
      return TURBO_EINVAL;
    }
    request->imr_interface = iface_addr;
  } else {
    request->imr_interface.s_addr = htonl(INADDR_ANY);
  }
  return 0;
}

static inline int turbo_datagram_prepare_ipv6_membership(
    const char *group, const char *iface, struct ipv6_mreq *request) {
  unsigned int interface_index;
  int rc;

  if (!group || group[0] == '\0' || !request) {
    return TURBO_EINVAL;
  }

  memset(request, 0, sizeof(*request));
  if (inet_pton(AF_INET6, group, &request->ipv6mr_multiaddr) != 1 ||
      request->ipv6mr_multiaddr.s6_addr[0] != 0xffU) {
    return TURBO_EINVAL;
  }

  rc = turbo_datagram_parse_interface_index(iface, &interface_index);
  if (rc != 0) {
    return rc;
  }
  request->ipv6mr_interface = interface_index;
  return 0;
}

#endif /* TURBO_DATAGRAM_MULTICAST_INTERNAL_H */
