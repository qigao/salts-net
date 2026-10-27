/** @file snmp_parser.c @brief SNMP message parser. */
#include "snmp_parser.h"
#include "snmp_usm.h"
#include "snmp_usm_wire.h"
#include <salts/asn1_value.h>
#include <stdlib.h>
#include <string.h>

static int tag_is(const salts_asn1_value *v, uint8_t cls, uint8_t cons,
                  uint32_t number) {
  uint8_t actual_cls = 0u, actual_cons = 0u;
  uint32_t actual_number = 0u;
  return v && salts_asn1_value_get_tag(v, &actual_cls, &actual_cons,
                                        &actual_number) == SALTS_ASN1_OK &&
         actual_cls == cls && actual_cons == cons && actual_number == number;
}

static const salts_asn1_value *child(const salts_asn1_value *v, size_t index) {
  return salts_asn1_value_child_at(v, index);
}

static void *parse_alloc(MemoryPool *pool, size_t size) {
  return pool ? pool_alloc(pool, size) : malloc(size);
}

static int copy_content(const salts_asn1_value *v, uint8_t **out,
                        size_t *out_size, MemoryPool *pool) {
  const uint8_t *data = NULL;
  size_t size = 0u;
  if (!out || !out_size ||
      salts_asn1_value_get_content(v, &data, &size) != SALTS_ASN1_OK)
    return SNMP_PARSE_ERROR_MALFORMED;
  *out = NULL;
  *out_size = size;
  if (size != 0u) {
    *out = (uint8_t *)parse_alloc(pool, size);
    if (!*out) return SNMP_PARSE_ERROR_MEMORY;
    memcpy(*out, data, size);
  }
  return SNMP_PARSE_OK;
}

static int parse_oid(const salts_asn1_value *v, snmp_oid_t *oid,
                     MemoryPool *pool) {
  size_t count = 0u;
  if (!oid || salts_asn1_value_get_oid(v, NULL, 0u, &count) != SALTS_ASN1_OK ||
      count > SIZE_MAX / sizeof(uint32_t))
    return SNMP_PARSE_ERROR_MALFORMED;
  oid->components =
      (uint32_t *)parse_alloc(pool, count * sizeof(uint32_t));
  if (!oid->components) return SNMP_PARSE_ERROR_MEMORY;
  if (salts_asn1_value_get_oid(v, oid->components, count, &count) !=
      SALTS_ASN1_OK)
    return SNMP_PARSE_ERROR_MALFORMED;
  oid->count = count;
  return SNMP_PARSE_OK;
}

static int parse_unsigned(const salts_asn1_value *v, size_t max_size,
                          uint64_t *out) {
  const uint8_t *data = NULL;
  size_t size = 0u;
  uint64_t result = 0u;
  if (!out || salts_asn1_value_get_content(v, &data, &size) != SALTS_ASN1_OK ||
      size == 0u || size > max_size || (data[0] & 0x80u) != 0u)
    return SNMP_PARSE_ERROR_MALFORMED;
  for (size_t i = 0u; i < size; ++i) result = (result << 8u) | data[i];
  *out = result;
  return SNMP_PARSE_OK;
}

