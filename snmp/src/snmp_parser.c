/**
 * @file snmp_parser.c
 * @brief SNMP Message Parser Implementation
 */

#include "snmp_parser.h"
#include "snmp_usm_wire.h"
#include "snmp_usm.h"
#include "asn1_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* Helper: Parse OID from ASN.1 value */
static int parse_oid(const asn1_value_t *asn1_oid, snmp_oid_t *oid, MemoryPool *pool) {
  if (!asn1_oid || asn1_oid->type != ASN1_TYPE_OBJECT_IDENTIFIER) {
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  if (pool) {
    oid->components = (uint32_t *)pool_alloc(pool, asn1_oid->value.oid.count * sizeof(uint32_t));
  } else {
    oid->components = (uint32_t *)malloc(asn1_oid->value.oid.count * sizeof(uint32_t));
  }

  if (!oid->components) {
    return SNMP_PARSE_ERROR_MEMORY;
  }

  memcpy(oid->components, asn1_oid->value.oid.components,
         asn1_oid->value.oid.count * sizeof(uint32_t));
  oid->count = asn1_oid->value.oid.count;

  return SNMP_PARSE_OK;
}

static void *snmp_parse_alloc(MemoryPool *pool, size_t size) {
  return pool ? pool_alloc(pool, size) : malloc(size);
}

/* Helper: Parse VarBind value */
static int parse_varbind_value(const asn1_value_t *asn1_val, snmp_varbind_t *varbind,
                               MemoryPool *pool) {
  if (!asn1_val || !varbind) {
    return SNMP_PARSE_ERROR_INVALID;
  }

  // Map ASN.1 types to SNMP types
  switch (asn1_val->type) {
  case ASN1_TYPE_INTEGER:
    varbind->value_type = SNMP_TYPE_INTEGER;
    varbind->value.i32 = (int32_t)asn1_val->value.integer;
    break;

  case ASN1_TYPE_OCTET_STRING:
    varbind->value_type = SNMP_TYPE_OCTET_STRING;
    varbind->value.bytes.len = asn1_val->value.octet_string.length;
    
    /* Copy the data instead of just copying the pointer */
    if (asn1_val->value.octet_string.length > 0 && asn1_val->value.octet_string.data) {
      if (pool) {
        varbind->value.bytes.data = (uint8_t *)pool_alloc(pool, asn1_val->value.octet_string.length);
      } else {
        varbind->value.bytes.data = (uint8_t *)malloc(asn1_val->value.octet_string.length);
      }
      
      if (varbind->value.bytes.data) {
        memcpy(varbind->value.bytes.data, asn1_val->value.octet_string.data, asn1_val->value.octet_string.length);
      } else {
        return SNMP_PARSE_ERROR_MEMORY;
      }
    } else {
      varbind->value.bytes.data = NULL;
    }
    break;

  case ASN1_TYPE_NULL:
    varbind->value_type = SNMP_TYPE_NULL;
    /* NULL has no value */
    break;

  case ASN1_TYPE_OBJECT_IDENTIFIER:
    varbind->value_type = SNMP_TYPE_OID;
    return parse_oid(asn1_val, &varbind->value.oid, pool);

  /* SNMP application values and v2 exception values use distinct tag classes. */
  default:
    if (asn1_val->tag_class == 1) { /* Application */
      switch (asn1_val->tag_number) {
        case 0: /* IpAddress */
          varbind->value_type = SNMP_TYPE_IPADDRESS;
          varbind->value.bytes.len = asn1_val->value.octet_string.length;
          
          /* Copy the data */
          if (asn1_val->value.octet_string.length > 0 && asn1_val->value.octet_string.data) {
            if (pool) {
              varbind->value.bytes.data = (uint8_t *)pool_alloc(pool, asn1_val->value.octet_string.length);
            } else {
              varbind->value.bytes.data = (uint8_t *)malloc(asn1_val->value.octet_string.length);
            }
            
            if (varbind->value.bytes.data) {
              memcpy(varbind->value.bytes.data, asn1_val->value.octet_string.data, asn1_val->value.octet_string.length);
            } else {
              return SNMP_PARSE_ERROR_MEMORY;
            }
          } else {
            varbind->value.bytes.data = NULL;
          }
          break;
        case 1: /* Counter32 */
          varbind->value_type = SNMP_TYPE_COUNTER32;
          if (asn1_val->value.octet_string.length <= 4) {
            uint32_t val = 0;
            for (size_t i = 0; i < asn1_val->value.octet_string.length; i++) {
              val = (val << 8) | asn1_val->value.octet_string.data[i];
            }
            varbind->value.i32 = (int32_t)val;
          }
          break;
        case 2: /* Gauge32 */
          varbind->value_type = SNMP_TYPE_GAUGE32;
          if (asn1_val->value.octet_string.length <= 4) {
            uint32_t val = 0;
            for (size_t i = 0; i < asn1_val->value.octet_string.length; i++) {
              val = (val << 8) | asn1_val->value.octet_string.data[i];
            }
            varbind->value.i32 = (int32_t)val;
          }
          break;
        case 3: /* TimeTicks */
          varbind->value_type = SNMP_TYPE_TIMETICKS;
          if (asn1_val->value.octet_string.length <= 4) {
            uint32_t val = 0;
            for (size_t i = 0; i < asn1_val->value.octet_string.length; i++) {
              val = (val << 8) | asn1_val->value.octet_string.data[i];
            }
            varbind->value.i32 = (int32_t)val;
          }
          break;
        case 4: /* Opaque */
          varbind->value_type = SNMP_TYPE_OPAQUE;
          varbind->value.bytes.len = asn1_val->value.octet_string.length;
          if (varbind->value.bytes.len > 0u) {
            varbind->value.bytes.data =
                (uint8_t *)snmp_parse_alloc(pool, varbind->value.bytes.len);
            if (!varbind->value.bytes.data) return SNMP_PARSE_ERROR_MEMORY;
            memcpy(varbind->value.bytes.data, asn1_val->value.octet_string.data,
                   varbind->value.bytes.len);
          }
          break;
        case 6: /* Counter64 */
          varbind->value_type = SNMP_TYPE_COUNTER64;
          if (asn1_val->value.octet_string.length <= 8) {
            uint64_t val = 0;
            for (size_t i = 0; i < asn1_val->value.octet_string.length; i++) {
              val = (val << 8) | asn1_val->value.octet_string.data[i];
            }
            varbind->value.i64 = (int64_t)val;
          }
          break;
        default:
          return SNMP_PARSE_ERROR_VALUE_TYPE;
      }
    } else if (asn1_val->tag_class == 2 &&
               asn1_val->value.octet_string.length == 0u) {
      switch (asn1_val->tag_number) {
        case 0:
          varbind->value_type = SNMP_TYPE_NOSUCHOBJECT;
          break;
        case 1:
          varbind->value_type = SNMP_TYPE_NOSUCHINSTANCE;
          break;
        case 2:
          varbind->value_type = SNMP_TYPE_ENDOFMIBVIEW;
          break;
        default:
          return SNMP_PARSE_ERROR_VALUE_TYPE;
      }
    } else {
      return SNMP_PARSE_ERROR_VALUE_TYPE;
    }
    break;
  }

  return SNMP_PARSE_OK;
}

/* Helper: Parse VarBind list */
static int parse_varbinds(const asn1_value_t *asn1_varbinds, snmp_pdu_t *pdu, MemoryPool *pool) {
  if (!asn1_varbinds || asn1_varbinds->type != ASN1_TYPE_SEQUENCE) {
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  pdu->varbind_count = asn1_varbinds->value.sequence.count;

  if (pdu->varbind_count == 0) {
    pdu->varbinds = NULL;
    return SNMP_PARSE_OK;
  }

  /* Allocate varbinds array */
  if (pool) {
    pdu->varbinds = (snmp_varbind_t *)pool_alloc(pool, pdu->varbind_count * sizeof(snmp_varbind_t));
  } else {
    pdu->varbinds = (snmp_varbind_t *)calloc(pdu->varbind_count, sizeof(snmp_varbind_t));
  }

  if (!pdu->varbinds) {
    return SNMP_PARSE_ERROR_MEMORY;
  }

  /* Parse each VarBind */
  for (size_t i = 0; i < pdu->varbind_count; i++) {
    asn1_value_t *varbind_seq = asn1_varbinds->value.sequence.children[i];

    if (varbind_seq->type != ASN1_TYPE_SEQUENCE || varbind_seq->value.sequence.count != 2) {
      return SNMP_PARSE_ERROR_MALFORMED;
    }

    /* child[0] = OID */
    int result = parse_oid(varbind_seq->value.sequence.children[0], &pdu->varbinds[i].oid, pool);
    if (result != SNMP_PARSE_OK) {
      return result;
    }

    /* child[1] = Value */
    result = parse_varbind_value(varbind_seq->value.sequence.children[1], &pdu->varbinds[i], pool);
    if (result != SNMP_PARSE_OK) {
      return result;
    }
  }

  return SNMP_PARSE_OK;
}

/* Helper: Parse PDU */
static int parse_pdu(const asn1_value_t *asn1_pdu, snmp_pdu_t *pdu, MemoryPool *pool) {
  if (!asn1_pdu || (asn1_pdu->type != ASN1_TYPE_SEQUENCE && asn1_pdu->type != TK_CONTEXT_SPECIFIC)) {
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  /* PDU type is the context-specific tag */
  pdu->type = (snmp_pdu_type_t)asn1_pdu->tag;

  /* Validate PDU type */
  if (pdu->type < SNMP_PDU_GET_REQUEST || pdu->type > SNMP_PDU_REPORT) {
    return SNMP_PARSE_ERROR_PDU_TYPE;
  }

  /* PDU has 4 fields (5 for GetBulkRequest) */
  size_t expected_children = (pdu->type == SNMP_PDU_GET_BULK_REQUEST) ? 4 : 4;
  if (asn1_pdu->value.sequence.count < expected_children) {
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  /* child[0] = request-id */
  asn1_value_t *request_id = asn1_pdu->value.sequence.children[0];
  if (request_id->type != ASN1_TYPE_INTEGER) {
    return SNMP_PARSE_ERROR_MALFORMED;
  }
  pdu->request_id = (int32_t)request_id->value.integer;

  if (pdu->type == SNMP_PDU_GET_BULK_REQUEST) {
    /* GetBulkRequest: child[1] = non-repeaters, child[2] = max-repetitions */
    asn1_value_t *non_repeaters = asn1_pdu->value.sequence.children[1];
    asn1_value_t *max_repetitions = asn1_pdu->value.sequence.children[2];

    if (non_repeaters->type != ASN1_TYPE_INTEGER || max_repetitions->type != ASN1_TYPE_INTEGER) {
      return SNMP_PARSE_ERROR_MALFORMED;
    }

    pdu->non_repeaters = (int32_t)non_repeaters->value.integer;
    pdu->max_repetitions = (int32_t)max_repetitions->value.integer;
    pdu->error_status = 0;
    pdu->error_index = 0;

    /* child[3] = variable-bindings */
    return parse_varbinds(asn1_pdu->value.sequence.children[3], pdu, pool);
  } else {
    /* Other PDUs: child[1] = error-status, child[2] = error-index */
    asn1_value_t *error_status = asn1_pdu->value.sequence.children[1];
    asn1_value_t *error_index = asn1_pdu->value.sequence.children[2];

    if (error_status->type != ASN1_TYPE_INTEGER || error_index->type != ASN1_TYPE_INTEGER) {
      return SNMP_PARSE_ERROR_MALFORMED;
    }

    pdu->error_status = (int32_t)error_status->value.integer;
    pdu->error_index = (int32_t)error_index->value.integer;
    pdu->non_repeaters = 0;
    pdu->max_repetitions = 0;

    /* child[3] = variable-bindings */
    return parse_varbinds(asn1_pdu->value.sequence.children[3], pdu, pool);
  }
}

/* Main parser function */
int snmp_parse(const uint8_t *data, size_t len, snmp_message_t *msg, MemoryPool *pool) {
  if (!data || !msg) {
    return SNMP_PARSE_ERROR_INVALID;
  }

  memset(msg, 0, sizeof(*msg));

  /* Parse ASN.1 structure using re2c+lemon parser */
  asn1_value_t *root = NULL;
  int result = scan_binary_asn1(data, len, &root);
  if (result != 0 || !root) {
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  /* SNMP message is a SEQUENCE with 3 elements */
  if (root->type != ASN1_TYPE_SEQUENCE || root->value.sequence.count != 3) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  /* child[0] = version */
  asn1_value_t *version = root->value.sequence.children[0];
  if (version->type != ASN1_TYPE_INTEGER) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  msg->version = (snmp_version_t)version->value.integer;

  /* Validate version */
  if (msg->version != SNMP_VERSION_1 && msg->version != SNMP_VERSION_2C &&
      msg->version != SNMP_VERSION_3) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_VERSION;
  }

  /* child[1] = community (for v1/v2c) */
  asn1_value_t *community = root->value.sequence.children[1];
  if (community->type != ASN1_TYPE_OCTET_STRING) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  /* Copy community string */
  size_t comm_len = community->value.octet_string.length;
  if (pool) {
    msg->community = (char *)pool_alloc(pool, comm_len + 1);
  } else {
    msg->community = (char *)malloc(comm_len + 1);
  }
  
  if (!msg->community) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_MEMORY;
  }
  
  memcpy(msg->community, community->value.octet_string.data, comm_len);
  msg->community[comm_len] = '\0';
  msg->community_len = comm_len;

  /* child[2] = PDU */
  int parse_result = parse_pdu(root->value.sequence.children[2], &msg->pdu, pool);
  if (parse_result != SNMP_PARSE_OK) {
    if (!pool) {
      free(msg->community);
    }
    asn1_free(root);
    return parse_result;
  }

  asn1_free(root);
  return (int)len; /* Return bytes consumed */
}

void snmp_message_free(snmp_message_t *msg) {
  if (!msg)
    return;

  free(msg->community);
  free(msg->usm_params.authoritative_engine_id);
  free(msg->usm_params.user_name);
  free(msg->scoped_pdu.context_engine_id);
  free(msg->scoped_pdu.context_name);

  /* Free varbinds */
  if (msg->pdu.varbinds) {
    for (size_t i = 0; i < msg->pdu.varbind_count; i++) {
      free(msg->pdu.varbinds[i].oid.components);
      if (msg->pdu.varbinds[i].value_type == SNMP_TYPE_OID) {
        free(msg->pdu.varbinds[i].value.oid.components);
      } else if (msg->pdu.varbinds[i].value_type == SNMP_TYPE_OCTET_STRING ||
                 msg->pdu.varbinds[i].value_type == SNMP_TYPE_IPADDRESS ||
                 msg->pdu.varbinds[i].value_type == SNMP_TYPE_OPAQUE) {
        free(msg->pdu.varbinds[i].value.bytes.data);
      }
    }
    free(msg->pdu.varbinds);
  }

  memset(msg, 0, sizeof(*msg));
}

/*
 * Parse and verify SNMPv3 message with USM security
 */
int snmp_parse_v3(const uint8_t *data, size_t len, snmp_message_t *msg, const snmp_v3_user_t *user,
                  MemoryPool *pool) {
  asn1_value_t *scoped_pdu = NULL;
  uint8_t *context_engine_id = NULL;
  char *context_name = NULL;
  if (!data || !msg) {
    return SNMP_PARSE_ERROR_INVALID;
  }

  memset(msg, 0, sizeof(*msg));

  /* Decode top-level SEQUENCE */
  asn1_value_t *root = NULL;
  int result = scan_binary_asn1(data, len, &root);
  if (result != 0 || !root || root->tag != 0x30) {
    if (root) {
      asn1_free(root);
    }
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  /* Must have 4 children: version, msgGlobalData, msgSecurityParameters, msgData */
  if (root->value.sequence.count != 4) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  asn1_value_t **children = root->value.sequence.children;

  /* 1. version (must be 3) */
  if (children[0]->tag != 0x02 || children[0]->value.integer != 3) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_VERSION;
  }
  msg->version = SNMP_VERSION_3;

  /* 2. msgGlobalData */
  if (children[1]->tag != 0x30 || children[1]->value.sequence.count != 4) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }
  asn1_value_t **global_data = children[1]->value.sequence.children;

  msg->v3_header.msg_id = (uint32_t)global_data[0]->value.integer;
  msg->v3_header.msg_max_size = (uint32_t)global_data[1]->value.integer;
  msg->v3_header.msg_flags = global_data[2]->value.octet_string.data[0];
  msg->v3_header.msg_security_model = (uint32_t)global_data[3]->value.integer;

  /* 3. msgSecurityParameters (OCTET STRING containing USM params) */
  if (children[2]->tag != 0x04) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  result = usm_decode_security_params(children[2]->value.octet_string.data, children[2]->value.octet_string.length,
                                      &msg->usm_params, pool);
  if (result != USM_OK) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  /* 4. Verify authentication if enabled */
  int is_authenticated = (msg->v3_header.msg_flags & SNMP_MSG_FLAG_AUTH) != 0;
  int is_encrypted = (msg->v3_header.msg_flags & SNMP_MSG_FLAG_PRIV) != 0;

  if (is_encrypted && !is_authenticated) {
    asn1_free(root);
    return SNMP_PARSE_ERROR_AUTH;
  }
  if (is_authenticated) {
    const uint8_t *auth_field = NULL;
    size_t auth_field_len = 0u;
    uint8_t *message_copy = NULL;
    int auth_result;
    if (!user || !user->user_name || !msg->usm_params.user_name ||
        strcmp(user->user_name, msg->usm_params.user_name) != 0 ||
        (user->auth_protocol != SNMP_AUTH_MD5 &&
         user->auth_protocol != SNMP_AUTH_SHA1) ||
        (user->auth_protocol == SNMP_AUTH_MD5 && user->auth_key_len != 16u) ||
        (user->auth_protocol == SNMP_AUTH_SHA1 && user->auth_key_len != 20u) ||
        len > 65535u ||
        snmp_usm_find_auth_field(data, len, &auth_field, &auth_field_len) != 0 ||
        auth_field_len != sizeof(msg->usm_params.auth_params)) {
      asn1_free(root);
      return SNMP_PARSE_ERROR_AUTH;
    }
    message_copy = (uint8_t *)malloc(len);
    if (!message_copy) {
      asn1_free(root);
      return SNMP_PARSE_ERROR_MEMORY;
    }
    memcpy(message_copy, data, len);
    memset(message_copy + (size_t)(auth_field - data), 0, auth_field_len);
    auth_result = usm_verify_auth(message_copy, len, user->auth_key,
                                  user->auth_key_len, user->auth_protocol,
                                  msg->usm_params.auth_params);
    free(message_copy);
    if (auth_result != USM_OK) {
      asn1_free(root);
      return SNMP_PARSE_ERROR_AUTH;
    }
  }

  /* 5. msgData (either plaintext scopedPDU or encrypted) */
  asn1_value_t *msg_data = children[3];

  if (is_encrypted) {
    /* Encrypted - msgData is OCTET STRING */
    if (msg_data->tag != 0x04) {
      asn1_free(root);
      return SNMP_PARSE_ERROR_MALFORMED;
    }

    if (!user || user->priv_protocol == SNMP_PRIV_NONE) {
      asn1_free(root);
      return SNMP_PARSE_ERROR_MALFORMED; /* Cannot decrypt without keys */
    }

    /* Decrypt */
    uint8_t plaintext[2048];
    size_t plaintext_len = sizeof(plaintext);

    result = usm_decrypt(msg_data->value.octet_string.data, msg_data->value.octet_string.length, user->priv_key,
                         user->priv_key_len, user->priv_protocol, msg->usm_params.engine_boots,
                         msg->usm_params.engine_time, msg->usm_params.priv_params, plaintext,
                         &plaintext_len);

    if (result != USM_OK) {
      asn1_free(root);
      return SNMP_PARSE_ERROR_MALFORMED;
    }

    /* Parse decrypted scopedPDU */
    result = scan_binary_asn1(plaintext, plaintext_len, &scoped_pdu);
    if (result != 0 || !scoped_pdu || scoped_pdu->tag != 0x30) {
      asn1_free(root);
      if (scoped_pdu) {
        asn1_free(scoped_pdu);
      }
      return SNMP_PARSE_ERROR_MALFORMED;
    }

    msg_data = scoped_pdu; /* Replace with decrypted data */
  }

  /* Parse scopedPDU: contextEngineID, contextName, PDU */
  if (msg_data->tag != 0x30 || msg_data->value.sequence.count != 3) {
    asn1_free(root);
    if (scoped_pdu) {
      asn1_free(scoped_pdu);
    }
    return SNMP_PARSE_ERROR_MALFORMED;
  }

  asn1_value_t **scoped = msg_data->value.sequence.children;
  msg->scoped_pdu.context_engine_id_len = scoped[0]->value.octet_string.length;
  if (msg->scoped_pdu.context_engine_id_len > 0) {
    context_engine_id = (uint8_t *)snmp_parse_alloc(pool, msg->scoped_pdu.context_engine_id_len);
    if (!context_engine_id) {
      asn1_free(root);
      if (scoped_pdu) {
        asn1_free(scoped_pdu);
      }
      return SNMP_PARSE_ERROR_MALFORMED;
    }
    memcpy(context_engine_id, scoped[0]->value.octet_string.data, msg->scoped_pdu.context_engine_id_len);
  }
  msg->scoped_pdu.context_engine_id = context_engine_id;
  msg->scoped_pdu.context_name_len = scoped[1]->value.octet_string.length;
  context_name = (char *)snmp_parse_alloc(pool, msg->scoped_pdu.context_name_len + 1);
  if (!context_name) {
    asn1_free(root);
    if (scoped_pdu) {
      asn1_free(scoped_pdu);
    }
    return SNMP_PARSE_ERROR_MALFORMED;
  }
  memcpy(context_name, scoped[1]->value.octet_string.data, msg->scoped_pdu.context_name_len);
  context_name[msg->scoped_pdu.context_name_len] = '\0';
  msg->scoped_pdu.context_name = context_name;

  /* Parse PDU */
  result = parse_pdu(scoped[2], &msg->pdu, pool);
  if (result != SNMP_PARSE_OK) {
    asn1_free(root);
    if (scoped_pdu) {
      asn1_free(scoped_pdu);
    }
    return result;
  }

  /* Copy PDU to scoped_pdu for v3 */
  msg->scoped_pdu.pdu = msg->pdu;

  asn1_free(root);
  if (scoped_pdu) {
    asn1_free(scoped_pdu);
  }

  return (int)len; /* Successfully consumed all bytes */
}
