/**
 * @file ldap_client.c
 * @brief LDAP Client Implementation with TCP Transport
 */

#include "ldap_client.h"
#include "ldap_builder.h"
#include "ldap_parser.h"
#include "turbo_tcp.h"
#include "arena_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <stb_sprintf.h>
#include "tlog.h"

/* Client error codes (match ldap_client.h) */
#define LDAP_CLIENT_OK              0
#define LDAP_CLIENT_ERROR_MEMORY   -1
#define LDAP_CLIENT_ERROR_INVALID  -2
#define LDAP_CLIENT_ERROR_NETWORK  -3
#define LDAP_CLIENT_ERROR_TIMEOUT  -4
#define LDAP_CLIENT_ERROR_PROTOCOL -5
#define LDAP_CLIENT_ERROR_AUTH     -6
#define LDAP_CLIENT_ERROR_TLS      -7

#define LDAP_DEFAULT_PORT 389
#define LDAP_DEFAULT_TIMEOUT_MS 30000
#define LDAP_RECV_BUFFER_SIZE 65536
#define LDAP_SEND_BUFFER_SIZE 4096

/* LDAP Client structure */
struct ldap_client_s {
    /* Configuration */
    char *host;
    uint16_t port;
    uint32_t timeout_ms;

    /* Runtime state */
    int32_t next_message_id;
    char error_msg[256];
    int last_result_code;

    /* TCP client */
    turbo_tcp_client_t *tcp;
    uv_loop_t *loop;
    int connected;

    /* Receive buffer */
    uint8_t *recv_buf;
    size_t recv_buf_size;
    size_t recv_buf_used;

    /* Response state (for synchronous wait) */
    ldap_message_t *pending_response;
    int pending_result;
    int response_received;
    int expected_message_id;
    uv_timer_t timeout_timer;

    /* Search callback state */
    ldap_search_cb search_callback;
    void *search_user_data;
    ldap_result_data_t *search_result;
};

/* Parse URL: ldap://host:port or ldaps://host:port */
static int parse_url(const char *url, char **host, uint16_t *port, int *use_tls) {
    *use_tls = 0;
    *port = LDAP_DEFAULT_PORT;

    if (strncmp(url, "ldaps://", 8) == 0) {
        *use_tls = 1;
        *port = 636;
        url += 8;
    } else if (strncmp(url, "ldap://", 7) == 0) {
        url += 7;
    }

    /* Find port separator */
    const char *colon = strchr(url, ':');
    const char *slash = strchr(url, '/');

    size_t host_len;
    if (colon && (!slash || colon < slash)) {
        host_len = colon - url;
        *port = (uint16_t)atoi(colon + 1);
    } else if (slash) {
        host_len = slash - url;
    } else {
        host_len = strlen(url);
    }

    *host = malloc(host_len + 1);
    if (!*host) return -1;
    memcpy(*host, url, host_len);
    (*host)[host_len] = '\0';

    return 0;
}

/* TCP receive callback */
static int on_tcp_recv(void *handle, const turbo_arena_slice_t *slice, void *peer) {
    (void)peer;
    turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
    ldap_client_t *client = (ldap_client_t *)tcp->user_data;

    if (!slice || !slice->data || slice->length == 0) return 0;

    /* Append to receive buffer */
    if (client->recv_buf_used + slice->length > client->recv_buf_size) {
        /* Buffer overflow - expand */
        size_t new_size = client->recv_buf_size * 2;
        uint8_t *new_buf = realloc(client->recv_buf, new_size);
        if (!new_buf) return -1;
        client->recv_buf = new_buf;
        client->recv_buf_size = new_size;
    }

    memcpy(client->recv_buf + client->recv_buf_used, slice->data, slice->length);
    client->recv_buf_used += slice->length;

    /* Try to parse complete messages */
    while (client->recv_buf_used > 0) {
        int complete = ldap_message_complete(client->recv_buf, client->recv_buf_used);
        if (complete <= 0) break;  /* Incomplete or error */

        ldap_parse_result_t parse_result;
        int rc = ldap_parse_message(client->recv_buf, client->recv_buf_used, &parse_result);

        if (rc == LDAP_PARSE_OK && parse_result.message) {
            ldap_message_t *msg = parse_result.message;
            TLOG_DEBUG("LDAP message received: ID={:d}, Op={:d}", msg->message_id, msg->protocol_op);

            /* Check if this is the response we're waiting for */
            if (msg->message_id == client->expected_message_id) {
                if (msg->protocol_op == LDAP_RES_SEARCH_ENTRY) {
                    /* Search entry - call callback */
                    if (client->search_callback) {
                        client->search_callback(client, &msg->payload.search_entry,
                                                client->search_user_data);
                    }
                    ldap_message_free(msg);
                } else if (msg->protocol_op == LDAP_RES_SEARCH_DONE) {
                    /* Search done - copy result and signal completion */
                    if (client->search_result) {
                        *client->search_result = msg->payload.search_done;
                        /* Clear so free doesn't double-free */
                        memset(&msg->payload.search_done, 0, sizeof(ldap_result_data_t));
                    }
                    client->last_result_code = msg->payload.search_done.result_code;
                    client->pending_result = LDAP_CLIENT_OK;
                    client->response_received = 1;
                    ldap_message_free(msg);
                    uv_stop(client->loop);
                } else {
                    /* Other response - store and signal */
                    client->pending_response = msg;
                    client->pending_result = LDAP_CLIENT_OK;
                    client->response_received = 1;
                    uv_stop(client->loop);
                }
            } else {
                /* Unexpected message ID - discard */
                ldap_message_free(msg);
            }

            /* Remove parsed data from buffer */
            size_t consumed = parse_result.bytes_consumed;
            if (consumed < client->recv_buf_used) {
                memmove(client->recv_buf, client->recv_buf + consumed,
                        client->recv_buf_used - consumed);
            }
            client->recv_buf_used -= consumed;
        } else {
            break;  /* Parse error */
        }
    }

    return 0;
}

