/**
 * @file snmp_walk_example.c
 * @brief Example: SNMP Walk (traverse MIB tree)
 *
 * Usage: ./snmp_walk_example <host> <community> <root_OID>
 * Example: ./snmp_walk_example 192.168.1.1 public 1.3.6.1.2.1.1
 */

#include "snmp_client.h"
#include <stdio.h>
#include <stdlib.h>

static void print_varbind(
    const snmp_oid_t *oid,
    const snmp_varbind_t *varbind,
    void *user_data
) {
    (void)user_data;

    /* Print OID */
    char oid_str[256];
    snmp_oid_to_string(oid, oid_str, sizeof(oid_str));
    printf("%s = ", oid_str);

    /* Print value */
    switch (varbind->value_type) {
        case SNMP_TYPE_INTEGER:
            printf("INTEGER: %d\n", varbind->value.i32);
            break;

        case SNMP_TYPE_OCTET_STRING:
            printf("STRING: %.*s\n",
                   (int)varbind->value.bytes.len,
                   (char *)varbind->value.bytes.data);
            break;

        case SNMP_TYPE_OID: {
            char val_oid[256];
            snmp_oid_to_string(&varbind->value.oid, val_oid, sizeof(val_oid));
            printf("OID: %s\n", val_oid);
            break;
        }

        case SNMP_TYPE_COUNTER32:
            printf("Counter32: %u\n", (uint32_t)varbind->value.i32);
            break;

        case SNMP_TYPE_GAUGE32:
            printf("Gauge32: %u\n", (uint32_t)varbind->value.i32);
            break;

        case SNMP_TYPE_TIMETICKS:
            printf("Timeticks: %u\n", (uint32_t)varbind->value.i32);
            break;

        case SNMP_TYPE_COUNTER64:
            printf("Counter64: %llu\n", (unsigned long long)varbind->value.i64);
            break;

        case SNMP_TYPE_IPADDRESS:
            if (varbind->value.bytes.len == 4) {
                printf("IpAddress: %u.%u.%u.%u\n",
                       varbind->value.bytes.data[0],
                       varbind->value.bytes.data[1],
                       varbind->value.bytes.data[2],
                       varbind->value.bytes.data[3]);
            }
            break;

        case SNMP_TYPE_NULL:
            printf("NULL\n");
            break;

        default:
            printf("(type: 0x%02X)\n", varbind->value_type);
            break;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <host> <community> <root_OID>\n", argv[0]);
        printf("Example: %s 192.168.1.1 public 1.3.6.1.2.1.1\n", argv[0]);
        printf("\nCommon root OIDs:\n");
        printf("  1.3.6.1.2.1.1         System group\n");
        printf("  1.3.6.1.2.1.2.2.1     Interface table\n");
        printf("  1.3.6.1.2.1.4         IP group\n");
        printf("  1.3.6.1.2.1.6.13      TCP connection table\n");
        return 1;
    }

    const char *host = argv[1];
    const char *community = argv[2];
    const char *root_oid_str = argv[3];

    /* Parse root OID */
    snmp_oid_t root_oid;
    if (snmp_oid_from_string(root_oid_str, &root_oid) != 0) {
        fprintf(stderr, "Error: Invalid OID: %s\n", root_oid_str);
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
        snmp_oid_free(&root_oid);
        return 1;
    }

    /* Perform SNMP walk */
    printf("Walking %s @ %s (community: %s)...\n\n",
           root_oid_str, host, community);

    int count = snmp_client_walk(client, &root_oid, print_varbind, NULL);

    if (count < 0) {
        fprintf(stderr, "\nError: %s (code: %d)\n",
                snmp_client_get_error(client), count);
        snmp_client_destroy(client);
        snmp_oid_free(&root_oid);
        return 1;
    }

    printf("\nTotal OIDs retrieved: %d\n", count);

    /* Cleanup */
    snmp_client_destroy(client);
    snmp_oid_free(&root_oid);

    return 0;
}
