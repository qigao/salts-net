/**
 * @file snmp_get_example.c
 * @brief Example: SNMP Get Request
 *
 * Usage: ./snmp_get_example <host> <community> <OID>
 * Example: ./snmp_get_example 192.168.1.1 public 1.3.6.1.2.1.1.1.0
 */

#include "snmp_client.h"
#include <stdio.h>
#include <stdlib.h>

static void print_value(const snmp_varbind_t *varbind) {
    switch (varbind->value_type) {
        case SNMP_TYPE_INTEGER:
            printf("%d\n", varbind->value.i32);
            break;

        case SNMP_TYPE_OCTET_STRING:
            printf("%.*s\n",
                   (int)varbind->value.bytes.len,
                   (char *)varbind->value.bytes.data);
            break;

        case SNMP_TYPE_OID: {
            char oid_str[256];
            snmp_oid_to_string(&varbind->value.oid, oid_str, sizeof(oid_str));
            printf("%s\n", oid_str);
            break;
        }

        case SNMP_TYPE_COUNTER32:
        case SNMP_TYPE_GAUGE32:
        case SNMP_TYPE_TIMETICKS:
            printf("%u\n", (uint32_t)varbind->value.i32);
            break;

        case SNMP_TYPE_COUNTER64:
            printf("%lld\n", (long long)varbind->value.i64);
            break;

        case SNMP_TYPE_IPADDRESS:
            if (varbind->value.bytes.len == 4) {
                printf("%u.%u.%u.%u\n",
                       varbind->value.bytes.data[0],
                       varbind->value.bytes.data[1],
                       varbind->value.bytes.data[2],
                       varbind->value.bytes.data[3]);
            }
            break;

        case SNMP_TYPE_NULL:
            printf("(null)\n");
            break;

        case SNMP_TYPE_NOSUCHOBJECT:
            printf("(no such object)\n");
            break;

        case SNMP_TYPE_NOSUCHINSTANCE:
            printf("(no such instance)\n");
            break;

        case SNMP_TYPE_ENDOFMIBVIEW:
            printf("(end of MIB view)\n");
            break;

        default:
            printf("(unknown type: 0x%02X)\n", varbind->value_type);
            break;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <host> <community> <OID>\n", argv[0]);
        printf("Example: %s 192.168.1.1 public 1.3.6.1.2.1.1.1.0\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    const char *community = argv[2];
    const char *oid_str = argv[3];

    /* Parse OID */
    snmp_oid_t oid;
    if (snmp_oid_from_string(oid_str, &oid) != 0) {
        fprintf(stderr, "Error: Invalid OID: %s\n", oid_str);
        return 1;
    }

    /* Create SNMP client */
    snmp_client_config_t config = {
        .host = host,
        .port = 161,
        .community = community,
        .version = SNMP_VERSION_2C,
        .timeout_ms = 5000,
        .retries = 3
    };

    snmp_client_t *client = snmp_client_create(&config);
    if (!client) {
        fprintf(stderr, "Error: Failed to create SNMP client\n");
        snmp_oid_free(&oid);
        return 1;
    }

    /* Send GetRequest */
    printf("Querying %s @ %s (community: %s)...\n", oid_str, host, community);

    snmp_message_t response;
    int result = snmp_client_get(client, &oid, 1, &response);

    if (result != SNMP_CLIENT_OK) {
        fprintf(stderr, "Error: %s (code: %d)\n",
                snmp_client_get_error(client), result);
        snmp_client_destroy(client);
        snmp_oid_free(&oid);
        return 1;
    }

    /* Print result */
    if (response.pdu.varbind_count > 0) {
        printf("%s = ", oid_str);
        print_value(&response.pdu.varbinds[0]);
    } else {
        printf("(no data)\n");
    }

    /* Cleanup */
    snmp_client_destroy(client);
    snmp_oid_free(&oid);

    return 0;
}