/* TCP connect callback */
static void on_tcp_connect(void *handle, int status) {
    turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
    ldap_client_t *client = (ldap_client_t *)tcp->user_data;

    if (status == 0) {
        client->connected = 1;
        client->pending_result = LDAP_CLIENT_OK;
        TLOG_INFO("LDAP connected to {:s}:{:d}", client->host, client->port);
    } else {
        client->pending_result = LDAP_CLIENT_ERROR_NETWORK;
        stbsp_snprintf(client->error_msg, sizeof(client->error_msg),
                       "Connection failed: %s", uv_strerror(status));
        TLOG_ERROR("LDAP connection failed to {:s}:{:d}: {:s}", 
                   client->host, client->port, client->error_msg);
    }
    client->response_received = 1;
    uv_stop(client->loop);
}

/* TCP close callback */
static void on_tcp_close(void *handle) {
    turbo_tcp_client_t *tcp = (turbo_tcp_client_t *)handle;
    ldap_client_t *client = (ldap_client_t *)tcp->user_data;
    client->connected = 0;
}

/* Timeout callback */
static void on_timeout(uv_timer_t *timer) {
    ldap_client_t *client = (ldap_client_t *)timer->data;

    if (!client->response_received) {
        client->pending_result = LDAP_CLIENT_ERROR_TIMEOUT;
        strcpy(client->error_msg, "Operation timeout");
        TLOG_ERROR("LDAP operation timeout for {:s}:{:d}", client->host, client->port);
        uv_stop(client->loop);
    }
}

/* Send request and wait for response */
static int send_and_wait(ldap_client_t *client, const uint8_t *data, size_t len, int message_id) {
    if (!client->connected) {
        strcpy(client->error_msg, "Not connected");
        return LDAP_CLIENT_ERROR_NETWORK;
    }

    /* Reset state */
    client->pending_response = NULL;
    client->pending_result = LDAP_CLIENT_ERROR_TIMEOUT;
    client->response_received = 0;
    client->expected_message_id = message_id;

    /* Send request */
    int rc = turbo_tcp_send(client->tcp, (const char *)data, len);
    if (rc != 0) {
        strcpy(client->error_msg, "Send failed");
        return LDAP_CLIENT_ERROR_NETWORK;
    }

    /* Start timeout timer */
    uv_timer_start(&client->timeout_timer, on_timeout, client->timeout_ms, 0);

    /* Run event loop until response or timeout */
    uv_run(client->loop, UV_RUN_DEFAULT);

    /* Stop timer */
    uv_timer_stop(&client->timeout_timer);

    return client->pending_result;
}

