#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET snmp_test_socket_t;
  #define SNMP_TEST_INVALID_SOCKET INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
typedef int snmp_test_socket_t;
  #define SNMP_TEST_INVALID_SOCKET (-1)
#endif

#include "snmp_client.h"
#include "snmp_builder_internal.h"
#include "tinytest.h"

#include <salts/thread.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SNMP_TEST_SOCKET_TIMEOUT_MS = 2000 };

typedef enum snmp_test_agent_behavior_e {
  SNMP_TEST_AGENT_RESPOND = 0,
  SNMP_TEST_AGENT_STALE_THEN_RESPOND,
  SNMP_TEST_AGENT_DROP_RESPONSE,
  SNMP_TEST_AGENT_MALFORMED_RESPONSE,
  SNMP_TEST_AGENT_END_OF_MIB
} snmp_test_agent_behavior_t;

typedef struct snmp_test_agent_s {
  snmp_test_socket_t socket;
  int status;
  snmp_test_agent_behavior_t behavior;
  snmp_pdu_type_t expected_request_type;
} snmp_test_agent_t;

typedef struct snmp_test_exchange_s {
  int status;
  int agent_status;
  int request_id;
  size_t varbind_count;
  char value[3];
} snmp_test_exchange_t;

typedef struct snmp_test_v3_agent_s {
  snmp_test_socket_t socket;
  int status;
  snmp_security_level_t security_level;
  snmp_security_level_t response_security_level;
  const char *security_name;
  uint32_t response_time_offset;
  snmp_pdu_type_t expected_request_type;
} snmp_test_v3_agent_t;

typedef struct snmp_test_v3_exchange_s {
  int status;
  int agent_status;
  snmp_version_t version;
  snmp_pdu_type_t pdu_type;
} snmp_test_v3_exchange_t;

