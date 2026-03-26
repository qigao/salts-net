#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <platform.h>

#include "session.h"
#include "request.h"
#include "compat.h"
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#endif

// Dynamic session storage
static Session *sessions = NULL;
static int max_sessions = 0;
static int initialized = 0;

// Key-Value format separators (using ASCII control characters)
#define KV_DELIMITER '\x1F'        // ASCII Unit Separator - separates key from value
#define PAIR_DELIMITER '\x1E'      // ASCII Record Separator - separates key-value pairs
#define MAX_SESSION_DATA_SIZE 4096 // Prevent unlimited growth

static const cookie_options_t COOKIE_DEFAULTS = {
    .path = "/",
    .same_site = "Lax",
    .http_only = true,
    .secure = true,
};


static char *safe_strdup(const char *str)
{
    if (!str)
        return NULL;
    size_t len = strlen(str);
    char *copy = malloc(len + 1);
    if (copy)
    {
        memcpy(copy, str, len + 1);
    }
    return copy;
}

int init_sessions(void)
{
    if (initialized)
        return 1;

    const int initial_capacity = MAX_SESSIONS_DEFAULT;
    sessions = calloc(initial_capacity, sizeof(Session));
    if (!sessions)
        return 0;

    max_sessions = initial_capacity;
    initialized = 1;
    return 1;
}

void reset_sessions(void)
{
    if (!initialized)
        return;

    // Free all session data
    for (int i = 0; i < max_sessions; i++)
    {
        if (sessions[i].id[0] != '\0' && sessions[i].data != NULL)
        {
            free(sessions[i].data);
            sessions[i].data = NULL;
        }
    }

    free(sessions);
    sessions = NULL;
    max_sessions = 0;
    initialized = 0;
}

static int resize_sessions(int new_capacity)
{
    if (new_capacity <= max_sessions)
        return 1;

    Session *new_sessions = realloc(sessions, new_capacity * sizeof(Session));
    if (!new_sessions)
        return 0;

    // Initialize new session slots
    for (int i = max_sessions; i < new_capacity; i++)
    {
        memset(&new_sessions[i], 0, sizeof(Session));
    }

    sessions = new_sessions;
    max_sessions = new_capacity;
    return 1;
}

static int get_random_bytes(unsigned char *buffer, size_t length)
{
#ifdef _WIN32
    HCRYPTPROV hCryptProv;
    int result = 0;

    if (CryptAcquireContext(&hCryptProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        if (CryptGenRandom(hCryptProv, (DWORD)length, buffer))
            result = 1;
        CryptReleaseContext(hCryptProv, 0);
    }
    return result;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return 0;

    size_t bytes_read = 0;
    while (bytes_read < length)
    {
        ssize_t result = read(fd, buffer + bytes_read, length - bytes_read);
        if (result < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            return 0;
        }
        bytes_read += (size_t)result;
    }

    close(fd);
    return 1;
#endif
}

static void generate_session_id(char *buffer)
{
    unsigned char entropy[SESSION_ID_LEN];

    if (!get_random_bytes(entropy, SESSION_ID_LEN))
    {
        fprintf(stderr, "Random generation failed, using fallback method\n");

        unsigned int seed = (unsigned int)time(NULL);
#ifdef _WIN32
        seed ^= (unsigned int)GetCurrentProcessId();
#else
        seed ^= (unsigned int)getpid();
#endif

        static unsigned int counter = 0;
        seed ^= ++counter;

        void *stack_var = &seed;
        seed ^= ((uintptr_t)stack_var >> 3);

        srand(seed);
        for (size_t i = 0; i < SESSION_ID_LEN; i++)
        {
            entropy[i] = (unsigned char)(rand() & 0xFF);
        }
    }

    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const size_t charset_len = sizeof(charset) - 1;

    for (size_t i = 0; i < SESSION_ID_LEN; i++)
    {
        buffer[i] = charset[entropy[i] % charset_len];
    }

    // Clear entropy from memory
    memset(entropy, 0, SESSION_ID_LEN);
    buffer[SESSION_ID_LEN] = '\0';
}

static void cleanup_expired_sessions(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < max_sessions; i++)
    {
        if (sessions[i].id[0] != '\0' && sessions[i].expires < now)
            free_session(&sessions[i]);
    }
}

