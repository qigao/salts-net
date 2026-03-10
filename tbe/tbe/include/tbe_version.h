#ifndef TBE_VERSION_H
#define TBE_VERSION_H

#define TBE_VERSION_MAJOR 1
#define TBE_VERSION_MINOR 0
#define TBE_VERSION_PATCH 0

#define TBE_VERSION_STRING "1.0.0"

/**
 * @brief Get the library version string.
 * @return A static string in the format "MAJOR.MINOR.PATCH".
 */
const char *tbe_version(void);

/**
 * @brief Get the library version as separate components.
 * @param major Pointer to store major version (can be NULL).
 * @param minor Pointer to store minor version (can be NULL).
 * @param patch Pointer to store patch version (can be NULL).
 */
void tbe_version_components(int *major, int *minor, int *patch);

/**
 * @brief Thread safety: This library is NOT thread-safe.
 *
 * The TBE parser maintains internal state during parsing and does not
 * use any locking mechanisms. If you need to parse schemas from multiple
 * threads, you must:
 *
 * 1. Use separate Node trees for each thread, OR
 * 2. Serialize access to parse_schema() with external locking
 *
 * Once a schema is parsed into a Node tree, the tree can be safely
 * read from multiple threads as long as no thread modifies it.
 */

#endif /* TBE_VERSION_H */
