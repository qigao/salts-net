#ifndef turboutils_DOTENV_H
#define turboutils_DOTENV_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Loads environment variables from a .env file into the process environment.
 *
 * If path is a directory, it looks for a file named ".env" inside it.
 * If path is a file, it loads that file.
 *
 * @param path Directory or file path.
 * @param overwrite Whether to overwrite existing environment variables.
 * @return 0 on success, -1 on failure (e.g., file not found).
 */
int dotenv_load(const char *path, bool overwrite);

/**
 * @brief Loads environment variables from the default ".env" file in the current directory.
 *
 * @param overwrite Whether to overwrite existing environment variables.
 * @return 0 on success, -1 on failure.
 */
int dotenv_load_default(bool overwrite);

#ifdef __cplusplus
}
#endif

#endif // turboutils_DOTENV_H
