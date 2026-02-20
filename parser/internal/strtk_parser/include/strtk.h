#ifndef TURBONET_PARSER_INTERNAL_STRTK_H
#define TURBONET_PARSER_INTERNAL_STRTK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =================================================================================================
// 1. Tokenizer
// =================================================================================================

/**
 * @brief Tokenizes a string using specified delimiters.
 * 
 * Supports basic delimiters and quoted strings.
 * 
 * @param input Input string (null-terminated recommended).
 * @param len Length of input string (use 0 for auto-detect).
 * @param delimiters String containing delimiter characters (e.g., " ,").
 * @param ignore_empty If true, skips empty tokens.
 * @param count Output: Number of tokens found.
 * @return char** Array of token strings (null-terminated). Caller must free with strtk_free_tokens.
 */
char **strtk_tokenize(const char *input, size_t len, const char *delimiters, bool ignore_empty, size_t *count);

/**
 * @brief Frees the token array returned by strtk_tokenize.
 * 
 * @param tokens Token array.
 * @param count Number of tokens.
 */
void strtk_free_tokens(char **tokens, size_t count);


// =================================================================================================
// 2. Conversion
// =================================================================================================

/**
 * @brief Converts string to integer (long long).
 */
bool strtk_to_int(const char *s, long long *out);

/**
 * @brief Converts string to double.
 */
bool strtk_to_double(const char *s, double *out);

/**
 * @brief Converts string to boolean ("true"/"false", "1"/"0", "yes"/"no").
 */
bool strtk_to_bool(const char *s, bool *out);


// =================================================================================================
// 3. String Splitting (Simpler API)
// =================================================================================================

/**
 * @brief Splits a string by a single delimiter char.
 */
char **strtk_split(const char *input, char delimiter, size_t *count);


#ifdef __cplusplus
}
#endif

#endif // TURBONET_PARSER_INTERNAL_STRTK_H