static void snmp_test_close_socket(snmp_test_socket_t socket_value) {
  if (socket_value == SNMP_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int snmp_test_set_receive_timeout(snmp_test_socket_t socket_value) {
#if defined(_WIN32)
  const DWORD timeout_ms = SNMP_TEST_SOCKET_TIMEOUT_MS;
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms,
                    (int)sizeof(timeout_ms));
#else
  const struct timeval timeout = {SNMP_TEST_SOCKET_TIMEOUT_MS / 1000,
                                  (SNMP_TEST_SOCKET_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                    (socklen_t)sizeof(timeout));
#endif
}

static void snmp_test_agent_run(void *user) {
  uint8_t response[] = {
      0x30, 0x28, 0x02, 0x01, 0x01, 0x04, 0x06, 'p',  'u',  'b',  'l',  'i',  'c',
      0xA2, 0x1B, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x30, 0x10,
      0x30, 0x0E, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00, 0x04,
      0x02, 'o',  'k'};
  size_t response_size = sizeof(response);
  snmp_test_agent_t *agent = (snmp_test_agent_t *)user;
  struct sockaddr_storage peer;
#if defined(_WIN32)
  int peer_size = (int)sizeof(peer);
#else
  socklen_t peer_size = (socklen_t)sizeof(peer);
#endif
  uint8_t wire_request[1024];
  const int received = recvfrom(agent->socket, (char *)wire_request,
                                (int)sizeof(wire_request), 0,
                                (struct sockaddr *)&peer, &peer_size);
  if (received <= 0) {
    agent->status = -1;
    return;
  }
  {
    snmp_message_t request = {0};
    if (snmp_parse(wire_request, (size_t)received, &request, NULL) <= 0 ||
        request.pdu.type != agent->expected_request_type) {
      snmp_message_free(&request);
      agent->status = -1;
      return;
    }
    snmp_message_free(&request);
  }
  if (agent->behavior == SNMP_TEST_AGENT_DROP_RESPONSE) {
    agent->status = 0;
    return;
  }
  if (agent->behavior == SNMP_TEST_AGENT_STALE_THEN_RESPOND) {
    response[17] = 2u;
    if (sendto(agent->socket, (const char *)response, (int)sizeof(response), 0,
               (const struct sockaddr *)&peer, peer_size) != (int)sizeof(response)) {
      agent->status = -1;
      return;
    }
    response[17] = 1u;
  } else if (agent->behavior == SNMP_TEST_AGENT_MALFORMED_RESPONSE) {
    response[0] = 0xFFu;
  } else if (agent->behavior == SNMP_TEST_AGENT_END_OF_MIB) {
    response[1] = 0x26u;
    response[14] = 0x19u;
    response[25] = 0x0Eu;
    response[27] = 0x0Cu;
    response[39] = SNMP_TYPE_ENDOFMIBVIEW;
    response[40] = 0u;
    response_size -= 2u;
  }
  agent->status =
      sendto(agent->socket, (const char *)response, (int)response_size, 0,
             (const struct sockaddr *)&peer, peer_size) == (int)response_size
          ? 0
          : -1;
}

static void snmp_test_v3_agent_run(void *user) {
  static const uint8_t engine_id[] = {0x80, 0x00, 0x1f, 0x88, 0x55};
  snmp_test_v3_agent_t *agent = (snmp_test_v3_agent_t *)user;
  struct sockaddr_storage peer;
#if defined(_WIN32)
  int peer_size = (int)sizeof(peer);
#else
  socklen_t peer_size = (socklen_t)sizeof(peer);
#endif
  uint8_t packet[1024];
  snmp_oid_t oid = {0};
  snmp_usm_params_t params = {0};
  snmp_v3_user_t v3_user = {0};
  const snmp_v3_user_t *auth_user = NULL;

  agent->status = -1;
  if (snmp_oid_from_string("1.3.6.1.6.3.15.1.1.4.0", &oid) != 0) return;
  if (agent->security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV) {
    if (usm_create_user(agent->security_name, "authpass", SNMP_AUTH_SHA1,
                        agent->security_level == SNMP_SEC_LEVEL_AUTH_PRIV
                            ? "privpass"
                            : NULL,
                        agent->security_level == SNMP_SEC_LEVEL_AUTH_PRIV
                            ? SNMP_PRIV_AES128
                            : SNMP_PRIV_NONE,
                        engine_id, sizeof(engine_id),
                        &v3_user) != USM_OK) {
      snmp_oid_free(&oid);
      return;
    }
    auth_user = &v3_user;
  }
  params.authoritative_engine_id = (uint8_t *)engine_id;
  params.engine_id_len = sizeof(engine_id);
  params.engine_boots = 7u;
  params.engine_time = 41u;
  params.user_name = "";

  for (int exchange = 0; exchange < 2; ++exchange) {
    const int received = recvfrom(agent->socket, (char *)packet, (int)sizeof(packet), 0,
                                  (struct sockaddr *)&peer, &peer_size);
    snmp_message_t request;
    size_t response_len = sizeof(packet);
    if (received <= 0 ||
        snmp_parse_v3(packet, (size_t)received, &request,
                      exchange == 0 ? NULL : auth_user, NULL) <= 0 ||
        (exchange == 0 && request.usm_params.engine_id_len != 0u) ||
        (exchange == 1 &&
         (request.usm_params.engine_id_len != sizeof(engine_id) ||
          memcmp(request.usm_params.authoritative_engine_id, engine_id,
                 sizeof(engine_id)) != 0 ||
          strcmp(request.usm_params.user_name, agent->security_name) != 0 ||
          request.pdu.type != agent->expected_request_type))) {
      snmp_message_free(&request);
      free(v3_user.user_name);
      snmp_oid_free(&oid);
      return;
    }
    snmp_message_free(&request);

    params.user_name = exchange == 0 ? "" : agent->security_name;
    params.engine_time = 41u + (exchange == 0 ? 0u : agent->response_time_offset);
    if (snmp_build_v3_query(exchange == 0 ? SNMP_PDU_REPORT
                                          : SNMP_PDU_GET_RESPONSE,
                            exchange + 1, &oid, 1u, &params,
                            exchange == 0 ||
                                    agent->response_security_level ==
                                        SNMP_SEC_LEVEL_NOAUTH_NOPRIV
                                ? NULL
                                : auth_user,
                            exchange == 0 ? SNMP_SEC_LEVEL_NOAUTH_NOPRIV
                                          : agent->response_security_level,
                            packet, &response_len) != SNMP_BUILD_OK ||
        sendto(agent->socket, (const char *)packet, (int)response_len, 0,
               (const struct sockaddr *)&peer, peer_size) != (int)response_len) {
      free(v3_user.user_name);
      snmp_oid_free(&oid);
      return;
    }
  }
  free(v3_user.user_name);
  snmp_oid_free(&oid);
  agent->status = 0;
}

static int snmp_test_agent_open(snmp_test_agent_t *agent, uint16_t *port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  WSADATA data;
  int address_size = (int)sizeof(address);
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  memset(agent, 0, sizeof(*agent));
  agent->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (agent->socket == SNMP_TEST_INVALID_SOCKET) return -1;
  if (snmp_test_set_receive_timeout(agent->socket) != 0) return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(agent->socket, (const struct sockaddr *)&address, (int)sizeof(address)) != 0) return -1;
  if (getsockname(agent->socket, (struct sockaddr *)&address, &address_size) != 0) return -1;
  *port = ntohs(address.sin_port);
  return 0;
}

static void snmp_test_agent_close(snmp_test_agent_t *agent) {
  snmp_test_close_socket(agent->socket);
  agent->socket = SNMP_TEST_INVALID_SOCKET;
#if defined(_WIN32)
  (void)WSACleanup();
#endif
}

static snmp_test_exchange_t snmp_test_exchange(snmp_test_agent_behavior_t behavior,
                                               snmp_pdu_type_t request_type) {
  snmp_test_exchange_t exchange = {.status = SNMP_CLIENT_ERROR_NETWORK,
                                   .agent_status = -1};
  snmp_test_agent_t agent;
  salts_thread_t thread = NULL;
  snmp_client_t *client = NULL;
  snmp_message_t response;
  snmp_oid_t oid = {0};
  uint16_t port = 0u;

  if (snmp_test_agent_open(&agent, &port) != 0) return exchange;
  agent.behavior = behavior;
  agent.expected_request_type = request_type;
  if (salts_thread_create(&thread, snmp_test_agent_run, &agent) != 0) {
    snmp_test_agent_close(&agent);
    return exchange;
  }

  const snmp_client_config_t config = {.host = "127.0.0.1",
                                       .port = port,
                                       .community = "public",
                                       .version = SNMP_VERSION_2C,
                                       .timeout_ms =
                                           behavior == SNMP_TEST_AGENT_DROP_RESPONSE ||
                                                   behavior == SNMP_TEST_AGENT_MALFORMED_RESPONSE
                                               ? 25u
                                               : 1000u,
                                       .retries = 0u,
                                       .recv_buffer_size = 1024u};
  client = snmp_client_create(&config);
  if (client != NULL && snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid) == 0) {
    memset(&response, 0, sizeof(response));
    if (request_type == SNMP_PDU_SET_REQUEST) {
      snmp_varbind_t varbind = {.oid = oid, .value_type = SNMP_TYPE_INTEGER};
      varbind.value.i32 = 9;
      exchange.status = snmp_client_set(client, &varbind, 1u, &response);
    } else {
      exchange.status = snmp_client_get(client, &oid, 1u, &response);
    }
    if (exchange.status == SNMP_CLIENT_OK) {
      exchange.request_id = response.pdu.request_id;
      exchange.varbind_count = response.pdu.varbind_count;
      if (response.pdu.varbind_count == 1u && response.pdu.varbinds[0].value.bytes.len == 2u) {
        memcpy(exchange.value, response.pdu.varbinds[0].value.bytes.data, 2u);
        exchange.value[2] = '\0';
      }
    }
  }

  snmp_client_destroy(client);
  snmp_oid_free(&oid);
  if (salts_thread_join(&thread) != 0 || agent.status != 0) {
    exchange.status = SNMP_CLIENT_ERROR_NETWORK;
  }
  exchange.agent_status = agent.status;
  salts_thread_destroy(&thread);
  snmp_test_agent_close(&agent);
  return exchange;
}

