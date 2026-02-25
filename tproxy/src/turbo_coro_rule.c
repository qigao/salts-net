#include "turbo_coro_rule.h"
#include "turbo_str.h"
#include "tlog.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <uv.h>

typedef struct turbo_rule_entry_s {
    turbo_rule_t rule;
    struct turbo_rule_entry_s *next;
} turbo_rule_entry_t;

typedef struct turbo_group_member_s {
    char *name_or_url;
    uint64_t last_latency_ms;
    bool is_alive;
    struct turbo_group_member_s *next;
} turbo_group_member_t;

typedef struct turbo_group_s {
    char *name;
    turbo_group_type_t type;
    turbo_group_member_t *members;
    char *selected_member;      /**< For SELECT type */
    struct turbo_group_s *next;
} turbo_group_t;

typedef struct {
    uint8_t start[16];
    uint8_t end[16];
    bool is_ipv6;
    char country[4];
} geoip_range_t;

typedef struct turbo_dns_mapping_s {
    char ip[64];
    char domain[256];
    struct turbo_dns_mapping_s *next;
} turbo_dns_mapping_t;

struct turbo_coro_rule_engine_s {
    turbo_rule_entry_t *head;
    turbo_rule_entry_t *tail;
    
    turbo_group_t *groups;
    
    geoip_range_t *geoip_db;
    size_t geoip_count;
    
    turbo_dns_mapping_t *dns_cache;

    turbo_group_health_cb health_cb;
    void *health_user_data;
};

turbo_coro_rule_engine_t* turbo_coro_rule_engine_create(void) {
    turbo_coro_rule_engine_t *engine = (turbo_coro_rule_engine_t*)calloc(1, sizeof(turbo_coro_rule_engine_t));
    return engine;
}

void turbo_coro_rule_engine_destroy(turbo_coro_rule_engine_t *engine) {
    if (!engine) return;
    
    // Cleanup rules
    turbo_rule_entry_t *curr = engine->head;
    while (curr) {
        turbo_rule_entry_t *next = curr->next;
        free(curr->rule.payload);
        if (curr->rule.proxy_url) free(curr->rule.proxy_url);
        free(curr);
        curr = next;
    }
    
    // Cleanup groups
    turbo_group_t *g = engine->groups;
    while (g) {
        turbo_group_t *gn = g->next;
        turbo_group_member_t *m = g->members;
        while (m) {
            turbo_group_member_t *mn = m->next;
            free(m->name_or_url);
            free(m);
            m = mn;
        }
        free(g->name);
        if (g->selected_member) free(g->selected_member);
        free(g);
        g = gn;
    }
    
    // Cleanup DNS
    turbo_dns_mapping_t *dm = engine->dns_cache;
    while (dm) {
        turbo_dns_mapping_t *dn = dm->next;
        free(dm);
        dm = dn;
    }
    
    if (engine->geoip_db) free(engine->geoip_db);
    
    free(engine);
}

int turbo_coro_rule_add(turbo_coro_rule_engine_t *engine, 
                                 turbo_rule_type_t type, 
                                 const char *payload, 
                                 turbo_rule_action_type_t action, 
                                 const char *proxy_url) {
    if (!engine) return -1;
    
    turbo_rule_entry_t *entry = (turbo_rule_entry_t*)calloc(1, sizeof(turbo_rule_entry_t));
    entry->rule.type = type;
    entry->rule.payload = payload ? strdup(payload) : NULL;
    entry->rule.action = action;
    entry->rule.proxy_url = proxy_url ? strdup(proxy_url) : NULL;
    
    if (engine->tail) {
        engine->tail->next = entry;
        engine->tail = entry;
    } else {
        engine->head = engine->tail = entry;
    }
    
    return 0;
}