Session *create_session(int max_age)
{
    if (!initialized && !init_sessions())
        return NULL;

    cleanup_expired_sessions();

    int slot = -1;
    for (int i = 0; i < max_sessions; i++)
    {
        if (sessions[i].id[0] == '\0')
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        if (!resize_sessions(max_sessions * 2))
            return NULL;
        slot = max_sessions / 2;
    }

    generate_session_id(sessions[slot].id);
    sessions[slot].expires = time(NULL) + max_age;

    // Initialize with empty data
    free(sessions[slot].data);
    sessions[slot].data = safe_strdup("");
    if (!sessions[slot].data)
    {
        sessions[slot].id[0] = '\0';
        return NULL;
    }

    return &sessions[slot];
}

Session *find_session(const char *id)
{
    if (!id || !initialized)
        return NULL;

    time_t now = time(NULL);
    for (int i = 0; i < max_sessions; i++)
    {
        if (sessions[i].id[0] != '\0' &&
            strcmp(sessions[i].id, id) == 0 &&
            sessions[i].expires >= now)
        {
            return &sessions[i];
        }
    }

    return NULL;
}

static void remove_key_from_data(char *data, const char *encoded_key)
{
    if (!data || !encoded_key)
        return;

    char *search_start = data;

    while (search_start && *search_start)
    {
        char *key_end = strchr(search_start, KV_DELIMITER);
        if (!key_end)
            break;

        size_t key_len = key_end - search_start;

        if (key_len == strlen(encoded_key) &&
            strncmp(search_start, encoded_key, key_len) == 0)
        {

            // Found the key, find the end of this pair
            char *value_start = key_end + 1;
            char *pair_end = strchr(value_start, PAIR_DELIMITER);

            if (pair_end)
            {
                // Remove this pair by shifting the rest
                char *after_pair = pair_end + 1;
                size_t remaining_len = strlen(after_pair);
                memmove(search_start, after_pair, remaining_len + 1);
            }
            else
            {
                // This is the last pair, just truncate
                *search_start = '\0';
            }
            break;
        }

        // Move to next pair
        char *next_pair = strchr(search_start, PAIR_DELIMITER);
        search_start = next_pair ? next_pair + 1 : NULL;
    }
}

void set_session(Session *sess, const char *key, const char *value)
{
    if (!sess || !key || !value)
        return;

    char *encoded_key = turbo_url_encode(key);
    char *encoded_value = turbo_url_encode(value);
    if (!encoded_key || !encoded_value)
    {
        free(encoded_key);
        free(encoded_value);
        return;
    }

    // Check session data size limit before adding
    size_t current_len = sess->data ? strlen(sess->data) : 0;
    size_t key_len = strlen(encoded_key);
    size_t value_len = strlen(encoded_value);
    size_t additional_size = key_len + value_len + 2; // KV_DELIMITER + PAIR_DELIMITER

    if (current_len + additional_size > MAX_SESSION_DATA_SIZE)
    {
        fprintf(stderr, "Session data size limit exceeded\n");
        free(encoded_key);
        free(encoded_value);
        return;
    }

    // Remove existing key if present
    if (sess->data && strlen(sess->data) > 0)
    {
        remove_key_from_data(sess->data, encoded_key);
    }

    // Add new key-value pair
    current_len = sess->data ? strlen(sess->data) : 0;
    size_t new_len = current_len + key_len + value_len + 3; // key + KV_DELIM + value + PAIR_DELIM + null

    char *new_data = malloc(new_len);
    if (new_data)
    {
        if (current_len > 0)
        {
            strcpy(new_data, sess->data);
        }
        else
        {
            new_data[0] = '\0';
        }

        // Append new pair
        strcat(new_data, encoded_key);

        // Add delimiters manually for safety
        size_t pos = strlen(new_data);
        new_data[pos] = KV_DELIMITER;
        new_data[pos + 1] = '\0';

        strcat(new_data, encoded_value);

        pos = strlen(new_data);
        new_data[pos] = PAIR_DELIMITER;
        new_data[pos + 1] = '\0';

        free(sess->data);
        sess->data = new_data;
    }

    free(encoded_key);
    free(encoded_value);
}