/* Create client */
ldap_client_t *ldap_client_create(const ldap_client_config_t *config) {
    ldap_client_t *client = calloc(1, sizeof(ldap_client_t));
    if (!client) return NULL;

    /* Parse URL */
    int use_tls = 0;
    if (config && config->url) {
        if (parse_url(config->url, &client->host, &client->port, &use_tls) != 0) {
            free(client);
            return NULL;
        }
    } else {
        client->host = strdup("localhost");
        client->port = LDAP_DEFAULT_PORT;
    }

    client->timeout_ms = (config && config->timeout_ms > 0) ?
                         config->timeout_ms : LDAP_DEFAULT_TIMEOUT_MS;
    client->next_message_id = 1;

    /* Allocate receive buffer */
    client->recv_buf_size = LDAP_RECV_BUFFER_SIZE;
    client->recv_buf = malloc(client->recv_buf_size);
    if (!client->recv_buf) {
        free(client->host);
        free(client);
        return NULL;
    }

    /* Create event loop */
    client->loop = malloc(sizeof(uv_loop_t));
    if (!client->loop || uv_loop_init(client->loop) != 0) {
        free(client->recv_buf);
        free(client->host);
        free(client->loop);
        free(client);
        return NULL;
    }

    /* Initialize timeout timer */
    uv_timer_init(client->loop, &client->timeout_timer);
    client->timeout_timer.data = client;

    TLOG_INFO("LDAP client created for {:s}:{:d}", client->host, client->port);

    return client;
}

/* Destroy client */
void ldap_client_destroy(ldap_client_t *client) {
    if (!client) return;

    if (client->connected && client->tcp) {
        turbo_tcp_client_close(client->tcp);
        uv_run(client->loop, UV_RUN_DEFAULT);
    }

    uv_timer_stop(&client->timeout_timer);
    uv_close((uv_handle_t *)&client->timeout_timer, NULL);
    uv_run(client->loop, UV_RUN_NOWAIT);

    uv_loop_close(client->loop);
    free(client->loop);
    free(client->recv_buf);
    free(client->host);
    free(client);
}

/* Connect to server */
int ldap_client_connect(ldap_client_t *client) {
    if (!client) return LDAP_CLIENT_ERROR_INVALID;
    if (client->connected) return LDAP_CLIENT_OK;

    /* Create TCP client */
    client->tcp = turbo_tcp_client_create(client->loop);
    if (!client->tcp) {
        strcpy(client->error_msg, "Failed to create TCP client");
        return LDAP_CLIENT_ERROR_MEMORY;
    }

    client->tcp->user_data = client;

    /* Reset state */
    client->pending_result = LDAP_CLIENT_ERROR_TIMEOUT;
    client->response_received = 0;

    /* Connect */
    int rc = turbo_tcp_client_connect(client->tcp, client->host, client->port,
                                       on_tcp_recv, on_tcp_connect, on_tcp_close);
    if (rc != 0) {
        stbsp_snprintf(client->error_msg, sizeof(client->error_msg),
                       "Connect failed: %s", uv_strerror(rc));
        return LDAP_CLIENT_ERROR_NETWORK;
    }

    /* Start timeout timer */
    uv_timer_start(&client->timeout_timer, on_timeout, client->timeout_ms, 0);

    /* Wait for connection */
    uv_run(client->loop, UV_RUN_DEFAULT);

    uv_timer_stop(&client->timeout_timer);

    return client->pending_result;
}

/* Simple bind */
int ldap_client_simple_bind(ldap_client_t *client, const char *dn, const char *password,
                            ldap_result_data_t *result) {
    if (!client) return LDAP_CLIENT_ERROR_INVALID;

    /* Auto-connect if needed */
    if (!client->connected) {
        int rc = ldap_client_connect(client);
        if (rc != LDAP_CLIENT_OK) return rc;
    }

    /* Build bind request */
    uint8_t buf[LDAP_SEND_BUFFER_SIZE];
    size_t len = sizeof(buf);
    int message_id = client->next_message_id++;

    int rc = ldap_build_bind_request(message_id, 3, dn, password, buf, &len);
    if (rc != LDAP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build BindRequest");
        TLOG_ERROR("LDAP build error: {:s}", client->error_msg);
        return LDAP_CLIENT_ERROR_INVALID;
    }

    /* Send and wait */
    rc = send_and_wait(client, buf, len, message_id);
    if (rc != LDAP_CLIENT_OK) return rc;

    /* Process response */
    if (client->pending_response) {
        ldap_message_t *msg = client->pending_response;
        client->last_result_code = msg->payload.bind_response.result_code;

        if (result) {
            *result = msg->payload.bind_response;
            memset(&msg->payload.bind_response, 0, sizeof(ldap_result_data_t));
        }

        ldap_message_free(msg);
        client->pending_response = NULL;

        if (client->last_result_code != LDAP_SUCCESS) {
            return LDAP_CLIENT_ERROR_AUTH;
        }
    }

    return LDAP_CLIENT_OK;
}