static bool match_domain(const char *host, const char *payload, turbo_rule_type_t type) {
    if (!host || !payload) return false;
    
    switch (type) {
        case TURBO_RULE_DOMAIN:
            return strcmp(host, payload) == 0;
        case TURBO_RULE_DOMAIN_SUFFIX: {
            size_t h_len = strlen(host);
            size_t p_len = strlen(payload);
            if (h_len < p_len) return false;
            // payload ".cn" matches "baidu.cn" or "cn"
            if (payload[0] == '.') {
                return strcmp(host + h_len - p_len, payload) == 0;
            } else {
                // payload "baidu.com" matches "www.baidu.com" or "baidu.com"
                if (h_len == p_len) return strcmp(host, payload) == 0;
                if (h_len > p_len && host[h_len - p_len - 1] == '.') {
                    return strcmp(host + h_len - p_len, payload) == 0;
                }
                return false;
            }
        }
        case TURBO_RULE_DOMAIN_KEYWORD:
            return strstr(host, payload) != NULL;
        default:
            return false;
    }
}

static bool match_cidr(const char *ip_str, const char *cidr) {
    if (!ip_str || !cidr) return false;
    
    char cidr_ip[64] = {0};
    int mask_bits = 32;
    const char *slash = strchr(cidr, '/');
    if (slash) {
        size_t len = slash - cidr;
        if (len >= sizeof(cidr_ip)) return false;
        strncpy(cidr_ip, cidr, len);
        mask_bits = atoi(slash + 1);
    } else {
        strncpy(cidr_ip, cidr, sizeof(cidr_ip)-1);
    }
    
    struct in_addr addr, network;
    if (uv_inet_pton(AF_INET, ip_str, &addr) != 0) return false;
    if (uv_inet_pton(AF_INET, cidr_ip, &network) != 0) return false;
    
    uint32_t a = ntohl(addr.s_addr);
    uint32_t n = ntohl(network.s_addr);
    
    uint32_t mask = (mask_bits == 0) ? 0 : (~0U << (32 - mask_bits));
    return (a & mask) == (n & mask);
}

static const char* match_geoip(turbo_coro_rule_engine_t *engine, const char *ip_str) {
    if (!engine || !engine->geoip_db || !ip_str) return NULL;
    
    struct in_addr addr4;
    struct in6_addr addr6;
    bool is_v6 = false;
    uint8_t target[16] = {0};

    if (uv_inet_pton(AF_INET, ip_str, &addr4) == 0) {
        memcpy(target, &addr4.s_addr, 4);
    } else if (uv_inet_pton(AF_INET6, ip_str, &addr6) == 0) {
        memcpy(target, addr6.s6_addr, 16);
        is_v6 = true;
    } else {
        return NULL;
    }
    
    // Binary search
    size_t low = 0;
    size_t high = engine->geoip_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        geoip_range_t *r = &engine->geoip_db[mid];
        
        if (r->is_ipv6 != is_v6) {
            if (is_v6) low = mid + 1; // v6 comes after v4 in sorted list
            else high = mid;
            continue;
        }

        int cmp_start = memcmp(target, r->start, is_v6 ? 16 : 4);
        int cmp_end = memcmp(target, r->end, is_v6 ? 16 : 4);

        if (cmp_start >= 0 && cmp_end <= 0) {
            return r->country;
        }
        if (cmp_start < 0) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return NULL;
}

static const char* lookup_dns(turbo_coro_rule_engine_t *engine, const char *ip) {
    turbo_dns_mapping_t *m = engine->dns_cache;
    while (m) {
        if (strcmp(m->ip, ip) == 0) return m->domain;
        m = m->next;
    }
    return NULL;
}

static const char* resolve_group(turbo_coro_rule_engine_t *engine, const char *target, int depth) {
    if (depth > 10) return NULL; // Prevent infinite loops
    
    turbo_group_t *g = engine->groups;
    while (g) {
        if (strcmp(g->name, target) == 0) {
            const char *member_target = NULL;
            
            switch (g->type) {
                case TURBO_GROUP_SELECT:
                    member_target = g->selected_member ? g->selected_member : (g->members ? g->members->name_or_url : NULL);
                    break;
                case TURBO_GROUP_URL_TEST: {
                    // Pick member with lowest latency
                    turbo_group_member_t *m = g->members;
                    turbo_group_member_t *best = NULL;
                    while (m) {
                        if (m->is_alive) {
                            if (!best || m->last_latency_ms < best->last_latency_ms) best = m;
                        }
                        m = m->next;
                    }
                    if (best) member_target = best->name_or_url;
                    break;
                }
                case TURBO_GROUP_FALLBACK: {
                    // Pick first alive member
                    turbo_group_member_t *m = g->members;
                    while (m) {
                        if (m->is_alive) {
                            member_target = m->name_or_url;
                            break;
                        }
                        m = m->next;
                    }
                    break;
                }
                default:
                    if (g->members) member_target = g->members->name_or_url;
                    break;
            }
            
            if (!member_target) return NULL;
            if (strcmp(member_target, "Direct") == 0 || strcmp(member_target, "DIRECT") == 0) return NULL;
            if (strcmp(member_target, "Reject") == 0 || strcmp(member_target, "REJECT") == 0) return (const char*)-1;
            
            // Is member another group or a URL?
            const char *nested = resolve_group(engine, member_target, depth + 1);
            return nested ? nested : member_target;
        }
        g = g->next;
    }
    return NULL;
}