static int parse_varbind_value(const salts_asn1_value *v,
                               snmp_varbind_t *varbind, MemoryPool *pool) {
  uint8_t cls = 0u, cons = 0u;
  uint32_t number = 0u;
  int64_t integer = 0;
  uint64_t unsigned_value = 0u;
  const uint8_t *content = NULL;
  size_t content_size = 0u;
  if (!v || !varbind ||
      salts_asn1_value_get_tag(v, &cls, &cons, &number) != SALTS_ASN1_OK)
    return SNMP_PARSE_ERROR_INVALID;
  if (cls == 0u && !cons) {
    switch (number) {
    case 2u:
      if (salts_asn1_value_get_integer_i64(v, &integer) != SALTS_ASN1_OK ||
          integer < INT32_MIN || integer > INT32_MAX)
        return SNMP_PARSE_ERROR_MALFORMED;
      varbind->value_type = SNMP_TYPE_INTEGER;
      varbind->value.i32 = (int32_t)integer;
      return SNMP_PARSE_OK;
    case 4u:
      varbind->value_type = SNMP_TYPE_OCTET_STRING;
      return copy_content(v, &varbind->value.bytes.data,
                          &varbind->value.bytes.len, pool);
    case 5u:
      if (salts_asn1_value_get_content(v, &content, &content_size) !=
              SALTS_ASN1_OK ||
          content_size != 0u)
        return SNMP_PARSE_ERROR_MALFORMED;
      varbind->value_type = SNMP_TYPE_NULL;
      return SNMP_PARSE_OK;
    case 6u:
      varbind->value_type = SNMP_TYPE_OID;
      return parse_oid(v, &varbind->value.oid, pool);
    default:
      return SNMP_PARSE_ERROR_VALUE_TYPE;
    }
  }
  if (cls == 1u && !cons) {
    switch (number) {
    case 0u:
      varbind->value_type = SNMP_TYPE_IPADDRESS;
      return copy_content(v, &varbind->value.bytes.data,
                          &varbind->value.bytes.len, pool);
    case 1u:
    case 2u:
    case 3u:
      if (parse_unsigned(v, 5u, &unsigned_value) != SNMP_PARSE_OK ||
          unsigned_value > UINT32_MAX)
        return SNMP_PARSE_ERROR_MALFORMED;
      varbind->value_type = number == 1u ? SNMP_TYPE_COUNTER32
                            : number == 2u ? SNMP_TYPE_GAUGE32
                                           : SNMP_TYPE_TIMETICKS;
      varbind->value.i32 = (int32_t)(uint32_t)unsigned_value;
      return SNMP_PARSE_OK;
    case 4u:
      varbind->value_type = SNMP_TYPE_OPAQUE;
      return copy_content(v, &varbind->value.bytes.data,
                          &varbind->value.bytes.len, pool);
    case 6u:
      if (parse_unsigned(v, 9u, &unsigned_value) != SNMP_PARSE_OK)
        return SNMP_PARSE_ERROR_MALFORMED;
      varbind->value_type = SNMP_TYPE_COUNTER64;
      varbind->value.i64 = (int64_t)unsigned_value;
      return SNMP_PARSE_OK;
    default:
      return SNMP_PARSE_ERROR_VALUE_TYPE;
    }
  }
  if (cls == 2u && !cons && number <= 2u &&
      salts_asn1_value_get_content(v, &content, &content_size) ==
          SALTS_ASN1_OK &&
      content_size == 0u) {
    varbind->value_type = number == 0u ? SNMP_TYPE_NOSUCHOBJECT
                          : number == 1u ? SNMP_TYPE_NOSUCHINSTANCE
                                         : SNMP_TYPE_ENDOFMIBVIEW;
    return SNMP_PARSE_OK;
  }
  return SNMP_PARSE_ERROR_VALUE_TYPE;
}

static int parse_varbinds(const salts_asn1_value *list, snmp_pdu_t *pdu,
                          MemoryPool *pool) {
  if (!tag_is(list, 0u, 1u, 16u)) return SNMP_PARSE_ERROR_MALFORMED;
  pdu->varbind_count = salts_asn1_value_child_count(list);
  if (pdu->varbind_count == 0u) return SNMP_PARSE_OK;
  if (pdu->varbind_count > SIZE_MAX / sizeof(*pdu->varbinds))
    return SNMP_PARSE_ERROR_MEMORY;
  pdu->varbinds = pool
      ? (snmp_varbind_t *)pool_alloc(
            pool, pdu->varbind_count * sizeof(*pdu->varbinds))
      : (snmp_varbind_t *)calloc(pdu->varbind_count, sizeof(*pdu->varbinds));
  if (!pdu->varbinds) return SNMP_PARSE_ERROR_MEMORY;
  if (pool)
    memset(pdu->varbinds, 0,
           pdu->varbind_count * sizeof(*pdu->varbinds));
  for (size_t i = 0u; i < pdu->varbind_count; ++i) {
    const salts_asn1_value *varbind = child(list, i);
    int result;
    if (!tag_is(varbind, 0u, 1u, 16u) ||
        salts_asn1_value_child_count(varbind) != 2u)
      return SNMP_PARSE_ERROR_MALFORMED;
    result = parse_oid(child(varbind, 0u), &pdu->varbinds[i].oid, pool);
    if (result != SNMP_PARSE_OK) return result;
    result = parse_varbind_value(child(varbind, 1u), &pdu->varbinds[i], pool);
    if (result != SNMP_PARSE_OK) return result;
  }
  return SNMP_PARSE_OK;
}