/* Unbind */
int ldap_client_unbind(ldap_client_t *client) {
    if (!client || !client->connected) return LDAP_CLIENT_ERROR_INVALID;

    uint8_t buf[64];
    size_t len = sizeof(buf);
    int message_id = client->next_message_id++;

    int rc = ldap_build_unbind_request(message_id, buf, &len);
    if (rc != LDAP_BUILD_OK) return LDAP_CLIENT_ERROR_INVALID;

    /* Send unbind (no response expected) */
    turbo_tcp_send(client->tcp, (const char *)buf, len);

    /* Close connection */
    turbo_tcp_client_close(client->tcp);
    client->connected = 0;

    return LDAP_CLIENT_OK;
}

/* Search */
int ldap_client_search(ldap_client_t *client, const ldap_search_params_t *params,
                       ldap_search_cb callback, void *user_data, ldap_result_data_t *result) {
    if (!client || !params) return LDAP_CLIENT_ERROR_INVALID;

    /* Auto-connect if needed */
    if (!client->connected) {
        int rc = ldap_client_connect(client);
        if (rc != LDAP_CLIENT_OK) return rc;
    }

    /* Build search request */
    uint8_t buf[LDAP_SEND_BUFFER_SIZE];
    size_t len = sizeof(buf);
    int message_id = client->next_message_id++;

    int rc = ldap_build_search_request(
        message_id,
        params->base_dn,
        params->scope,
        0,  /* derefAliases */
        params->size_limit,
        params->time_limit,
        params->types_only ? 1 : 0,
        params->filter,
        params->attrs,
        buf, &len
    );

    if (rc != LDAP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build SearchRequest");
        return LDAP_CLIENT_ERROR_INVALID;
    }

    /* Set up callback state */
    client->search_callback = callback;
    client->search_user_data = user_data;
    client->search_result = result;

    /* Send and wait for SearchResultDone */
    rc = send_and_wait(client, buf, len, message_id);

    /* Clear callback state */
    client->search_callback = NULL;
    client->search_user_data = NULL;
    client->search_result = NULL;

    return rc;
}

/* Add */
int ldap_client_add(ldap_client_t *client, const char *dn,
                    const ldap_attribute_t *attrs, size_t attr_count,
                    ldap_result_data_t *result) {
    if (!client || !dn) return LDAP_CLIENT_ERROR_INVALID;

    if (!client->connected) {
        int rc = ldap_client_connect(client);
        if (rc != LDAP_CLIENT_OK) return rc;
    }

    uint8_t buf[LDAP_SEND_BUFFER_SIZE];
    size_t len = sizeof(buf);
    int message_id = client->next_message_id++;

    int rc = ldap_build_add_request(message_id, dn, attrs, attr_count, buf, &len);
    if (rc != LDAP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build AddRequest");
        return LDAP_CLIENT_ERROR_INVALID;
    }

    rc = send_and_wait(client, buf, len, message_id);
    if (rc != LDAP_CLIENT_OK) return rc;

    if (client->pending_response) {
        ldap_message_t *msg = client->pending_response;
        client->last_result_code = msg->payload.generic_result.result_code;
        if (result) {
            *result = msg->payload.generic_result;
            memset(&msg->payload.generic_result, 0, sizeof(ldap_result_data_t));
        }
        ldap_message_free(msg);
        client->pending_response = NULL;
    }

    return LDAP_CLIENT_OK;
}

/* Delete */
int ldap_client_delete(ldap_client_t *client, const char *dn, ldap_result_data_t *result) {
    if (!client || !dn) return LDAP_CLIENT_ERROR_INVALID;

    if (!client->connected) {
        int rc = ldap_client_connect(client);
        if (rc != LDAP_CLIENT_OK) return rc;
    }

    uint8_t buf[LDAP_SEND_BUFFER_SIZE];
    size_t len = sizeof(buf);
    int message_id = client->next_message_id++;

    int rc = ldap_build_delete_request(message_id, dn, buf, &len);
    if (rc != LDAP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build DeleteRequest");
        return LDAP_CLIENT_ERROR_INVALID;
    }

    rc = send_and_wait(client, buf, len, message_id);
    if (rc != LDAP_CLIENT_OK) return rc;

    if (client->pending_response) {
        ldap_message_t *msg = client->pending_response;
        client->last_result_code = msg->payload.generic_result.result_code;
        if (result) {
            *result = msg->payload.generic_result;
            memset(&msg->payload.generic_result, 0, sizeof(ldap_result_data_t));
        }
        ldap_message_free(msg);
        client->pending_response = NULL;
    }

    return LDAP_CLIENT_OK;
}