turbo_rule_action_type_t turbo_coro_rule_evaluate(turbo_coro_rule_engine_t *engine, 
                                                           const char *host, 
                                                           int port, 
                                                           const char **out_proxy_url) {
    (void)port;
    if (!engine || !host) return TURBO_RULE_ACTION_DIRECT;
    
    // DNS Sniffing: If host is an IP, try to find original domain
    const char *sniffed_domain = lookup_dns(engine, host);
    const char *eval_host = sniffed_domain ? sniffed_domain : host;
    if (sniffed_domain) TLOG_INFO("[Rule] Sniffed domain {} for IP {}", sniffed_domain, host);

    turbo_rule_entry_t *curr = engine->head;
    while (curr) {
        bool matched = false;
        switch (curr->rule.type) {
            case TURBO_RULE_DOMAIN:
            case TURBO_RULE_DOMAIN_SUFFIX:
            case TURBO_RULE_DOMAIN_KEYWORD:
                matched = match_domain(eval_host, curr->rule.payload, curr->rule.type);
                break;
            case TURBO_RULE_IP_CIDR:
                matched = match_cidr(host, curr->rule.payload);
                break;
            case TURBO_RULE_GEOIP: {
                const char *country = match_geoip(engine, host);
                if (country && strcmp(country, curr->rule.payload) == 0) matched = true;
                break;
            }
            case TURBO_RULE_MATCH:
                matched = true;
                break;
            default:
                break;
        }
        
        if (matched) {
            const char *target = curr->rule.proxy_url;
            // Resolve Proxy Groups
            const char *final_backend = resolve_group(engine, target, 0);
            if (out_proxy_url) *out_proxy_url = final_backend ? final_backend : target;
            return curr->rule.action;
        }
        curr = curr->next;
    }
    
    return TURBO_RULE_ACTION_DIRECT;
}