static int parse_pdu(const salts_asn1_value *v, snmp_pdu_t *pdu,
                     MemoryPool *pool) {
  uint8_t cls = 0u, cons = 0u;
  uint32_t number = 0u;
  int64_t fields[3];
  if (!v || !pdu ||
      salts_asn1_value_get_tag(v, &cls, &cons, &number) != SALTS_ASN1_OK ||
      cls != 2u || !cons || number > 8u)
    return SNMP_PARSE_ERROR_MALFORMED;
  pdu->type = (snmp_pdu_type_t)(0xa0u | number);
  if (pdu->type < SNMP_PDU_GET_REQUEST || pdu->type > SNMP_PDU_REPORT)
    return SNMP_PARSE_ERROR_PDU_TYPE;
  if (salts_asn1_value_child_count(v) != 4u)
    return SNMP_PARSE_ERROR_MALFORMED;
  for (size_t i = 0u; i < 3u; ++i) {
    if (salts_asn1_value_get_integer_i64(child(v, i), &fields[i]) !=
            SALTS_ASN1_OK ||
        fields[i] < INT32_MIN || fields[i] > INT32_MAX)
      return SNMP_PARSE_ERROR_MALFORMED;
  }
  pdu->request_id = (int32_t)fields[0];
  if (pdu->type == SNMP_PDU_GET_BULK_REQUEST) {
    pdu->non_repeaters = (int32_t)fields[1];
    pdu->max_repetitions = (int32_t)fields[2];
  } else {
    pdu->error_status = (int32_t)fields[1];
    pdu->error_index = (int32_t)fields[2];
  }
  return parse_varbinds(child(v, 3u), pdu, pool);
}

int snmp_parse(const uint8_t *data, size_t len, snmp_message_t *msg,
               MemoryPool *pool) {
  salts_asn1_value *root = NULL;
  const salts_asn1_value *community;
  const uint8_t *community_data = NULL;
  size_t community_size = 0u;
  int64_t version = 0;
  int result;
  if (!data || !msg) return SNMP_PARSE_ERROR_INVALID;
  memset(msg, 0, sizeof(*msg));
  if (salts_asn1_value_decode(data, len, SALTS_ASN1_BER, &root) !=
          SALTS_ASN1_OK ||
      !tag_is(root, 0u, 1u, 16u) ||
      salts_asn1_value_child_count(root) != 3u) {
    salts_asn1_value_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }
  if (salts_asn1_value_get_integer_i64(child(root, 0u), &version) !=
          SALTS_ASN1_OK ||
      (version != SNMP_VERSION_1 && version != SNMP_VERSION_2C)) {
    salts_asn1_value_free(root);
    return SNMP_PARSE_ERROR_VERSION;
  }
  msg->version = (snmp_version_t)version;
  community = child(root, 1u);
  if (!tag_is(community, 0u, 0u, 4u) ||
      salts_asn1_value_get_content(community, &community_data,
                                   &community_size) != SALTS_ASN1_OK) {
    salts_asn1_value_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }
  msg->community = (char *)parse_alloc(pool, community_size + 1u);
  if (!msg->community) {
    salts_asn1_value_free(root);
    return SNMP_PARSE_ERROR_MEMORY;
  }
  memcpy(msg->community, community_data, community_size);
  msg->community[community_size] = '\0';
  msg->community_len = community_size;
  result = parse_pdu(child(root, 2u), &msg->pdu, pool);
  salts_asn1_value_free(root);
  if (result != SNMP_PARSE_OK && !pool) free(msg->community);
  return result == SNMP_PARSE_OK ? (int)len : result;
}

