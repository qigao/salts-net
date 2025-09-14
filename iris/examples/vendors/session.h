#ifndef SESSION_H
#define SESSION_H

#include <time.h>
#include <stdint.h>
#include "router.h"
#include "cookie.h"

#define SESSION_ID_LEN 32       // Length of the session ID (32 characters)
#define MAX_SESSIONS_DEFAULT 10 // Default initial capacity for sessions

// Session structure to hold session information
typedef struct
{
    char id[SESSION_ID_LEN + 1]; // Unique session ID (32 bytes + null terminator)
    char *data;                  // Data associated with the session (JSON format)
    time_t expires;              // Expiration time of the session (UNIX timestamp)
} Session;

// Initialize the session system with default capacity
// Returns: 1 on success, 0 on failure
int init_sessions(void);

// Clean up and free all session resources
void reset_sessions(void);

// Create a new session with specified max age in seconds
// Returns: Pointer to new session on success, NULL on failure
Session *create_session(int max_age);

// Find a session by its ID
// Returns: Pointer to session if found and not expired, NULL otherwise
Session *find_session(const char *id);

// Set a key-value pair in the session's data (stored as JSON)
// Properly escapes JSON special characters
// Parameters:
//   sess: Session to modify
//   key: Key name (will be JSON escaped)
//   value: Value (will be JSON escaped)
void set_session(Session *sess, const char *key, const char *value);

char *get_session_value(Session *sess, const char *key);
void remove_session_value(Session *sess, const char *key);

// Free a session and its associated resources
// Clears session ID, expiration time, and frees session data
void free_session(Session *sess);

// Get the authenticated session from request cookies
// Returns: Session if found and authenticated, NULL otherwise
Session *get_session(Req *req);

// Print all registered sessions to stdout (debug purposes)
void print_sessions(void);

// Send session cookie to the client
void send_session(Res *res, Session *sess, cookie_options_t *options);

// Delete the session both from the client and the memory
void destroy_session(Res *res, Session *sess, cookie_options_t *options);

#endif