static snmp_test_v3_exchange_t snmp_test_v3_exchange(
    snmp_security_level_t security_level,
    snmp_security_level_t response_security_level,
    uint32_t response_time_offset, snmp_pdu_type_t request_type) {
  snmp_test_v3_exchange_t exchange = {.status = SNMP_CLIENT_ERROR_NETWORK,
                                      .agent_status = -1};
  snmp_test_agent_t socket_owner;
  snmp_test_v3_agent_t agent;
  salts_thread_t thread = NULL;
  snmp_client_t *client = NULL;
  snmp_oid_t oid = {0};
  snmp_message_t response;
  uint16_t port = 0u;
  const int authenticated = security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV;

  if (snmp_test_agent_open(&socket_owner, &port) != 0) return exchange;
  agent.socket = socket_owner.socket;
  agent.status = -1;
  agent.security_level = security_level;
  agent.response_security_level = response_security_level;
  agent.security_name = authenticated ? "authuser" : "public";
  agent.response_time_offset = response_time_offset;
  agent.expected_request_type = request_type;
  if (salts_thread_create(&thread, snmp_test_v3_agent_run, &agent) != 0) {
    snmp_test_agent_close(&socket_owner);
    return exchange;
  }
  const snmp_client_config_t config = {
      .host = "127.0.0.1",
      .port = port,
      .community = "public",
      .version = SNMP_VERSION_3,
      .timeout_ms = response_time_offset == 0u ? 1000u : 25u,
      .retries = 0u,
      .recv_buffer_size = 1024u,
      .security_name = agent.security_name,
      .auth_password = authenticated ? "authpass" : NULL,
      .auth_protocol = authenticated ? SNMP_AUTH_SHA1 : SNMP_AUTH_NONE,
      .priv_password = security_level == SNMP_SEC_LEVEL_AUTH_PRIV ? "privpass" : NULL,
      .priv_protocol = security_level == SNMP_SEC_LEVEL_AUTH_PRIV ? SNMP_PRIV_AES128
                                                                  : SNMP_PRIV_NONE,
      .security_level = security_level};
  client = snmp_client_create(&config);
  if (client && snmp_oid_from_string("1.3.6.1.2.1.1.1.0", &oid) == 0) {
    memset(&response, 0, sizeof(response));
    if (request_type == SNMP_PDU_GET_NEXT_REQUEST) {
      exchange.status = snmp_client_get_next(client, &oid, 1u, &response);
    } else if (request_type == SNMP_PDU_SET_REQUEST) {
      snmp_varbind_t varbind = {.oid = oid, .value_type = SNMP_TYPE_INTEGER};
      varbind.value.i32 = 17;
      exchange.status = snmp_client_set(client, &varbind, 1u, &response);
    } else {
      exchange.status = snmp_client_get(client, &oid, 1u, &response);
    }
    if (exchange.status == SNMP_CLIENT_OK) {
      exchange.version = response.version;
      exchange.pdu_type = response.pdu.type;
    }
  }
  snmp_client_destroy(client);
  snmp_oid_free(&oid);
  if (salts_thread_join(&thread) == 0) exchange.agent_status = agent.status;
  salts_thread_destroy(&thread);
  snmp_test_agent_close(&socket_owner);
  return exchange;
}