int turbo_coro_rule_parse_and_add(turbo_coro_rule_engine_t *engine, const char *line) {
    if (!engine || !line) return -1;
    
    // Simple split-by-comma parser
    char *copy = strdup(line);
    char *token = copy;
    char *parts[4] = {NULL};
    int count = 0;
    
    char *p = copy;
    while (*p && count < 4) {
        parts[count++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
        // Trim spaces
        while (*p == ' ') p++;
    }
    
    if (count < 2) {
        free(copy);
        return -1;
    }
    
    turbo_rule_type_t type;
    const char *payload = NULL;
    const char *action_str = NULL;
    const char *proxy_url = NULL;

    if (strcmp(parts[0], "DOMAIN") == 0) type = TURBO_RULE_DOMAIN;
    else if (strcmp(parts[0], "DOMAIN-SUFFIX") == 0) type = TURBO_RULE_DOMAIN_SUFFIX;
    else if (strcmp(parts[0], "DOMAIN-KEYWORD") == 0) type = TURBO_RULE_DOMAIN_KEYWORD;
    else if (strcmp(parts[0], "IP-CIDR") == 0) type = TURBO_RULE_IP_CIDR;
    else if (strcmp(parts[0], "GEOIP") == 0) type = TURBO_RULE_GEOIP;
    else if (strcmp(parts[0], "MATCH") == 0) type = TURBO_RULE_MATCH;
    else { free(copy); return -2; }

    if (type == TURBO_RULE_MATCH) {
        action_str = parts[1];
        if (count >= 3) proxy_url = parts[2]; // e.g. MATCH,Proxy,url
    } else {
        if (count < 3) { free(copy); return -1; }
        payload = parts[1];
        action_str = parts[2];
        if (count >= 4) proxy_url = parts[3];
    }
    
    turbo_rule_action_type_t action;
    if (tstr_casecmp(action_str, "DIRECT") == 0) action = TURBO_RULE_ACTION_DIRECT;
    else if (tstr_casecmp(action_str, "REJECT") == 0) action = TURBO_RULE_ACTION_REJECT;
    else {
        action = TURBO_RULE_ACTION_PROXY;
        // If it's a proxy and no proxy_url given, the action_str might be the url/name
        if (!proxy_url) proxy_url = action_str; 
    }
    
    turbo_coro_rule_add(engine, type, payload, action, proxy_url);
    
    free(copy);
    return 0;
}

int turbo_coro_rule_group_add(turbo_coro_rule_engine_t *engine, 
                                       const char *name, 
                                       turbo_group_type_t type) {
    if (!engine || !name) return -1;
    
    turbo_group_t *g = calloc(1, sizeof(turbo_group_t));
    g->name = strdup(name);
    g->type = type;
    g->next = engine->groups;
    engine->groups = g;
    return 0;
}

int turbo_coro_rule_group_add_member(turbo_coro_rule_engine_t *engine, 
                                              const char *group_name, 
                                              const char *member) {
    if (!engine || !group_name || !member) return -1;
    
    turbo_group_t *g = engine->groups;
    while (g) {
        if (strcmp(g->name, group_name) == 0) {
            turbo_group_member_t *m = calloc(1, sizeof(turbo_group_member_t));
            m->name_or_url = strdup(member);
            m->is_alive = true;
            
            // Append to end
            if (!g->members) {
                g->members = m;
            } else {
                turbo_group_member_t *curr = g->members;
                while (curr->next) curr = curr->next;
                curr->next = m;
            }
            return 0;
        }
        g = g->next;
    }
    return -2;
}

int turbo_coro_rule_group_select(turbo_coro_rule_engine_t *engine, 
                                          const char *group_name, 
                                          const char *member) {
    if (!engine || !group_name || !member) return -1;
    turbo_group_t *g = engine->groups;
    while (g) {
        if (strcmp(g->name, group_name) == 0) {
            if (g->selected_member) free(g->selected_member);
            g->selected_member = strdup(member);
            return 0;
        }
        g = g->next;
    }
    return -2;
}

int turbo_coro_rule_group_update_member(turbo_coro_rule_engine_t *engine,
                                                const char *group_name,
                                                const char *member,
                                                bool alive,
                                                uint64_t latency_ms) {
    if (!engine || !group_name || !member) return -1;
    turbo_group_t *g = engine->groups;
    while (g) {
        if (strcmp(g->name, group_name) == 0) {
            turbo_group_member_t *m = g->members;
            while (m) {
                if (strcmp(m->name_or_url, member) == 0) {
                    m->is_alive = alive;
                    m->last_latency_ms = latency_ms;
                    return 0;
                }
                m = m->next;
            }
            return -3; // Member not found
        }
        g = g->next;
    }
    return -2; // Group not found
}

void turbo_coro_rule_update_health(turbo_coro_rule_engine_t *engine,
                                            const char *url_or_name,
                                            bool alive,
                                            uint64_t latency_ms) {
    if (!engine || !url_or_name) return;
    turbo_group_t *g = engine->groups;
    while (g) {
        turbo_group_member_t *m = g->members;
        while (m) {
            if (strcmp(m->name_or_url, url_or_name) == 0) {
                m->is_alive = alive;
                m->last_latency_ms = latency_ms;
            }
            m = m->next;
        }
        g = g->next;
    }
}

void turbo_coro_rule_engine_set_health_cb(turbo_coro_rule_engine_t *engine,
                                                  turbo_group_health_cb cb,
                                                  void *user_data) {
    if (!engine) return;
    engine->health_cb = cb;
    engine->health_user_data = user_data;
}

void turbo_coro_rule_engine_trigger_health_checks(turbo_coro_rule_engine_t *engine) {
    if (!engine || !engine->health_cb) return;
    turbo_group_t *g = engine->groups;
    while (g) {
        turbo_group_member_t *m = g->members;
        while (m) {
            bool is_nested = false;
            turbo_group_t *ng = engine->groups;
            while (ng) {
                 if (strcmp(ng->name, m->name_or_url) == 0) {
                     is_nested = true;
                     break;
                 }
                 ng = ng->next;
            }
            if (!is_nested && strcmp(m->name_or_url, "Direct") != 0 && strcmp(m->name_or_url, "Reject") != 0 && strcmp(m->name_or_url, "DIRECT") != 0 && strcmp(m->name_or_url, "REJECT") != 0) {
                engine->health_cb(m->name_or_url, engine->health_user_data);
            }
            m = m->next;
        }
        g = g->next;
    }
}

void turbo_coro_rule_dns_record(turbo_coro_rule_engine_t *engine, 
                                         const char *ip, 
                                         const char *domain) {
    if (!engine || !ip || !domain) return;
    
    // Check if exists
    turbo_dns_mapping_t *curr = engine->dns_cache;
    while (curr) {
        if (strcmp(curr->ip, ip) == 0) {
            strncpy(curr->domain, domain, sizeof(curr->domain)-1);
            return;
        }
        curr = curr->next;
    }
    
    turbo_dns_mapping_t *m = calloc(1, sizeof(turbo_dns_mapping_t));
    strncpy(m->ip, ip, sizeof(m->ip)-1);
    strncpy(m->domain, domain, sizeof(m->domain)-1);
    m->next = engine->dns_cache;
    engine->dns_cache = m;
}

static int compare_geoip(const void *a, const void *b) {
    geoip_range_t *g1 = (geoip_range_t *)a;
    geoip_range_t *g2 = (geoip_range_t *)b;
    if (g1->is_ipv6 != g2->is_ipv6) return g1->is_ipv6 ? 1 : -1;
    return memcmp(g1->start, g2->start, g1->is_ipv6 ? 16 : 4);
}

int turbo_coro_rule_geoip_load(turbo_coro_rule_engine_t *engine, const char *path) {
    if (!engine || !path) return -1;
    
    FILE *f = fopen(path, "r");
    if (!f) return -2;
    
    // Count lines
    size_t count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) count++;
    rewind(f);
    
    engine->geoip_db = (geoip_range_t*)realloc(engine->geoip_db, count * sizeof(geoip_range_t));
    engine->geoip_count = 0;
    while (fgets(line, sizeof(line), f)) {
        char start_str[64], end_str[64], country_str[4];
        if (sscanf(line, "%63[^,],%63[^,],%3s", start_str, end_str, country_str) == 3) {
            engine->geoip_db = (geoip_range_t *)realloc(engine->geoip_db, (engine->geoip_count + 1) * sizeof(geoip_range_t));
            geoip_range_t *range = &engine->geoip_db[engine->geoip_count];
            
            struct in_addr addr4_s, addr4_e;
            struct in6_addr addr6_s, addr6_e;
            
            if (uv_inet_pton(AF_INET, start_str, &addr4_s) == 0 && uv_inet_pton(AF_INET, end_str, &addr4_e) == 0) {
                memset(range->start, 0, 16);
                memset(range->end, 0, 16);
                memcpy(range->start, &addr4_s.s_addr, 4);
                memcpy(range->end, &addr4_e.s_addr, 4);
                range->is_ipv6 = false;
            } else if (uv_inet_pton(AF_INET6, start_str, &addr6_s) == 0 && uv_inet_pton(AF_INET6, end_str, &addr6_e) == 0) {
                memcpy(range->start, addr6_s.s6_addr, 16);
                memcpy(range->end, addr6_e.s6_addr, 16);
                range->is_ipv6 = true;
            } else {
                continue;
            }
            strncpy(range->country, country_str, 3);
            range->country[3] = '\0';
            engine->geoip_count++;
        }
    }
    
    fclose(f);
    
    // Sort for binary search
    if (engine->geoip_count > 0) {
        qsort(engine->geoip_db, engine->geoip_count, sizeof(geoip_range_t), compare_geoip);
    }
    
    TLOG_INFO("[Rule] Loaded {} GeoIP ranges from {}", engine->geoip_count, path);
    return 0;
}