/* Modify */
int ldap_client_modify(ldap_client_t *client, const char *dn,
                       const ldap_modification_t *mods, size_t mod_count,
                       ldap_result_data_t *result) {
    if (!client || !dn) return LDAP_CLIENT_ERROR_INVALID;

    if (!client->connected) {
        int rc = ldap_client_connect(client);
        if (rc != LDAP_CLIENT_OK) return rc;
    }

    uint8_t buf[LDAP_SEND_BUFFER_SIZE];
    size_t len = sizeof(buf);
    int message_id = client->next_message_id++;

    int rc = ldap_build_modify_request(message_id, dn, mods, mod_count, buf, &len);
    if (rc != LDAP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build ModifyRequest");
        return LDAP_CLIENT_ERROR_INVALID;
    }

    rc = send_and_wait(client, buf, len, message_id);
    if (rc != LDAP_CLIENT_OK) return rc;

    if (client->pending_response) {
        ldap_message_t *msg = client->pending_response;
        client->last_result_code = msg->payload.generic_result.result_code;
        if (result) {
            *result = msg->payload.generic_result;
            memset(&msg->payload.generic_result, 0, sizeof(ldap_result_data_t));
        }
        ldap_message_free(msg);
        client->pending_response = NULL;
    }

    return LDAP_CLIENT_OK;
}

/* Rename */
int ldap_client_rename(ldap_client_t *client, const char *dn, const char *new_rdn,
                       const char *new_parent, int delete_old_rdn,
                       ldap_result_data_t *result) {
    if (!client || !dn || !new_rdn) return LDAP_CLIENT_ERROR_INVALID;

    if (!client->connected) {
        int rc = ldap_client_connect(client);
        if (rc != LDAP_CLIENT_OK) return rc;
    }

    uint8_t buf[LDAP_SEND_BUFFER_SIZE];
    size_t len = sizeof(buf);
    int message_id = client->next_message_id++;

    int rc = ldap_build_modifydn_request(message_id, dn, new_rdn, delete_old_rdn,
                                          new_parent, buf, &len);
    if (rc != LDAP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build ModifyDNRequest");
        return LDAP_CLIENT_ERROR_INVALID;
    }

    rc = send_and_wait(client, buf, len, message_id);
    if (rc != LDAP_CLIENT_OK) return rc;

    if (client->pending_response) {
        ldap_message_t *msg = client->pending_response;
        client->last_result_code = msg->payload.generic_result.result_code;
        if (result) {
            *result = msg->payload.generic_result;
            memset(&msg->payload.generic_result, 0, sizeof(ldap_result_data_t));
        }
        ldap_message_free(msg);
        client->pending_response = NULL;
    }

    return LDAP_CLIENT_OK;
}

/* Compare */
int ldap_client_compare(ldap_client_t *client, const char *dn, const char *attr,
                        const char *value, size_t value_len, ldap_result_data_t *result) {
    if (!client || !dn || !attr || !value) return LDAP_CLIENT_ERROR_INVALID;

    if (!client->connected) {
        int rc = ldap_client_connect(client);
        if (rc != LDAP_CLIENT_OK) return rc;
    }

    uint8_t buf[LDAP_SEND_BUFFER_SIZE];
    size_t len = sizeof(buf);
    int message_id = client->next_message_id++;

    int rc = ldap_build_compare_request(message_id, dn, attr,
                                         (const uint8_t *)value, value_len, buf, &len);
    if (rc != LDAP_BUILD_OK) {
        strcpy(client->error_msg, "Failed to build CompareRequest");
        return LDAP_CLIENT_ERROR_INVALID;
    }

    rc = send_and_wait(client, buf, len, message_id);
    if (rc != LDAP_CLIENT_OK) return rc;

    if (client->pending_response) {
        ldap_message_t *msg = client->pending_response;
        client->last_result_code = msg->payload.generic_result.result_code;
        if (result) {
            *result = msg->payload.generic_result;
            memset(&msg->payload.generic_result, 0, sizeof(ldap_result_data_t));
        }
        ldap_message_free(msg);
        client->pending_response = NULL;
    }

    return LDAP_CLIENT_OK;
}

/* Error string */
const char *ldap_err2string(int err) {
    switch (err) {
        case LDAP_CLIENT_OK:             return "Success";
        case LDAP_CLIENT_ERROR_MEMORY:   return "Memory allocation failed";
        case LDAP_CLIENT_ERROR_INVALID:  return "Invalid parameter";
        case LDAP_CLIENT_ERROR_NETWORK:  return "Network error";
        case LDAP_CLIENT_ERROR_TIMEOUT:  return "Operation timeout";
        case LDAP_CLIENT_ERROR_PROTOCOL: return "Protocol error";
        case LDAP_CLIENT_ERROR_AUTH:     return "Authentication failed";
        case LDAP_CLIENT_ERROR_TLS:      return "TLS error";
        default:                         return "Unknown error";
    }
}