static void snmp_test_walk_callback(const snmp_oid_t *oid,
                                    const snmp_varbind_t *varbind,
                                    void *user_data) {
  (void)oid;
  (void)varbind;
  (void)user_data;
}

static int snmp_test_walk_to_end_of_mib(void) {
  snmp_test_agent_t agent;
  salts_thread_t thread = NULL;
  snmp_client_t *client = NULL;
  snmp_oid_t root = {0};
  uint16_t port = 0u;
  int walk_count = -1;

  if (snmp_test_agent_open(&agent, &port) != 0) return -1;
  agent.behavior = SNMP_TEST_AGENT_END_OF_MIB;
  agent.expected_request_type = SNMP_PDU_GET_NEXT_REQUEST;
  if (salts_thread_create(&thread, snmp_test_agent_run, &agent) != 0) {
    snmp_test_agent_close(&agent);
    return -1;
  }
  {
    const snmp_client_config_t config = {.host = "127.0.0.1",
                                         .port = port,
                                         .community = "public",
                                         .version = SNMP_VERSION_2C,
                                         .timeout_ms = 1000u,
                                         .retries = 0u,
                                         .recv_buffer_size = 1024u};
    client = snmp_client_create(&config);
  }
  if (client != NULL && snmp_oid_from_string("1.3.6.1.2.1.1", &root) == 0) {
    walk_count = snmp_client_walk(client, &root, snmp_test_walk_callback, NULL);
  }

  snmp_client_destroy(client);
  snmp_oid_free(&root);
  if (salts_thread_join(&thread) != 0 || agent.status != 0) walk_count = -1;
  salts_thread_destroy(&thread);
  snmp_test_agent_close(&agent);
  return walk_count;
}

