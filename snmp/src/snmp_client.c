/**
 * @file snmp_client.c
 * @brief SNMP Client Implementation with UDP Transport
 */

#include "snmp_client.h"
#include "snmp_builder_internal.h"
#include <cnet/cnet.h>
#include "memory_pool.h"
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SNMP_CLIENT_REQUEST_CAPACITY = 1024,
    SNMP_CLIENT_URI_CAPACITY = 320,
    SNMP_CLIENT_QUEUE_CAPACITY = 4,
    SNMP_CLIENT_MAX_WALK_STEPS = 1000,
    SNMP_CLIENT_DEFAULT_STOP_TIMEOUT_MS = 5000
};

/* SNMP Client structure */
struct snmp_client_s {
    /* Configuration */
    char *host;
    uint16_t port;
    char *community;
    snmp_version_t version;
    uint32_t timeout_ms;
    uint32_t retries;
    size_t recv_buffer_size;

    /* SNMPv3 configuration */
    snmp_security_level_t security_level;
    char *security_name;
    char *auth_password;
    char *priv_password;
    snmp_auth_protocol_t auth_protocol;
    snmp_priv_protocol_t priv_protocol;
    snmp_v3_user_t v3_user;              /* User credentials */
    snmp_engine_time_t engine_time;       /* Local/remote engine time */
    uint8_t engine_id[32];                /* Remote engine ID */
    size_t engine_id_len;
    int engine_discovered;                /* Engine discovery complete */

    /* Runtime state */
    int32_t next_request_id;
    char error_msg[256];

    /* CNet has exactly one progress owner: the thread calling this synchronous API. */
    cnet_client net;
    cnet_connection connection;
    int net_initialized;
    int connected;
    int terminal;
    int transport_status;
    int receive_armed;
    int response_ready;
    int send_pending;
    uint8_t *recv_data;
    size_t recv_size;
    MemoryPool *response_pool;
};

static native_io_backend_kind snmp_client_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static char *snmp_client_duplicate(const char *value) {
    const size_t size = strlen(value) + 1u;
    char *copy = (char *)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

static uint32_t snmp_client_effective_timeout(const snmp_client_t *client) {
    return client->timeout_ms != 0u ? client->timeout_ms : 1u;
}

static uint8_t snmp_client_security_flags(snmp_security_level_t security_level) {
    uint8_t flags = 0u;
    if (security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV) flags |= SNMP_MSG_FLAG_AUTH;
    if (security_level == SNMP_SEC_LEVEL_AUTH_PRIV) flags |= SNMP_MSG_FLAG_PRIV;
    return flags;
}

static void snmp_client_set_transport_error(snmp_client_t *client, const char *operation,
                                            int status) {
    client->transport_status = status;
    (void)snprintf(client->error_msg, sizeof(client->error_msg),
                   "%s failed with CNet status %d", operation, status);
}

static void snmp_client_on_state(void *user, cnet_connection connection,
                                 cnet_connection_state state, const cnet_error *error) {
    snmp_client_t *client = (snmp_client_t *)user;
    (void)connection;
    if (state == CNET_CONNECTION_CONNECTED) {
        client->connected = 1;
    } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
        client->terminal = 1;
        client->transport_status = error != NULL ? error->status : SALTS_EIO;
    }
}

static void snmp_client_on_receive(void *user, cnet_connection connection,
                                   const cnet_receive_view *view) {
    snmp_client_t *client = (snmp_client_t *)user;
    (void)connection;
    client->receive_armed = 0;
    if (view == NULL || view->kind != CNET_MESSAGE_DATAGRAM ||
        view->size > client->recv_buffer_size) {
        snmp_client_set_transport_error(client, "receive", SALTS_EMSGSIZE);
        client->response_ready = -1;
        return;
    }
    memcpy(client->recv_data, view->data, view->size);
    client->recv_size = view->size;
    client->response_ready = 1;
}

static void snmp_client_on_send(void *user, cnet_connection connection, size_t size) {
    snmp_client_t *client = (snmp_client_t *)user;
    (void)connection;
    (void)size;
    client->send_pending = 0;
}

