#include "mustache_node_driver.h"
#include <string.h>

static void* node_get_root(void* provider_data) {
    return provider_data; // Assume provider_data is the root Node*
}

static void* node_get_child_by_name(void* node_ptr, const char* name, size_t size, void* provider_data) {
    Node* node = (Node*)node_ptr;
    if (!node || node->type != NODE_MAP) return NULL;

    for (size_t i = 0; i < node->data.map.count; i++) {
        Node* item = node->data.map.items[i];
        if (item->name && strlen(item->name) == size && strncmp(item->name, name, size) == 0) {
            return item;
        }
    }
    return NULL;
}

static void* node_get_child_by_index(void* node_ptr, unsigned index, void* provider_data) {
    Node* node = (Node*)node_ptr;
    if (!node) return NULL;

    if (node->type == NODE_LIST) {
        if (index < node->data.list.count) {
            return node->data.list.items[index];
        }
        return NULL;
    }

    /* Single values are iterable as a list of 1 */
    if (index == 0) return node;

    return NULL;
}

static int node_dump(void* node_ptr, int (*out_fn)(const char*, size_t, void*), void* renderer_data, void* provider_data) {
    Node* node = (Node*)node_ptr;
    if (!node) return 0;

    if (node->type == NODE_STRING && node->data.string_val) {
        return out_fn(node->data.string_val, strlen(node->data.string_val), renderer_data);
    }

    return 0;
}

static const MUSTACHE_DATAPROVIDER NODE_PROVIDER = {
    .dump = node_dump,
    .get_root = node_get_root,
    .get_child_by_name = node_get_child_by_name,
    .get_child_by_index = node_get_child_by_index,
    .get_partial = NULL,
    .is_lambda = NULL,
    .call_lambda = NULL
};

const MUSTACHE_DATAPROVIDER* get_node_mustache_provider(void) {
    return &NODE_PROVIDER;
}
