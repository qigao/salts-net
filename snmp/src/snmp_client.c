/**
 * @file snmp_client.c
 * @brief SNMP Client Implementation with UDP Transport
 */

#include "snmp_client.h"
#include "turbo_udp.h"
#include "arena_buffer.h"
#include "memory_pool.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#define STB_SPRINTF_IMPLEMENTATION
#include <stb_sprintf.h>

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

    /* UDP client */
    turbo_udp_client_t udp;
    uv_loop_t *loop;

    /* Response state (for synchronous wait) */
    snmp_message_t *pending_response;
    int pending_result;
    int response_received;
    uv_timer_t *timeout_timer;
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

/* UDP receive callback */
static int on_udp_recv(
    void *handle,
    const turbo_arena_slice_t *slice,
    void *peer
) {
    (void)peer;

    snmp_client_t *client = (snmp_client_t *)handle;

    if (!client->response_received && slice && slice->data && slice->length > 0) {
        /* Parse SNMP response */
        int result = snmp_parse(
            (const uint8_t *)slice->data,
            slice->length,
            client->pending_response,
            client->response_pool
        );

        if (result > 0) {
            client->pending_result = SNMP_CLIENT_OK;
            client->response_received = 1;

            /* Stop event loop */
            uv_stop(client->loop);
        }
    }

    return 0;  /* Don't close connection */
}

/* Timeout callback */
static void on_timeout(uv_timer_t *timer) {
    snmp_client_t *client = (snmp_client_t *)timer->data;

    if (!client->response_received) {
        client->pending_result = SNMP_CLIENT_ERROR_TIMEOUT;
        strcpy(client->error_msg, "Request timeout");

        /* Stop event loop */
        uv_stop(client->loop);
    }
}

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

    /* Create event loop */
    client->loop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    if (!client->loop || uv_loop_init(client->loop) != 0) {
        free(client->host);
        free(client->community);
        free(client->loop);
        free(client);
        return NULL;
    }

    /* Initialize UDP client */
    if (turbo_udp_server_init(&client->udp, client->loop, "0.0.0.0", 0) != 0) {
        uv_loop_close(client->loop);
        free(client->loop);
        free(client->host);
        free(client->community);
        free(client);
        return NULL;
    }

    /* Connect to target */
    if (turbo_udp_connect(&client->udp, client->host, client->port) != 0) {
        turbo_udp_server_stop(&client->udp);
        uv_loop_close(client->loop);
        free(client->loop);
        free(client->host);
        free(client->community);
        free(client);
        return NULL;
    }

    /* Start receiving */
    turbo_udp_server_start(&client->udp, on_udp_recv);

    return client;
}

/* Destroy client */
void snmp_client_destroy(snmp_client_t *client) {
    if (!client) return;

    turbo_udp_server_stop(&client->udp);
    uv_loop_close(client->loop);

    free(client->loop);
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

    while (attempt <= client->retries) {
        /* Create memory pool for response */
        client->response_pool = pool_create(client->recv_buffer_size);
        if (!client->response_pool) {
            return SNMP_CLIENT_ERROR_MEMORY;
        }

        /* Reset state */
        client->pending_response = response;
        client->pending_result = SNMP_CLIENT_ERROR_TIMEOUT;
        client->response_received = 0;

        /* Send request */
        int send_result = turbo_udp_send_connected(
            &client->udp,
            (const char *)request,
            request_len
        );

        if (send_result != 0) {
            pool_destroy(client->response_pool);
            return SNMP_CLIENT_ERROR_NETWORK;
        }

        /* Create timeout timer */
        uv_timer_t timeout_timer;
        uv_timer_init(client->loop, &timeout_timer);
        timeout_timer.data = client;
        client->timeout_timer = &timeout_timer;

        uv_timer_start(&timeout_timer, on_timeout, client->timeout_ms, 0);

        /* Run event loop until response or timeout */
        uv_run(client->loop, UV_RUN_DEFAULT);

        /* Stop timer */
        uv_timer_stop(&timeout_timer);
        uv_close((uv_handle_t *)&timeout_timer, NULL);
        uv_run(client->loop, UV_RUN_NOWAIT);  /* Process close callback */

        /* Check result */
        if (client->response_received) {
            /* Success - keep pool alive for response data */
            return SNMP_CLIENT_OK;
        }

        /* Failed - cleanup and retry */
        pool_destroy(client->response_pool);
        client->response_pool = NULL;
        attempt++;
    }

    /* All retries exhausted */
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg),
             "Request timeout after %u retries", client->retries);
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
        return SNMP_CLIENT_ERROR_INVALID;
    }

    /* Send and wait */
    int result = send_request_and_wait(client, request, request_len, response);

    if (result == SNMP_CLIENT_OK) {
        /* Check for SNMP errors */
        if (response->pdu.error_status != SNMP_ERROR_NOERROR) {
            stbsp_snprintf(client->error_msg, sizeof(client->error_msg),
                     "SNMP error: %d (index: %d)",
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
            stbsp_snprintf(client->error_msg, sizeof(client->error_msg),
                     "SNMP error: %d", response->pdu.error_status);
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
