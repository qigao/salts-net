#include <salts_snmp.h>

#include <cstring>

static_assert(SNMP_VERSION_1 == 0);
static_assert(SNMP_VERSION_2C == 1);
static_assert(SNMP_VERSION_3 == 3);

int main() {
    snmp_version_t parsed = SNMP_VERSION_1;
    auto *const build_next = &snmp_build_v3_get_next_request;
    auto *const build_set = &snmp_build_v3_set_request;
    return snmp_version_t_from_string("v3", &parsed) &&
                   parsed == SNMP_VERSION_3 &&
                   build_next != nullptr && build_set != nullptr &&
                   std::strcmp(snmp_security_level_t_to_string(
                                   SNMP_SEC_LEVEL_AUTH_PRIV),
                               "auth_priv") == 0
               ? 0
               : 1;
}