static int snmp_client_poll_until_connected(snmp_client_t *client) {
    const uint32_t timeout_ms = snmp_client_effective_timeout(client);
    const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
    while (!client->connected && !client->terminal) {
        const uint64_t now = salts_monotonic_ms();
        size_t events = 0u;
        uint32_t wait_ms;
        int status;
        if (now >= deadline) return SNMP_CLIENT_ERROR_TIMEOUT;
        wait_ms = (uint32_t)(deadline - now);
        status = cnet_client_poll(&client->net, wait_ms, &events);
        if (status != SALTS_OK) {
            snmp_client_set_transport_error(client, "connect poll", status);
            return SNMP_CLIENT_ERROR_NETWORK;
        }
    }
    if (client->terminal) {
        snmp_client_set_transport_error(client, "connect", client->transport_status);
        return SNMP_CLIENT_ERROR_NETWORK;
    }
    return SNMP_CLIENT_OK;
}

static void snmp_client_release_response(snmp_client_t *client) {
    if (client->response_pool) {
        pool_destroy(client->response_pool);
        client->response_pool = NULL;
    }
}

static void snmp_client_release(snmp_client_t *client) {
    if (!client) return;
    snmp_client_release_response(client);
    if (client->net_initialized) {
        (void)cnet_client_stop(&client->net, SNMP_CLIENT_DEFAULT_STOP_TIMEOUT_MS);
        (void)cnet_client_destroy(&client->net);
    }
    free(client->v3_user.user_name);
    free(client->security_name);
    free(client->auth_password);
    free(client->priv_password);
    free(client->recv_data);
    free(client->host);
    free(client->community);
    free(client);
}

/* Default configuration */
static void init_default_config(snmp_client_config_t *config) {
    config->host = "127.0.0.1";
    config->port = 161;
    config->community = "public";
    config->version = SNMP_VERSION_2C;
    config->timeout_ms = 5000;
    config->retries = 3;
    config->recv_buffer_size = 8192;

    /* SNMPv3 defaults */
    config->security_name = NULL;
    config->auth_password = NULL;
    config->auth_protocol = SNMP_AUTH_NONE;
    config->priv_password = NULL;
    config->priv_protocol = SNMP_PRIV_NONE;
    config->security_level = SNMP_SEC_LEVEL_NOAUTH_NOPRIV;
}

static int snmp_client_v3_config_is_valid(const snmp_client_config_t *config) {
    if (!config->security_name || config->security_name[0] == '\0') return 0;
    if (config->security_level == SNMP_SEC_LEVEL_NOAUTH_NOPRIV) {
        return config->auth_protocol == SNMP_AUTH_NONE &&
               config->priv_protocol == SNMP_PRIV_NONE;
    }
    if (config->security_level != SNMP_SEC_LEVEL_AUTH_NOPRIV &&
        config->security_level != SNMP_SEC_LEVEL_AUTH_PRIV) {
        return 0;
    }
    if (!config->auth_password || config->auth_password[0] == '\0' ||
        (config->auth_protocol != SNMP_AUTH_MD5 &&
         config->auth_protocol != SNMP_AUTH_SHA1)) {
        return 0;
    }
    if (config->security_level == SNMP_SEC_LEVEL_AUTH_NOPRIV) {
        return config->priv_protocol == SNMP_PRIV_NONE;
    }
    return config->priv_password && config->priv_password[0] != '\0' &&
           (config->priv_protocol == SNMP_PRIV_DES ||
            config->priv_protocol == SNMP_PRIV_AES128);
}

/* No longer needed: on_udp_recv, on_timeout */

