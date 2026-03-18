/**
 * @file mustache_node_driver.h
 * @brief Mustache Data Driver for Node tree
 */

#ifndef MUSTACHE_NODE_DRIVER_H
#define MUSTACHE_NODE_DRIVER_H

#include "platform.h"
#include "mustache.h"
#include "node_tree.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the standard DataProvider for a Node tree.
 */
const MUSTACHE_DATAPROVIDER* get_node_mustache_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSTACHE_NODE_DRIVER_H */
