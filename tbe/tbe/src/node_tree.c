#include "node_tree.h"
#include "schema_parser_dsl.h"
#include <stdlib.h>
#include <string.h>

Node *create_node_string(const char *name, const char *val) {
  if (!val) return NULL;

  Node *n = (Node *)calloc(1, sizeof(Node));
  if (!n) return NULL;

  n->type = NODE_STRING;
  if (name) {
    n->name = strdup(name);
    if (!n->name) {
      free(n);
      return NULL;
    }
  }

  n->data.string_val = strdup(val);
  if (!n->data.string_val) {
    free((void *)n->name);
    free(n);
    return NULL;
  }

  return n;
}

Node *create_node_list(const char *name) {
  Node *n = (Node *)calloc(1, sizeof(Node));
  if (!n) return NULL;

  n->type = NODE_LIST;
  if (name) {
    n->name = strdup(name);
    if (!n->name) {
      free(n);
      return NULL;
    }
  }
  return n;
}

static int node_add_child(Node *parent, Node *item) {
  Node ***items_ptr;
  size_t *count, *cap;

  if (parent->type == NODE_LIST) {
    items_ptr = &parent->data.list.items;
    count = &parent->data.list.count;
    cap = &parent->data.list.cap;
  } else {
    items_ptr = &parent->data.map.items;
    count = &parent->data.map.count;
    cap = &parent->data.map.cap;
  }

  if (*count == *cap) {
    size_t new_cap = *cap ? *cap * 2 : 8;
    Node **new_items = (Node **)realloc(*items_ptr, new_cap * sizeof(Node *));
    if (!new_items) return -1;
    *items_ptr = new_items;
    *cap = new_cap;
  }
  (*items_ptr)[(*count)++] = item;
  return 0;
}

int list_add(Node *list, Node *item) {
  return node_add_child(list, item);
}

Node *create_node_map(const char *name) {
  Node *n = (Node *)calloc(1, sizeof(Node));
  if (!n) return NULL;

  n->type = NODE_MAP;
  if (name) {
    n->name = strdup(name);
    if (!n->name) {
      free(n);
      return NULL;
    }
  }
  return n;
}

int map_add(Node *map, Node *item) {
  return node_add_child(map, item);
}

void node_free(Node *node) {
  if (!node) return;
  switch (node->type) {
  case NODE_STRING:
    free(node->data.string_val);
    break;
  case NODE_LIST:
    for (size_t i = 0; i < node->data.list.count; i++)
      node_free(node->data.list.items[i]);
    free(node->data.list.items);
    break;
  case NODE_MAP:
    for (size_t i = 0; i < node->data.map.count; i++)
      node_free(node->data.map.items[i]);
    free(node->data.map.items);
    break;
  default:
    break;
  }
  free((void *)node->name);
  free(node);
}