/* Create client */
snmp_client_t *snmp_client_create(const snmp_client_config_t *config) {
    snmp_client_config_t default_config;
    if (!config) {
        init_default_config(&default_config);
        config = &default_config;
    }

    if (!config->host || !config->community || config->recv_buffer_size == 0u ||
        config->recv_buffer_size > CNET_DATAGRAM_MAX_PAYLOAD_BYTES) {
        return NULL;
    }
    if (config->version != SNMP_VERSION_1 && config->version != SNMP_VERSION_2C &&
        config->version != SNMP_VERSION_3) {
        return NULL;
    }
    if (config->version == SNMP_VERSION_3 && !snmp_client_v3_config_is_valid(config)) {
        return NULL;
    }

    snmp_client_t *client = (snmp_client_t *)calloc(1, sizeof(snmp_client_t));
    if (!client) {
        return NULL;
    }

    /* Copy configuration */
    client->host = snmp_client_duplicate(config->host);
    client->community = snmp_client_duplicate(config->community);
    client->port = config->port;
    client->version = config->version;
    client->timeout_ms = config->timeout_ms;
    client->retries = config->retries;
    client->recv_buffer_size = config->recv_buffer_size;
    client->next_request_id = 1;
    client->recv_data = (uint8_t *)malloc(client->recv_buffer_size);
    if (!client->host || !client->community || !client->recv_data) {
        snmp_client_release(client);
        return NULL;
    }

    /* Initialize SNMPv3 if needed */
    if (config->version == SNMP_VERSION_3) {
        client->security_level = config->security_level;
        client->auth_protocol = config->auth_protocol;
        client->priv_protocol = config->priv_protocol;
        client->security_name = snmp_client_duplicate(config->security_name);
        if (config->auth_password) {
            client->auth_password = snmp_client_duplicate(config->auth_password);
        }
        if (config->priv_password) {
            client->priv_password = snmp_client_duplicate(config->priv_password);
        }
        if (!client->security_name ||
            (config->auth_password && !client->auth_password) ||
            (config->priv_password && !client->priv_password)) {
            snmp_client_release(client);
            return NULL;
        }
        client->engine_discovered = 0;
        client->engine_id_len = 0;

        /* Initialize engine time (will be updated during engine discovery) */
        usm_engine_time_init(&client->engine_time, 0);

        if (config->security_level == SNMP_SEC_LEVEL_NOAUTH_NOPRIV) {
            client->v3_user.user_name = snmp_client_duplicate(config->security_name);
            if (!client->v3_user.user_name) {
                snmp_client_release(client);
                return NULL;
            }
            client->v3_user.auth_protocol = SNMP_AUTH_NONE;
            client->v3_user.priv_protocol = SNMP_PRIV_NONE;
        }
    }

    {
        const cnet_client_config net_config = {
            .backend = snmp_client_backend(),
            .connection_capacity = 1u,
            .command_capacity = SNMP_CLIENT_QUEUE_CAPACITY,
            .request_capacity = SNMP_CLIENT_QUEUE_CAPACITY,
            .completion_batch_capacity = SNMP_CLIENT_QUEUE_CAPACITY,
            .event_capacity = SNMP_CLIENT_QUEUE_CAPACITY,
            .max_send_bytes = SNMP_CLIENT_REQUEST_CAPACITY,
            .receive_buffer_bytes = client->recv_buffer_size,
            .connect_timeout_ms = snmp_client_effective_timeout(client)};
        cnet_connect_options options;
        char uri[SNMP_CLIENT_URI_CAPACITY];
        const char *format = strchr(client->host, ':') != NULL ? "udp://[%s]:%u" : "udp://%s:%u";
        const int uri_size = snprintf(uri, sizeof(uri), format, client->host,
                                      (unsigned int)client->port);
        int status;
        if (uri_size < 0 || (size_t)uri_size >= sizeof(uri)) {
            snmp_client_release(client);
            return NULL;
        }
        status = cnet_client_init(&client->net, &net_config);
        if (status != SALTS_OK) {
            snmp_client_release(client);
            return NULL;
        }
        client->net_initialized = 1;
        options = (cnet_connect_options){
            .uri = uri,
            .observer = {.on_state = snmp_client_on_state,
                         .on_receive = snmp_client_on_receive,
                         .user = client,
                         .on_send = snmp_client_on_send}};
        status = cnet_connect(&client->net, &options, &client->connection);
        if (status != SALTS_OK || snmp_client_poll_until_connected(client) != SNMP_CLIENT_OK) {
            snmp_client_release(client);
            return NULL;
        }
    }

    return client;
}

/* Destroy client */
void snmp_client_destroy(snmp_client_t *client) {
    snmp_client_release(client);
}

