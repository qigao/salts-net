#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "cookie.h"
#include "security.h"
#include <stb_sprintf.h>
#include "turbo_logger.h"
char *get_cookie(Req *req, const char *name)
{
    if (!req || !name)
        return NULL;

    // Validate the requested cookie name first
    const iris_security_limits_t *limits = iris_security_get_limits();
    iris_security_result_t name_validation = iris_validate_cookie_name(name, limits->max_cookie_name_length);
    if (name_validation != IRIS_SECURITY_OK) {
        // Log security issue but don't crash - just return NULL
        TLOG_ERROR("Security: Invalid cookie name requested: {} (error: {})",
                name, iris_security_error_string(name_validation));
        return NULL;
    }

    const char *cookie_header = get_headers(req, "Cookie");
    if (!cookie_header)
        return NULL;

    size_t name_len = strlen(name);
    const char *pos = cookie_header;

    while (pos)
    {
        // Skip whitespace
        while (*pos && isspace((unsigned char)*pos))
            pos++;

        if (!*pos)
            break;

        // Check if this is our cookie
        if (strncmp(pos, name, name_len) == 0 && pos[name_len] == '=')
        {
            // Found our cookie, extract value
            pos += name_len + 1; // Skip 'name='

            // Find the end of the value (either ';' or end of string)
            const char *end = strchr(pos, ';');
            size_t len = end ? (size_t)(end - pos) : strlen(pos);

            // Trim trailing whitespace
            while (len > 0 && isspace((unsigned char)pos[len - 1]))
                len--;

            // Allocate and copy value
            char *value = malloc(len + 1);
            if (!value)
                return NULL;

            memcpy(value, pos, len);
            value[len] = '\0';

            // Validate the extracted cookie value
            iris_security_result_t value_validation = iris_validate_cookie_value(value, limits->max_cookie_value_length);
            if (value_validation != IRIS_SECURITY_OK) {
                // Log security issue and free the value
                TLOG_ERROR("Security: Invalid cookie value for '{}' (error: {})",
                        name, iris_security_error_string(value_validation));
                free(value);
                return NULL;
            }

            return value;
        }

        // Move to next cookie
        pos = strchr(pos, ';');
        if (pos)
            pos++; // Skip the ';'
    }

    return NULL;
}

void set_cookie(Res *res, const char *name, const char *value, cookie_options_t *options)
{
    if (!res || !name || !value)
    {
        TLOG_ERROR("Invalid parameters for set_cookie");
        return;
    }

    // Validate cookie name and value before setting
    iris_security_result_t validation_result = iris_validate_cookie(name, value);
    if (validation_result != IRIS_SECURITY_OK) {
        TLOG_ERROR("Security: Cannot set invalid cookie '{}' (error: {})",
                name, iris_security_error_string(validation_result));
        return;
    }

    if (options && options->max_age < 0)
    {
        TLOG_ERROR("Invalid max_age value");
        return;
    }

    int max_age = (options && options->max_age >= 0) ? options->max_age : -1;
    const char *path = (options && options->path) ? options->path : "/";
    const char *same_site = (options && options->same_site) ? options->same_site : NULL;
    bool http_only = options ? options->http_only : false;
    bool secure = options ? options->secure : false;

    if (!path)
        path = "/";

    int needed = stbsp_snprintf(NULL, 0, "%s=%s", name, value);

    if (max_age >= 0)
    {
        needed += stbsp_snprintf(NULL, 0, "; Max-Age=%d", max_age);
    }

    needed += stbsp_snprintf(NULL, 0, "; Path=%s", path);

    if (same_site && strlen(same_site) > 0)
    {
        needed += stbsp_snprintf(NULL, 0, "; SameSite=%s", same_site);
    }

    if (http_only)
    {
        needed += (int)strlen("; HttpOnly");
    }

    if (secure)
    {
        needed += (int)strlen("; Secure");
    }

    if (needed < 0)
    {
        TLOG_ERROR("Cookie formatting error");
        return;
    }

    char *cookie_val = malloc((size_t)needed + 1);
    if (!cookie_val)
    {
        perror("malloc for cookie_val");
        return;
    }

    int written = stbsp_snprintf(cookie_val, (size_t)needed + 1, "%s=%s", name, value);
    if (written < 0)
    {
        TLOG_ERROR("Cookie formatting error");
        free(cookie_val);
        return;
    }

    if (max_age >= 0)
    {
        int ret = stbsp_snprintf(cookie_val + written, (size_t)needed + 1 - written, "; Max-Age=%d", max_age);
        if (ret < 0)
            goto error;
        written += ret;
    }

    int ret = stbsp_snprintf(cookie_val + written, (size_t)needed + 1 - written, "; Path=%s", path);
    if (ret < 0)
        goto error;
    written += ret;

    if (same_site && strlen(same_site) > 0)
    {
        ret = stbsp_snprintf(cookie_val + written, (size_t)needed + 1 - written, "; SameSite=%s", same_site);
        if (ret < 0)
            goto error;
        written += ret;
    }

    if (http_only)
    {
        ret = stbsp_snprintf(cookie_val + written, (size_t)needed + 1 - written, "; HttpOnly");
        if (ret < 0)
            goto error;
        written += ret;
    }

    if (secure)
    {
        ret = stbsp_snprintf(cookie_val + written, (size_t)needed + 1 - written, "; Secure");
        if (ret < 0)
            goto error;
        written += ret;
    }

    set_header(res, "Set-Cookie", cookie_val);
    free(cookie_val);
    return;

error:
    TLOG_ERROR("Cookie formatting error during construction");
    free(cookie_val);
}
