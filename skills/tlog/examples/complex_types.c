#include <tlog.h>
#include <tlog_dump.h>
#include <stdio.h>
#include <string.h>

// Example struct
typedef struct {
    int id;
    char name[32];
    uint32_t ip;
} User;

// Helper to stringify User struct using thread-local buffer manually
// (If you don't use tlog_dump helpers, you can roll your own like this)
const char* user_to_string(const User* u) {
    static _Thread_local char buf[128];
    snprintf(buf, sizeof(buf), "User(id=%d, name=%s, ip=%s)", 
             u->id, u->name, tlog_ip4(u->ip));
    return buf;
}

int main() {
    // 1. Setup logger
    tlog_config_t config = {
        .min_level = TURBO_LOG_LEVEL_DEBUG,
        .async_mode = 0 // Sync for this simple example
    };
    tlog_t *logger = tlog_create(&config);
    if (!logger) return 1;
    
    tlog_add_sink(logger, turbo_sink_console_create(NULL));
    tlog_set_default(logger);

    // 2. Log using helpers
    uint8_t raw_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF};
    
    // tlog_hex uses a ring buffer, so we can use it multiple times in one call?
    // The implementation has a ring size of 4.
    // Let's test single usage
    TLOG_INFO("Received raw data: {}", tlog_hex(raw_data, sizeof(raw_data)));
    
    // Test multiple usages (Hex + IP)
    uint32_t ip = 0x0100007F; // 127.0.0.1 (reverse byte order if little endian machine, but tlog_ip4 takes raw uint32)
    // Actually tlog_ip4 takes bytes cast to uint32. 
    // On Little Endian: 0x0100007F -> 7F 00 00 01 -> 127.0.0.1
    
    TLOG_INFO("Connection from {} sent payload: {}", tlog_ip4(ip), tlog_hex(raw_data, 4));

    // 3. Log Structs
    User u = { .id = 42, .name = "Alice", .ip = ip };
    TLOG_INFO("User Login: {}", user_to_string(&u));

    tlog_destroy(logger);
    return 0;
}
