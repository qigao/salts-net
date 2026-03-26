/**
 * @file snmp_client.c
 * @brief SNMP Client Implementation with UDP Transport
 */

#include "snmp_client.h"
#include "CoroNet.h"
#include "turbo_buffer.h"
#include "memory_pool.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>
#include "tlog.h"

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
    snmp_v3_user_t v3_user;              /* User credentials */
    snmp_engine_time_t engine_time;       /* Local/remote engine time */
    uint8_t engine_id[32];                /* Remote engine ID */
    size_t engine_id_len;
    int engine_discovered;                /* Engine discovery complete */

    /* Runtime state */
    int32_t next_request_id;
    char error_msg[256];

    /* High-level synchronous client (now using coro_socket) */
    coro_socket_t *sock;
    MemoryPool *response_pool;
};

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

/* No longer needed: on_udp_recv, on_timeout */

/* Create client */
snmp_client_t *snmp_client_create(const snmp_client_config_t *config) {
    snmp_client_config_t default_config;
    if (!config) {
        init_default_config(&default_config);
        config = &default_config;
    }

    snmp_client_t *client = (snmp_client_t *)calloc(1, sizeof(snmp_client_t));
    if (!client) {
        return NULL;
    }

    /* Copy configuration */
    client->host = strdup(config->host);
    client->community = strdup(config->community);
    client->port = config->port;
    client->version = config->version;
    client->timeout_ms = config->timeout_ms;
    client->retries = config->retries;
    client->recv_buffer_size = config->recv_buffer_size;
    client->next_request_id = 1;

    /* Initialize SNMPv3 if needed */
    if (config->version == SNMP_VERSION_3) {
        client->security_level = config->security_level;
        client->engine_discovered = 0;
        client->engine_id_len = 0;

        /* Initialize engine time (will be updated during engine discovery) */
        usm_engine_time_init(&client->engine_time, 0);

        /* Create user from passwords (temporary, will be localized after engine discovery) */
        if (config->security_name && config->security_level != SNMP_SEC_LEVEL_NOAUTH_NOPRIV) {
            /* Use dummy engine ID for now, will re-localize after discovery */
            uint8_t dummy_engine_id[] = {0x80, 0x00, 0x00, 0x00, 0x00};

            int result = usm_create_user(
                config->security_name,
                config->auth_password,
                config->auth_protocol,
                config->priv_password,
                config->priv_protocol,
                dummy_engine_id,
                sizeof(dummy_engine_id),
                &client->v3_user
            );

            if (result != USM_OK) {
                free(client->host);
                free(client->community);
                free(client);
                return NULL;
            }
        } else if (config->security_name) {
            /* noAuthNoPriv - just set user name */
            client->v3_user.user_name = strdup(config->security_name);
            client->v3_user.auth_protocol = SNMP_AUTH_NONE;
            client->v3_user.priv_protocol = SNMP_PRIV_NONE;
        }
    }

    /* Get current coroutine context */
    coro_context_t *ctx = coro_context_current();
    if (!ctx) {
        TLOG_ERROR("SNMP client must be created within a coroutine context");
        free(client->host);
        free(client->community);
        free(client);
        return NULL;
    }

    /* Initialize socket */
    client->sock = coro_socket_create(ctx, CORO_SOCKET_UDP_V4);
    if (!client->sock) {
        free(client->host);
        free(client->community);
        free(client);
        return NULL;
    }

    if (coro_socket_connect(client->sock, client->host, client->port) != 0) {
        TLOG_ERROR("SNMP failed to connect to {:s}:{:d}", client->host, client->port);
        coro_socket_destroy(client->sock);
        free(client->host);
        free(client->community);
        free(client);
        return NULL;
    }

    TLOG_INFO("SNMP client created for {:s}:{:d} (version: {:d})",
              client->host, client->port, (int)client->version);

    return client;
}

/* Destroy client */
void snmp_client_destroy(snmp_client_t *client) {
    if (!client) return;

    coro_socket_destroy(client->sock);
    free(client->host);
    free(client->community);
    free(client);
}