void snmp_message_free(snmp_message_t *msg) {
  if (!msg) return;
  free(msg->community);
  free(msg->usm_params.authoritative_engine_id);
  free(msg->usm_params.user_name);
  free(msg->scoped_pdu.context_engine_id);
  free(msg->scoped_pdu.context_name);
  if (msg->pdu.varbinds) {
    for (size_t i = 0u; i < msg->pdu.varbind_count; ++i) {
      free(msg->pdu.varbinds[i].oid.components);
      if (msg->pdu.varbinds[i].value_type == SNMP_TYPE_OID)
        free(msg->pdu.varbinds[i].value.oid.components);
      else if (msg->pdu.varbinds[i].value_type == SNMP_TYPE_OCTET_STRING ||
               msg->pdu.varbinds[i].value_type == SNMP_TYPE_IPADDRESS ||
               msg->pdu.varbinds[i].value_type == SNMP_TYPE_OPAQUE)
        free(msg->pdu.varbinds[i].value.bytes.data);
    }
    free(msg->pdu.varbinds);
  }
  memset(msg, 0, sizeof(*msg));
}

int snmp_parse_v3(const uint8_t *data, size_t len, snmp_message_t *msg,
                  const snmp_v3_user_t *user, MemoryPool *pool) {
  salts_asn1_value *root = NULL, *decrypted = NULL;
  const salts_asn1_value *global, *security, *msg_data;
  const uint8_t *bytes = NULL;
  size_t byte_count = 0u;
  int64_t integer = 0;
  int result, authenticated, encrypted;
  if (!data || !msg) return SNMP_PARSE_ERROR_INVALID;
  memset(msg, 0, sizeof(*msg));
  if (salts_asn1_value_decode(data, len, SALTS_ASN1_BER, &root) !=
          SALTS_ASN1_OK ||
      !tag_is(root, 0u, 1u, 16u) ||
      salts_asn1_value_child_count(root) != 4u) {
    salts_asn1_value_free(root);
    return SNMP_PARSE_ERROR_MALFORMED;
  }
  if (salts_asn1_value_get_integer_i64(child(root, 0u), &integer) !=
          SALTS_ASN1_OK ||
      integer != 3) {
    salts_asn1_value_free(root);
    return SNMP_PARSE_ERROR_VERSION;
  }
  msg->version = SNMP_VERSION_3;
  global = child(root, 1u);
  if (!tag_is(global, 0u, 1u, 16u) ||
      salts_asn1_value_child_count(global) != 4u)
    goto malformed;
  if (salts_asn1_value_get_integer_i64(child(global, 0u), &integer) !=
          SALTS_ASN1_OK || integer < 0 || integer > UINT32_MAX)
    goto malformed;
  msg->v3_header.msg_id = (uint32_t)integer;
  if (salts_asn1_value_get_integer_i64(child(global, 1u), &integer) !=
          SALTS_ASN1_OK || integer < 0 || integer > UINT32_MAX)
    goto malformed;
  msg->v3_header.msg_max_size = (uint32_t)integer;
  if (!tag_is(child(global, 2u), 0u, 0u, 4u) ||
      salts_asn1_value_get_content(child(global, 2u), &bytes, &byte_count) !=
          SALTS_ASN1_OK || byte_count != 1u)
    goto malformed;
  msg->v3_header.msg_flags = bytes[0];
  if (salts_asn1_value_get_integer_i64(child(global, 3u), &integer) !=
          SALTS_ASN1_OK || integer < 0 || integer > UINT32_MAX)
    goto malformed;
  msg->v3_header.msg_security_model = (uint32_t)integer;
  security = child(root, 2u);
  if (!tag_is(security, 0u, 0u, 4u) ||
      salts_asn1_value_get_content(security, &bytes, &byte_count) !=
          SALTS_ASN1_OK ||
      usm_decode_security_params(bytes, byte_count, &msg->usm_params, pool) !=
          USM_OK)
    goto malformed;
  authenticated = (msg->v3_header.msg_flags & SNMP_MSG_FLAG_AUTH) != 0;
  encrypted = (msg->v3_header.msg_flags & SNMP_MSG_FLAG_PRIV) != 0;
  if (encrypted && !authenticated) {
    salts_asn1_value_free(root);
    return SNMP_PARSE_ERROR_AUTH;
  }
  if (authenticated) {
    const uint8_t *auth_field = NULL;
    size_t auth_field_len = 0u;
    uint8_t *message_copy;
    if (!user || !user->user_name || !msg->usm_params.user_name ||
        strcmp(user->user_name, msg->usm_params.user_name) != 0 ||
        (user->auth_protocol != SNMP_AUTH_MD5 &&
         user->auth_protocol != SNMP_AUTH_SHA1) ||
        (user->auth_protocol == SNMP_AUTH_MD5 && user->auth_key_len != 16u) ||
        (user->auth_protocol == SNMP_AUTH_SHA1 && user->auth_key_len != 20u) ||
        len > 65535u ||
        snmp_usm_find_auth_field(data, len, &auth_field, &auth_field_len) != 0 ||
        auth_field_len != sizeof(msg->usm_params.auth_params)) {
      salts_asn1_value_free(root);
      return SNMP_PARSE_ERROR_AUTH;
    }
    message_copy = (uint8_t *)malloc(len);
    if (!message_copy) {
      salts_asn1_value_free(root);
      return SNMP_PARSE_ERROR_MEMORY;
    }
    memcpy(message_copy, data, len);
    memset(message_copy + (size_t)(auth_field - data), 0, auth_field_len);
    result = usm_verify_auth(message_copy, len, user->auth_key,
                             user->auth_key_len, user->auth_protocol,
                             msg->usm_params.auth_params);
    free(message_copy);
    if (result != USM_OK) {
      salts_asn1_value_free(root);
      return SNMP_PARSE_ERROR_AUTH;
    }
  }
  msg_data = child(root, 3u);
  if (encrypted) {
    uint8_t plaintext[2048];
    size_t plaintext_len = sizeof(plaintext);
    if (!user || user->priv_protocol == SNMP_PRIV_NONE ||
        !tag_is(msg_data, 0u, 0u, 4u) ||
        salts_asn1_value_get_content(msg_data, &bytes, &byte_count) !=
            SALTS_ASN1_OK ||
        usm_decrypt(bytes, byte_count, user->priv_key, user->priv_key_len,
                    user->priv_protocol, msg->usm_params.engine_boots,
                    msg->usm_params.engine_time, msg->usm_params.priv_params,
                    plaintext, &plaintext_len) != USM_OK ||
        salts_asn1_value_decode(plaintext, plaintext_len, SALTS_ASN1_BER,
                                &decrypted) != SALTS_ASN1_OK)
      goto malformed;
    msg_data = decrypted;
  }
  if (!tag_is(msg_data, 0u, 1u, 16u) ||
      salts_asn1_value_child_count(msg_data) != 3u ||
      !tag_is(child(msg_data, 0u), 0u, 0u, 4u) ||
      copy_content(child(msg_data, 0u), &msg->scoped_pdu.context_engine_id,
                   &msg->scoped_pdu.context_engine_id_len, pool) !=
          SNMP_PARSE_OK ||
      !tag_is(child(msg_data, 1u), 0u, 0u, 4u) ||
      salts_asn1_value_get_content(child(msg_data, 1u), &bytes,
                                   &byte_count) != SALTS_ASN1_OK)
    goto malformed;
  msg->scoped_pdu.context_name =
      (char *)parse_alloc(pool, byte_count + 1u);
  if (!msg->scoped_pdu.context_name) {
    salts_asn1_value_free(decrypted);
    salts_asn1_value_free(root);
    return SNMP_PARSE_ERROR_MEMORY;
  }
  memcpy(msg->scoped_pdu.context_name, bytes, byte_count);
  msg->scoped_pdu.context_name[byte_count] = '\0';
  msg->scoped_pdu.context_name_len = byte_count;
  result = parse_pdu(child(msg_data, 2u), &msg->pdu, pool);
  if (result != SNMP_PARSE_OK) {
    salts_asn1_value_free(decrypted);
    salts_asn1_value_free(root);
    return result;
  }
  msg->scoped_pdu.pdu = msg->pdu;
  salts_asn1_value_free(decrypted);
  salts_asn1_value_free(root);
  return (int)len;
malformed:
  salts_asn1_value_free(decrypted);
  salts_asn1_value_free(root);
  return SNMP_PARSE_ERROR_MALFORMED;
}
