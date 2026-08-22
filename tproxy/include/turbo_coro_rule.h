/**
 * @file coro_rule.h
 * @brief Rule-based routing engine for TProxy (inspired by V2Ray/Clash).
 */

#ifndef coro_RULE_H
#define coro_RULE_H


#include "turbo_tproxy_api.h"
#include "platform.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TURBO_RULE_DOMAIN,          /**< Exact domain match (e.g. "google.com") */
    TURBO_RULE_DOMAIN_SUFFIX,   /**< Domain suffix match (e.g. ".cn") */
    TURBO_RULE_DOMAIN_KEYWORD,  /**< Domain keyword match (e.g. "google") */
    TURBO_RULE_IP_CIDR,        /**< IP CIDR match (e.g. "192.168.1.0/24") */
    TURBO_RULE_GEOIP,          /**< GeoIP country code match (e.g. "CN") */
    TURBO_RULE_MATCH,           /**< Default match-all rule */
} turbo_rule_type_t;

typedef enum {
    TURBO_GROUP_SELECT,         /**< Manual selection (default to first) */
    TURBO_GROUP_URL_TEST,       /**< Select member with lowest latency */
    TURBO_GROUP_FALLBACK,       /**< Use members in order of availability */
    TURBO_GROUP_LOAD_BALANCE,   /**< Round-robin or random */
} turbo_group_type_t;

typedef enum {
    TURBO_RULE_ACTION_DIRECT,   /**< Direct connection */
    TURBO_RULE_ACTION_PROXY,    /**< Use a specific backend URL or Group name */
    TURBO_RULE_ACTION_REJECT,   /**< Drop connection */
} turbo_rule_action_type_t;

typedef struct {
    turbo_rule_type_t type;
    char *payload;              /**< Domain name, CIDR, or Country Code */
    turbo_rule_action_type_t action;
    char *proxy_url;            /**< Optional: Backend URL if action is TURBO_RULE_ACTION_PROXY */
} turbo_rule_t;

typedef struct coro_rule_engine_s coro_rule_engine_t;

/**
 * @brief Create a new rule engine.
 */
TURBONET_TPROXY_C_API coro_rule_engine_t* coro_rule_engine_create(void);

/**
 * @brief Destroy a rule engine and all its rules.
 */
TURBONET_TPROXY_C_API void coro_rule_engine_destroy(coro_rule_engine_t *engine);

/**
 * @brief Add a rule to the engine (FIFO priority).
 * @param type Rule type (Domain, IP, etc.)
 * @param payload The match criteria (e.g., "google.com")
 * @param action Direct, Proxy, or Reject
 * @param proxy_url URL to use if action is Proxy
 * @return 0 on success
 */
TURBONET_TPROXY_C_API int coro_rule_add(coro_rule_engine_t *engine,
                                 turbo_rule_type_t type, 
                                 const char *payload, 
                                 turbo_rule_action_type_t action, 
                                 const char *proxy_url);

/**
 * @brief Evaluates a target against the rules.
 * @param engine The rule engine.
 * @param host Target hostname or IP string.
 * @param port Target port.
 * @param[out] out_proxy_url Pointer to a string that will receive the proxy URL if matched.
 * @return The action to take.
 */
TURBONET_TPROXY_C_API turbo_rule_action_type_t coro_rule_evaluate(coro_rule_engine_t *engine,
                                                           const char *host, 
                                                           int port, 
                                                           const char **out_proxy_url);

/**
 * @brief Parse a rule string (Clash/V2Ray format).
 * Format: "TYPE,PAYLOAD,ACTION[,PROXY_URL]"
 * Example: "DOMAIN-SUFFIX,google.com,Proxy,wss://remote:443"
 */
TURBONET_TPROXY_C_API int coro_rule_parse_and_add(coro_rule_engine_t *engine, const char *line);

/* ============================================================================ 
 * Proxy Groups
 * ========================================================================= */

/**
 * @brief Add a proxy group to the engine.
 * @param name Name of the group (used as target in rules).
 * @param type Group type (Select, Latency, etc.).
 * @return 0 on success.
 */
TURBONET_TPROXY_C_API int coro_rule_group_add(coro_rule_engine_t *engine,
                                       const char *name, 
                                       turbo_group_type_t type);

/**
 * @brief Add a member (proxy URL or another group name) to a group.
 */
TURBONET_TPROXY_C_API int coro_rule_group_add_member(coro_rule_engine_t *engine,
                                              const char *group_name, 
                                              const char *member);

/**
 * @brief Manually set the selected member for a SELECT group.
 */
TURBONET_TPROXY_C_API int coro_rule_group_select(coro_rule_engine_t *engine,
                                          const char *group_name, 
                                          const char *member);

/**
 * @brief Update a member's health/latency.
 */
TURBONET_TPROXY_C_API int coro_rule_group_update_member(coro_rule_engine_t *engine,
                                                const char *group_name,
                                                const char *member,
                                                bool alive,
                                                uint64_t latency_ms);

/**
 * @brief Callback for group health testing.
 */
typedef void (*turbo_group_health_cb)(const char *url, void *user_data);

/**
 * @brief Set callback for periodic health checks.
 */
TURBONET_TPROXY_C_API void coro_rule_engine_set_health_cb(coro_rule_engine_t *engine,
                                                  turbo_group_health_cb cb,
                                                  void *user_data);

/**
 * @brief Manually trigger health checks for all members in all groups.
 */
TURBONET_TPROXY_C_API void coro_rule_engine_trigger_health_checks(coro_rule_engine_t *engine);

/**
 * @brief Update health/latency for any member matching the given URL across all groups.
 */
TURBONET_TPROXY_C_API void coro_rule_update_health(coro_rule_engine_t *engine,
                                            const char *url_or_name,
                                            bool alive,
                                            uint64_t latency_ms);

/* ============================================================================ 
 * GeoIP & DNS Sniffing
 * ========================================================================= */

/**
 * @brief Load a GeoIP database (CSV or simplified binary format).
 */
TURBONET_TPROXY_C_API int coro_rule_geoip_load(coro_rule_engine_t *engine, const char *path);

/**
 * @brief Records a DNS mapping for sniffing (e.g., from intercepted DNS traffic).
 */
TURBONET_TPROXY_C_API void coro_rule_dns_record(coro_rule_engine_t *engine,
                                         const char *ip, 
                                         const char *domain);

#ifdef __cplusplus
}
#endif

#endif // coro_RULE_H