char *get_session_value(Session *sess, const char *key)
{
    if (!sess || !key || !sess->data || strlen(sess->data) == 0)
        return NULL;

    char *encoded_key = turbo_url_encode(key);
    if (!encoded_key)
        return NULL;

    char *search_start = sess->data;
    char *result = NULL;

    while (search_start && *search_start)
    {
        // Find key delimiter
        char *key_end = strchr(search_start, KV_DELIMITER);
        if (!key_end)
            break;

        size_t key_len = key_end - search_start;

        // Check if this is our key
        if (key_len == strlen(encoded_key) &&
            strncmp(search_start, encoded_key, key_len) == 0)
        {

            // Found the key, extract value
            char *value_start = key_end + 1;
            char *value_end = strchr(value_start, PAIR_DELIMITER);

            size_t value_len = value_end ? (value_end - value_start) : strlen(value_start);

            char *encoded_value = malloc(value_len + 1);
            if (encoded_value)
            {
                strncpy(encoded_value, value_start, value_len);
                encoded_value[value_len] = '\0';

                result = turbo_url_decode(encoded_value);
                free(encoded_value);
            }
            break;
        }

        // Move to next pair
        char *next_pair = strchr(search_start, PAIR_DELIMITER);
        search_start = next_pair ? next_pair + 1 : NULL;
    }

    free(encoded_key);
    return result;
}

void remove_session_value(Session *sess, const char *key)
{
    if (!sess || !key || !sess->data)
        return;

    char *encoded_key = turbo_url_encode(key);
    if (!encoded_key)
        return;

    remove_key_from_data(sess->data, encoded_key);
    free(encoded_key);
}

void free_session(Session *sess)
{
    if (!sess)
        return;

    memset(sess->id, 0, sizeof(sess->id));
    sess->expires = 0;
    if (sess->data)
    {
        free(sess->data);
        sess->data = NULL;
    }
}

Session *get_session(Req *req)
{
    char *sid = get_cookie(req, "session");
    if (!sid)
        return NULL;

    Session *sess = find_session(sid);
    free(sid);
    return sess;
}

void print_sessions(void)
{
    time_t now = time(NULL);
    printf("=== Sessions ===\n");

    for (int i = 0; i < max_sessions; i++)
    {
        Session *s = &sessions[i];
        if (s->id[0] == '\0')
            continue;

        printf("[#%02d] id=%.8s..., expires in %lds\n",
               i, s->id, (long)(s->expires - now));

        // Parse and display key-value pairs nicely
        if (s->data && strlen(s->data) > 0)
        {
            char *data_copy = safe_strdup(s->data);
            if (data_copy)
            {
                char *pair_start = data_copy;
                int pair_count = 0;

                while (pair_start && *pair_start)
                {
                    char *key_end = strchr(pair_start, KV_DELIMITER);
                    if (!key_end)
                        break;

                    *key_end = '\0';
                    char *value_start = key_end + 1;
                    char *pair_end = strchr(value_start, PAIR_DELIMITER);

                    if (pair_end)
                        *pair_end = '\0';

                    // Decode for display
                    char *decoded_key = turbo_url_decode(pair_start);
                    char *decoded_value = turbo_url_decode(value_start);

                    printf("      %s = %s\n",
                           decoded_key ? decoded_key : pair_start,
                           decoded_value ? decoded_value : value_start);

                    free(decoded_key);
                    free(decoded_value);

                    pair_start = pair_end ? pair_end + 1 : NULL;
                    pair_count++;
                }

                if (pair_count == 0)
                    printf("      (empty)\n");

                free(data_copy);
            }
        }
        else
        {
            printf("      (empty)\n");
        }
    }
    printf("================\n");
}

void send_session(Res *res, Session *sess, cookie_options_t *options)
{
    if (!res || !sess || sess->id[0] == '\0')
        return;

    time_t now = time(NULL);
    int max_age = (int)difftime(sess->expires, now);
    if (max_age < 0)
        return;

    cookie_options_t opt = options ? *options : COOKIE_DEFAULTS;
    opt.max_age = max_age;

    set_cookie(res, "session", sess->id, &opt);
}

void destroy_session(Res *res, Session *sess, cookie_options_t *options)
{
    if (!res || !sess || sess->id[0] == '\0')
        return;

    cookie_options_t opt = options ? *options : COOKIE_DEFAULTS;
    opt.max_age = 0;

    set_cookie(res, "session", "", &opt);
    free_session(sess);
}