/* Send request and wait for response (with retry) */
static int send_request_and_wait(
    snmp_client_t *client,
    const uint8_t *request,
    size_t request_len,
    int32_t expected_request_id,
    snmp_message_t *response
) {
    uint32_t attempt = 0u;
    int last_failure = SNMP_CLIENT_ERROR_TIMEOUT;

    snmp_client_release_response(client);

    while (attempt <= client->retries) {
        const uint32_t timeout_ms = snmp_client_effective_timeout(client);
        const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
        int status;

        client->response_ready = 0;
        client->recv_size = 0u;
        if (!client->receive_armed) {
            status = cnet_receive(&client->net, client->connection, 1u);
            if (status != SALTS_OK) {
                snmp_client_set_transport_error(client, "receive admission", status);
                return SNMP_CLIENT_ERROR_NETWORK;
            }
            client->receive_armed = 1;
        }

        client->send_pending = 1;
        status = cnet_send(&client->net, client->connection, request, request_len);
        if (status != SALTS_OK) {
            client->send_pending = 0;
            snmp_client_set_transport_error(client, "send admission", status);
            return SNMP_CLIENT_ERROR_NETWORK;
        }

        for (;;) {
            while (!client->response_ready && !client->terminal) {
                const uint64_t now = salts_monotonic_ms();
                size_t events = 0u;
                uint32_t wait_ms;
                if (now >= deadline) break;
                wait_ms = (uint32_t)(deadline - now);
                status = cnet_client_poll(&client->net, wait_ms, &events);
                if (status != SALTS_OK) {
                    snmp_client_set_transport_error(client, "request poll", status);
                    return SNMP_CLIENT_ERROR_NETWORK;
                }
            }

            if (client->terminal || client->response_ready < 0) {
                snmp_client_set_transport_error(client, "request", client->transport_status);
                return SNMP_CLIENT_ERROR_NETWORK;
            }
            if (client->response_ready == 0) {
                if (last_failure != SNMP_CLIENT_ERROR_RESPONSE) {
                    last_failure = SNMP_CLIENT_ERROR_TIMEOUT;
                }
                break;
            }

            client->response_ready = 0;
            client->response_pool = pool_create(client->recv_buffer_size);
            if (!client->response_pool) return SNMP_CLIENT_ERROR_MEMORY;
            status = client->version == SNMP_VERSION_3
                         ? snmp_parse_v3(client->recv_data, client->recv_size, response,
                                         client->security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV
                                             ? &client->v3_user
                                             : NULL,
                                         client->response_pool)
                         : snmp_parse(client->recv_data, client->recv_size, response,
                                      client->response_pool);
            if (status > 0 && response->pdu.request_id == expected_request_id) {
                int valid_v3_state = 1;
                if (client->version == SNMP_VERSION_3) {
                    const uint8_t security_mask =
                        SNMP_MSG_FLAG_AUTH | SNMP_MSG_FLAG_PRIV;
                    const uint8_t expected_flags = client->engine_discovered
                                                       ? snmp_client_security_flags(
                                                             client->security_level)
                                                       : 0u;
                    valid_v3_state =
                        response->v3_header.msg_security_model == 3u &&
                        (response->v3_header.msg_flags & security_mask) ==
                            expected_flags;
                    if (valid_v3_state && client->engine_discovered) {
                        valid_v3_state =
                            response->usm_params.engine_id_len ==
                                client->engine_id_len &&
                            memcmp(response->usm_params.authoritative_engine_id,
                                   client->engine_id, client->engine_id_len) == 0 &&
                            response->usm_params.user_name != NULL &&
                            strcmp(response->usm_params.user_name,
                                   client->security_name) == 0;
                    }
                    if (valid_v3_state && client->engine_discovered &&
                        (expected_flags & SNMP_MSG_FLAG_AUTH) != 0u) {
                        valid_v3_state =
                            usm_verify_time_window(&client->engine_time,
                                                   response->usm_params.engine_boots,
                                                   response->usm_params.engine_time) == USM_OK;
                    }
                }
                if (valid_v3_state) return SNMP_CLIENT_OK;
            }

            snmp_client_release_response(client);
            last_failure = SNMP_CLIENT_ERROR_RESPONSE;
            if (salts_monotonic_ms() >= deadline) break;
            status = cnet_receive(&client->net, client->connection, 1u);
            if (status != SALTS_OK) {
                snmp_client_set_transport_error(client, "receive admission", status);
                return SNMP_CLIENT_ERROR_NETWORK;
            }
            client->receive_armed = 1;
        }
        attempt++;
    }

    (void)snprintf(client->error_msg, sizeof(client->error_msg),
                   last_failure == SNMP_CLIENT_ERROR_RESPONSE
                       ? "Invalid response after %u retries"
                       : "Request timeout after %u retries",
                   client->retries);
    return last_failure;
}