/* Send request and wait for response (with retry) */
static int send_request_and_wait(
    snmp_client_t *client,
    const uint8_t *request,
    size_t request_len,
    snmp_message_t *response
) {
    uint32_t attempt = 0;

    /* Set socket timeout */
    coro_socket_set_timeout(client->sock, client->timeout_ms);

    while (attempt <= client->retries) {
        /* Send request */
        if (coro_socket_send(client->sock, request, request_len) != 0) {
            TLOG_DEBUG("SNMP send error on attempt {:d}", attempt + 1);
            return SNMP_CLIENT_ERROR_NETWORK;
        }

        /* Receive response */
        char *data = NULL;
        size_t len = 0;
        int res = coro_socket_recv(client->sock, &data, &len);

        if (res == 0) {
            /* Create memory pool for response */
            client->response_pool = pool_create(client->recv_buffer_size);
            if (!client->response_pool) {
                coro_socket_free_recv(data);
                return SNMP_CLIENT_ERROR_MEMORY;
            }

            /* Parse SNMP response */
            int result = snmp_parse((const uint8_t *)data, len, response, client->response_pool);
            coro_socket_free_recv(data);

            if (result > 0) {
                TLOG_DEBUG("SNMP response received ({:d} bytes)", (int)len);
                return SNMP_CLIENT_OK;
            } else {
                TLOG_DEBUG("SNMP parse error in response from {:s}", client->host);
                pool_destroy(client->response_pool);
                client->response_pool = NULL;
                /* Might be a malformed packet, try next attempt */
            }
        } else if (res == TURBO_ETIMEDOUT) {
            TLOG_DEBUG("SNMP attempt {:d} timed out for {:s}", attempt + 1, client->host);
        } else {
            /* For actual network errors, fail immediately */
            TLOG_DEBUG("SNMP network error: {:d}", res);
            return SNMP_CLIENT_ERROR_NETWORK;
        }

        /* Timeout or parsing error - retry */
        attempt++;
    }

    /* All retries exhausted */
    fmt(client->error_msg, sizeof(client->error_msg),
        "Request timeout after {} retries", client->retries);
    return SNMP_CLIENT_ERROR_TIMEOUT;
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

    /* Build GetRequest */
    uint8_t request[1024];
    size_t request_len = sizeof(request);

    int build_result = snmp_build_get_request(
        client->version,
        client->community,
        client->next_request_id++,
        oids,
        oid_count,
        request,
        &request_len
    );

    if (build_result != SNMP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build GetRequest");
        TLOG_ERROR("SNMP build error: {:s}", client->error_msg);
        return SNMP_CLIENT_ERROR_INVALID;
    }

    /* Send and wait */
    int result = send_request_and_wait(client, request, request_len, response);

    if (result == SNMP_CLIENT_OK) {
        /* Check for SNMP errors */
        if (response->pdu.error_status != SNMP_ERROR_NOERROR) {
            fmt(client->error_msg, sizeof(client->error_msg),
                "SNMP error: {} (index: {})",
                response->pdu.error_status,
                response->pdu.error_index);
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

    /* Build GetNextRequest */
    uint8_t request[1024];
    size_t request_len = sizeof(request);

    int build_result = snmp_build_get_next_request(
        client->version,
        client->community,
        client->next_request_id++,
        oids,
        oid_count,
        request,
        &request_len
    );

    if (build_result != SNMP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build GetNextRequest");
        return SNMP_CLIENT_ERROR_INVALID;
    }

    /* Send and wait */
    int result = send_request_and_wait(client, request, request_len, response);

    if (result == SNMP_CLIENT_OK) {
        /* Check for SNMP errors */
        if (response->pdu.error_status != SNMP_ERROR_NOERROR) {
            fmt(client->error_msg, sizeof(client->error_msg),
                "SNMP error: {}", response->pdu.error_status);
            return SNMP_CLIENT_ERROR_SNMP;
        }
    }

    return result;
}

/* SNMP Set (stub) */
int snmp_client_set(
    snmp_client_t *client,
    const snmp_varbind_t *varbinds,
    size_t varbind_count,
    snmp_message_t *response
) {
    (void)client;
    (void)varbinds;
    (void)varbind_count;
    (void)response;
    return SNMP_CLIENT_ERROR_INVALID;  /* Not implemented yet */
}

/* SNMP Walk */
int snmp_client_walk(
    snmp_client_t *client,
    const snmp_oid_t *root_oid,
    snmp_walk_cb callback,
    void *user_data
) {
    if (!client || !root_oid || !callback) {
        return SNMP_CLIENT_ERROR_INVALID;
    }

    snmp_oid_t current_oid = *root_oid;
    int count = 0;
    const int MAX_WALK = 1000;  /* Safety limit */

    while (count < MAX_WALK) {
        snmp_message_t response;
        int result = snmp_client_get_next(client, &current_oid, 1, &response);

        if (result != SNMP_CLIENT_OK) {
            break;  /* Error or timeout */
        }

        /* Check if we're still under the root OID */
        if (response.pdu.varbind_count == 0) {
            break;  /* No more OIDs */
        }

        snmp_varbind_t *varbind = &response.pdu.varbinds[0];

        /* Check for end-of-MIB */
        if (varbind->value_type == SNMP_TYPE_ENDOFMIBVIEW ||
            varbind->value_type == SNMP_TYPE_NOSUCHOBJECT ||
            varbind->value_type == SNMP_TYPE_NOSUCHINSTANCE) {
            pool_destroy(client->response_pool);
            break;
        }

        /* Check if still under root */
        size_t cmp_len = (root_oid->count < varbind->oid.count) ?
                         root_oid->count : varbind->oid.count;
        int still_under_root = 1;

        for (size_t i = 0; i < cmp_len; i++) {
            if (varbind->oid.components[i] != root_oid->components[i]) {
                still_under_root = 0;
                break;
            }
        }

        if (!still_under_root) {
            pool_destroy(client->response_pool);
            break;
        }

        /* Call user callback */
        callback(&varbind->oid, varbind, user_data);

        /* Move to next OID */
        current_oid = varbind->oid;
        count++;

        /* Cleanup response pool */
        pool_destroy(client->response_pool);
        client->response_pool = NULL;
    }

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