spec("SNMP CNet transport") {
  it("performs a loopback request without a coroutine context") {
    const snmp_test_exchange_t exchange =
        snmp_test_exchange(SNMP_TEST_AGENT_RESPOND, SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_OK);
    check_equal(exchange.request_id, 1);
    check_equal(exchange.varbind_count, 1u);
    check_equal(exchange.value, "ok");
  }

  it("ignores a stale response before the matching response") {
    const snmp_test_exchange_t exchange =
        snmp_test_exchange(SNMP_TEST_AGENT_STALE_THEN_RESPOND,
                           SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_OK);
    check_equal(exchange.request_id, 1);
    check_equal(exchange.varbind_count, 1u);
    check_equal(exchange.value, "ok");
  }

  it("returns the SNMP timeout domain when no datagram arrives") {
    const snmp_test_exchange_t exchange =
        snmp_test_exchange(SNMP_TEST_AGENT_DROP_RESPONSE, SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_ERROR_TIMEOUT);
  }

  it("returns the SNMP response domain for a malformed datagram") {
    const snmp_test_exchange_t exchange =
        snmp_test_exchange(SNMP_TEST_AGENT_MALFORMED_RESPONSE,
                           SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_ERROR_RESPONSE);
  }

  it("sends a typed SetRequest through CNet") {
    const snmp_test_exchange_t exchange =
        snmp_test_exchange(SNMP_TEST_AGENT_RESPOND, SNMP_PDU_SET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_OK);
    check_equal(exchange.agent_status, 0);
  }

  it("discovers the authoritative engine before a v3 noAuth request") {
    const snmp_test_v3_exchange_t exchange =
        snmp_test_v3_exchange(SNMP_SEC_LEVEL_NOAUTH_NOPRIV,
                              SNMP_SEC_LEVEL_NOAUTH_NOPRIV, 0u,
                              SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_OK);
    check_equal(exchange.version, SNMP_VERSION_3);
    check_equal(exchange.pdu_type, SNMP_PDU_GET_RESPONSE);
    check_equal(exchange.agent_status, 0);
  }

  it("localizes credentials after v3 engine discovery") {
    const snmp_test_v3_exchange_t exchange =
        snmp_test_v3_exchange(SNMP_SEC_LEVEL_AUTH_NOPRIV,
                              SNMP_SEC_LEVEL_AUTH_NOPRIV, 0u,
                              SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_OK);
    check_equal(exchange.pdu_type, SNMP_PDU_GET_RESPONSE);
    check_equal(exchange.agent_status, 0);
  }

  it("rejects an authNoPriv response that omits authentication") {
    const snmp_test_v3_exchange_t exchange = snmp_test_v3_exchange(
        SNMP_SEC_LEVEL_AUTH_NOPRIV, SNMP_SEC_LEVEL_NOAUTH_NOPRIV, 0u,
        SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_ERROR_RESPONSE);
    check_equal(exchange.agent_status, 0);
  }

  it("rejects an authPriv response that omits privacy") {
    const snmp_test_v3_exchange_t exchange = snmp_test_v3_exchange(
        SNMP_SEC_LEVEL_AUTH_PRIV, SNMP_SEC_LEVEL_AUTH_NOPRIV, 0u,
        SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_ERROR_RESPONSE);
    check_equal(exchange.agent_status, 0);
  }

  it("rejects an authenticated v3 response outside the engine time window") {
    const snmp_test_v3_exchange_t exchange =
        snmp_test_v3_exchange(SNMP_SEC_LEVEL_AUTH_NOPRIV,
                              SNMP_SEC_LEVEL_AUTH_NOPRIV, 1000u,
                              SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_ERROR_RESPONSE);
    check_equal(exchange.agent_status, 0);
  }

  it("encrypts and decrypts a v3 authPriv exchange") {
    const snmp_test_v3_exchange_t exchange =
        snmp_test_v3_exchange(SNMP_SEC_LEVEL_AUTH_PRIV,
                              SNMP_SEC_LEVEL_AUTH_PRIV, 0u,
                              SNMP_PDU_GET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_OK);
    check_equal(exchange.pdu_type, SNMP_PDU_GET_RESPONSE);
    check_equal(exchange.agent_status, 0);
  }

  it("sends a v3 GetNextRequest after discovery") {
    const snmp_test_v3_exchange_t exchange =
        snmp_test_v3_exchange(SNMP_SEC_LEVEL_AUTH_NOPRIV,
                              SNMP_SEC_LEVEL_AUTH_NOPRIV, 0u,
                              SNMP_PDU_GET_NEXT_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_OK);
    check_equal(exchange.pdu_type, SNMP_PDU_GET_RESPONSE);
    check_equal(exchange.agent_status, 0);
  }

  it("sends a typed v3 SetRequest after discovery") {
    const snmp_test_v3_exchange_t exchange =
        snmp_test_v3_exchange(SNMP_SEC_LEVEL_AUTH_NOPRIV,
                              SNMP_SEC_LEVEL_AUTH_NOPRIV, 0u,
                              SNMP_PDU_SET_REQUEST);
    check_equal(exchange.status, SNMP_CLIENT_OK);
    check_equal(exchange.pdu_type, SNMP_PDU_GET_RESPONSE);
    check_equal(exchange.agent_status, 0);
  }

  it("rejects AES-256 before opening the transport") {
    const snmp_client_config_t config = {
        .host = "127.0.0.1",
        .port = 161u,
        .community = "public",
        .version = SNMP_VERSION_3,
        .timeout_ms = 1000u,
        .retries = 0u,
        .recv_buffer_size = 1024u,
        .security_name = "authuser",
        .auth_password = "authpass",
        .auth_protocol = SNMP_AUTH_SHA1,
        .priv_password = "privpass",
        .priv_protocol = SNMP_PRIV_AES256,
        .security_level = SNMP_SEC_LEVEL_AUTH_PRIV};
    snmp_client_t *client = snmp_client_create(&config);
    check_null(client);
    snmp_client_destroy(client);
  }

  it("releases an end-of-MIB walk response exactly once") {
    check_equal(snmp_test_walk_to_end_of_mib(), 0);
  }
}