static int snmp_client_discover_engine(snmp_client_t *client) {
    static const uint32_t discovery_oid_components[] = {1u, 3u, 6u, 1u, 6u, 3u,
                                                         15u, 1u, 1u, 4u, 0u};
    const snmp_oid_t discovery_oid = {
        .components = (uint32_t *)discovery_oid_components,
        .count = sizeof(discovery_oid_components) / sizeof(discovery_oid_components[0])};
    snmp_usm_params_t params = {0};
    snmp_message_t response;
    uint8_t request[SNMP_CLIENT_REQUEST_CAPACITY];
    size_t request_len = sizeof(request);
    const int32_t request_id = client->next_request_id++;
    int status;

    params.user_name = "";
    status = snmp_build_v3_get_request(request_id, &discovery_oid, 1u, &params,
                                       NULL, SNMP_SEC_LEVEL_NOAUTH_NOPRIV,
                                       request, &request_len);
    if (status != SNMP_BUILD_OK) {
        (void)snprintf(client->error_msg, sizeof(client->error_msg),
                       "Failed to build SNMPv3 engine discovery request");
        return SNMP_CLIENT_ERROR_INVALID;
    }
    memset(&response, 0, sizeof(response));
    status = send_request_and_wait(client, request, request_len, request_id,
                                   &response);
    if (status != SNMP_CLIENT_OK) return status;
    if (response.usm_params.engine_id_len == 0u ||
        response.usm_params.engine_id_len > sizeof(client->engine_id)) {
        (void)snprintf(client->error_msg, sizeof(client->error_msg),
                       "SNMPv3 engine discovery returned an invalid engine ID");
        snmp_client_release_response(client);
        return SNMP_CLIENT_ERROR_RESPONSE;
    }

    memcpy(client->engine_id, response.usm_params.authoritative_engine_id,
           response.usm_params.engine_id_len);
    client->engine_id_len = response.usm_params.engine_id_len;
    usm_engine_time_update(&client->engine_time, response.usm_params.engine_boots,
                           response.usm_params.engine_time);
    if (client->security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV &&
        usm_create_user(client->security_name, client->auth_password,
                        client->auth_protocol, client->priv_password,
                        client->priv_protocol, client->engine_id,
                        client->engine_id_len, &client->v3_user) != USM_OK) {
        (void)snprintf(client->error_msg, sizeof(client->error_msg),
                       "Failed to localize SNMPv3 credentials");
        snmp_client_release_response(client);
        return SNMP_CLIENT_ERROR_INVALID;
    }
    client->engine_discovered = 1;
    snmp_client_release_response(client);
    return SNMP_CLIENT_OK;
}

static void snmp_client_get_v3_params(snmp_client_t *client,
                                      snmp_usm_params_t *params) {
    memset(params, 0, sizeof(*params));
    params->authoritative_engine_id = client->engine_id;
    params->engine_id_len = client->engine_id_len;
    params->user_name = client->v3_user.user_name;
    usm_engine_time_get(&client->engine_time, &params->engine_boots,
                        &params->engine_time);
}

static int snmp_client_build_v3_query(snmp_client_t *client,
                                      snmp_pdu_type_t pdu_type,
                                      int32_t request_id,
                                      const snmp_oid_t *oids, size_t oid_count,
                                      uint8_t *request, size_t *request_len) {
    snmp_usm_params_t params = {0};
    snmp_client_get_v3_params(client, &params);
    return snmp_build_v3_query(
        pdu_type, request_id, oids, oid_count, &params,
        client->security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV ? &client->v3_user
                                                             : NULL,
        client->security_level, request, request_len);
}

/* SNMP Get */
int snmp_client_get(
    snmp_client_t *client,
    const snmp_oid_t *oids,
    size_t oid_count,
    snmp_message_t *response
) {
    if (!client || !oids || oid_count == 0 || !response) {
        return SNMP_CLIENT_ERROR_INVALID;
    }

    uint8_t request[SNMP_CLIENT_REQUEST_CAPACITY];
    size_t request_len = sizeof(request);
    int32_t request_id;
    int build_result;

    if (client->version == SNMP_VERSION_3 && !client->engine_discovered) {
        const int discovery_result = snmp_client_discover_engine(client);
        if (discovery_result != SNMP_CLIENT_OK) return discovery_result;
    }
    request_id = client->next_request_id++;
    if (client->version == SNMP_VERSION_3) {
        build_result = snmp_client_build_v3_query(
            client, SNMP_PDU_GET_REQUEST, request_id, oids, oid_count, request,
            &request_len);
    } else {
        build_result = snmp_build_get_request(client->version, client->community,
                                              request_id, oids, oid_count, request,
                                              &request_len);
    }

    if (build_result != SNMP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build GetRequest");
        return SNMP_CLIENT_ERROR_INVALID;
    }

    /* Send and wait */
    int result = send_request_and_wait(client, request, request_len, request_id, response);

    if (result == SNMP_CLIENT_OK) {
        /* Check for SNMP errors */
        if (response->pdu.error_status != SNMP_ERROR_NOERROR) {
            (void)snprintf(client->error_msg, sizeof(client->error_msg),
                           "SNMP error: %d (index: %d)",
                           response->pdu.error_status, response->pdu.error_index);
            return SNMP_CLIENT_ERROR_SNMP;
        }
    }

    return result;
}

/* SNMP GetNext */
int snmp_client_get_next(
    snmp_client_t *client,
    const snmp_oid_t *oids,
    size_t oid_count,
    snmp_message_t *response
) {
    if (!client || !oids || oid_count == 0 || !response) {
        return SNMP_CLIENT_ERROR_INVALID;
    }

    uint8_t request[SNMP_CLIENT_REQUEST_CAPACITY];
    size_t request_len = sizeof(request);
    int32_t request_id;
    int build_result;

    if (client->version == SNMP_VERSION_3 && !client->engine_discovered) {
        const int discovery_result = snmp_client_discover_engine(client);
        if (discovery_result != SNMP_CLIENT_OK) return discovery_result;
    }
    request_id = client->next_request_id++;
    build_result = client->version == SNMP_VERSION_3
                       ? snmp_client_build_v3_query(
                             client, SNMP_PDU_GET_NEXT_REQUEST, request_id, oids,
                             oid_count, request, &request_len)
                       : snmp_build_get_next_request(
                             client->version, client->community, request_id, oids,
                             oid_count, request, &request_len);

    if (build_result != SNMP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build GetNextRequest");
        return SNMP_CLIENT_ERROR_INVALID;
    }

    /* Send and wait */
    int result = send_request_and_wait(client, request, request_len, request_id, response);

    if (result == SNMP_CLIENT_OK) {
        /* Check for SNMP errors */
        if (response->pdu.error_status != SNMP_ERROR_NOERROR) {
            (void)snprintf(client->error_msg, sizeof(client->error_msg),
                           "SNMP error: %d", response->pdu.error_status);
            return SNMP_CLIENT_ERROR_SNMP;
        }
    }

    return result;
}

int snmp_client_set(
    snmp_client_t *client,
    const snmp_varbind_t *varbinds,
    size_t varbind_count,
    snmp_message_t *response
) {
    uint8_t request[SNMP_CLIENT_REQUEST_CAPACITY];
    size_t request_len = sizeof(request);
    int32_t request_id;
    int build_result;
    int result;
    if (!client || !varbinds || varbind_count == 0u || !response) {
        return SNMP_CLIENT_ERROR_INVALID;
    }
    if (client->version == SNMP_VERSION_3 && !client->engine_discovered) {
        const int discovery_result = snmp_client_discover_engine(client);
        if (discovery_result != SNMP_CLIENT_OK) return discovery_result;
    }

    request_id = client->next_request_id++;
    if (client->version == SNMP_VERSION_3) {
        snmp_usm_params_t params;
        snmp_client_get_v3_params(client, &params);
        build_result = snmp_build_v3_set_request(
            request_id, varbinds, varbind_count, &params,
            client->security_level >= SNMP_SEC_LEVEL_AUTH_NOPRIV
                ? &client->v3_user
                : NULL,
            client->security_level, request, &request_len);
    } else {
        build_result = snmp_build_set_request(client->version, client->community,
                                              request_id, varbinds, varbind_count,
                                              request, &request_len);
    }
    if (build_result != SNMP_BUILD_OK) {
        (void)snprintf(client->error_msg, sizeof(client->error_msg),
                       "Failed to build SetRequest");
        return SNMP_CLIENT_ERROR_INVALID;
    }
    result = send_request_and_wait(client, request, request_len, request_id,
                                   response);
    if (result == SNMP_CLIENT_OK &&
        response->pdu.error_status != SNMP_ERROR_NOERROR) {
        (void)snprintf(client->error_msg, sizeof(client->error_msg),
                       "SNMP error: %d (index: %d)",
                       response->pdu.error_status, response->pdu.error_index);
        return SNMP_CLIENT_ERROR_SNMP;
    }
    return result;
}

/* SNMP Walk */
int snmp_client_walk(
    snmp_client_t *client,
    const snmp_oid_t *root_oid,
    snmp_walk_cb callback,
    void *user_data
) {
    snmp_oid_t current_oid = {0};
    int count = 0;

    if (!client || !root_oid || !root_oid->components || root_oid->count == 0u ||
        !callback) {
        return SNMP_CLIENT_ERROR_INVALID;
    }
    if (root_oid->count > SIZE_MAX / sizeof(*current_oid.components)) {
        return SNMP_CLIENT_ERROR_MEMORY;
    }
    current_oid.components =
        (uint32_t *)malloc(root_oid->count * sizeof(*current_oid.components));
    if (!current_oid.components) {
        return SNMP_CLIENT_ERROR_MEMORY;
    }
    memcpy(current_oid.components, root_oid->components,
           root_oid->count * sizeof(*current_oid.components));
    current_oid.count = root_oid->count;

    while (count < SNMP_CLIENT_MAX_WALK_STEPS) {
        snmp_message_t response;
        int result = snmp_client_get_next(client, &current_oid, 1, &response);

        if (result != SNMP_CLIENT_OK) {
            snmp_client_release_response(client);
            break;  /* Error or timeout */
        }

        /* Check if we're still under the root OID */
        if (response.pdu.varbind_count == 0) {
            snmp_client_release_response(client);
            break;  /* No more OIDs */
        }

        snmp_varbind_t *varbind = &response.pdu.varbinds[0];

        /* Check for end-of-MIB */
        if (varbind->value_type == SNMP_TYPE_ENDOFMIBVIEW ||
            varbind->value_type == SNMP_TYPE_NOSUCHOBJECT ||
            varbind->value_type == SNMP_TYPE_NOSUCHINSTANCE) {
            snmp_client_release_response(client);
            break;
        }

        /* Check if still under root */
        size_t cmp_len = root_oid->count;
        int still_under_root = varbind->oid.count >= root_oid->count;

        for (size_t i = 0; i < cmp_len; i++) {
            if (varbind->oid.components[i] != root_oid->components[i]) {
                still_under_root = 0;
                break;
            }
        }

        if (!still_under_root) {
            snmp_client_release_response(client);
            break;
        }

        /* Preserve the next cursor before releasing the response-owned pool. */
        if (varbind->oid.count > SIZE_MAX / sizeof(*current_oid.components)) {
            snmp_client_release_response(client);
            free(current_oid.components);
            return SNMP_CLIENT_ERROR_MEMORY;
        }
        {
            uint32_t *next_components =
                (uint32_t *)realloc(current_oid.components,
                                    varbind->oid.count *
                                        sizeof(*current_oid.components));
            if (!next_components) {
                snmp_client_release_response(client);
                free(current_oid.components);
                return SNMP_CLIENT_ERROR_MEMORY;
            }
            current_oid.components = next_components;
            memcpy(current_oid.components, varbind->oid.components,
                   varbind->oid.count * sizeof(*current_oid.components));
            current_oid.count = varbind->oid.count;
        }

        callback(&varbind->oid, varbind, user_data);
        count++;

        /* Cleanup response pool */
        snmp_client_release_response(client);
    }

    free(current_oid.components);
    return count;
}

/* Get error message */
const char *snmp_client_get_error(snmp_client_t *client) {
    return client ? client->error_msg : "Invalid client";
}

/* Set timeout */
void snmp_client_set_timeout(snmp_client_t *client, uint32_t timeout_ms) {
    if (client) {
        client->timeout_ms = timeout_ms;
    }
}

/* Set retries */
void snmp_client_set_retries(snmp_client_t *client, uint32_t retries) {
    if (client) {
        client->retries = retries;
    }
}
