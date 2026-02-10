#include "tinytest.h"
#include "mustache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 模拟数据结构
typedef struct {
    const char *key;
    const char *value;
} KV;

typedef struct {
    KV *globals;
    int n_globals;
    KV *items;
    int n_items;
    void *node_obj; 
} ScopeTestData;

// C 回调实现
static void *get_root(void *data) {
    return data;
}

static void *get_child_by_name(void *node, const char *name, size_t size, void *data) {
    ScopeTestData *d = (ScopeTestData *)data;
    if (node == d) {
        if (size == 5 && strncmp(name, "items", 5) == 0) return (void *)d->items;
        if (size == 4 && strncmp(name, "node", 4) == 0) return d->node_obj;

        for (int i = 0; i < d->n_globals; i++) {
            if (strlen(d->globals[i].key) == size && strncmp(d->globals[i].key, name, size) == 0) {
                return (void *)&d->globals[i];
            }
        }
    } else {
        KV *item = (KV *)node;
        if (item->key && strlen(item->key) == size && strncmp(item->key, name, size) == 0) {
            return (void *)item;
        }
    }
    return NULL;
}

static void *get_child_by_index(void *node, unsigned index, void *data) {
    ScopeTestData *d = (ScopeTestData *)data;
    if (node == d->items && (int)index < d->n_items) {
        return (void *)&d->items[index];
    }
    /* Per mustache spec: truthy scalar/object is iterable as a single-element list. */
    if (node != d && node != d->items && index == 0) {
        return node;
    }
    return NULL;
}

static int dump(void *node, int (*out)(const char *, size_t, void *), void *rdata, void *pdata) {
    KV *kv = (KV *)node;
    if (kv && kv->value) return out(kv->value, strlen(kv->value), rdata);
    return 0;
}

spec("mustache scope climbing") {
    describe("section resolution") {
        it("should support object as section (truthy non-list)") {
            KV globals[] = { {"id", "root_id"} };
            KV node_obj = { "id", "node_id" };
            ScopeTestData data = { globals, 1, NULL, 0, &node_obj }; 

            MUSTACHE_DATAPROVIDER provider = { dump, get_root, get_child_by_name, get_child_by_index, NULL };
            const char *tpl_text = "[{{#node}}{{id}}{{/node}}]";
            MUSTACHE_TEMPLATE *tpl = mustache_compile(tpl_text, strlen(tpl_text), NULL, NULL, 0);
            
            MUSTACHE_STRING_RENDERER renderer;
            mustache_string_renderer_init(&renderer);
            mustache_process(tpl, (MUSTACHE_RENDERER *)&renderer, &renderer, &provider, &data);
            
            char *output = mustache_string_renderer_get(&renderer);
            check_str_eq(output, "[node_id]");
            
            free(output);
            mustache_string_renderer_free(&renderer);
            mustache_release(tpl);
        }
        
        it("should support implicit iterator {{.}}") {
            // 修正：将 standalone 包装在 KV 结构中
            KV val_obj = { ".", "standalone_value" };
            ScopeTestData data = { NULL, 0, NULL, 0, &val_obj }; 

            MUSTACHE_DATAPROVIDER provider = { dump, get_root, get_child_by_name, get_child_by_index, NULL };
            const char *tpl_text = "{{#node}}val:{{.}}{{/node}}";
            MUSTACHE_TEMPLATE *tpl = mustache_compile(tpl_text, strlen(tpl_text), NULL, NULL, 0);
            
            MUSTACHE_STRING_RENDERER renderer;
            mustache_string_renderer_init(&renderer);
            mustache_process(tpl, (MUSTACHE_RENDERER *)&renderer, &renderer, &provider, &data);
            
            char *output = mustache_string_renderer_get(&renderer);
            check_str_eq(output, "val:standalone_value");
            
            free(output);
            mustache_string_renderer_free(&renderer);
            mustache_release(tpl);
        }

        it("should climb to root context when variable is missing in child") {
            KV globals[] = { {"color", "red"} };
            KV items[] = { {"name", "A"}, {"name", "B"} };
            ScopeTestData data = { globals, 1, items, 2, NULL };

            MUSTACHE_DATAPROVIDER provider = { dump, get_root, get_child_by_name, get_child_by_index, NULL };
            const char *tpl_text = "{{#items}}{{name}}:{{color}},{{/items}}";
            MUSTACHE_TEMPLATE *tpl = mustache_compile(tpl_text, strlen(tpl_text), NULL, NULL, 0);
            
            MUSTACHE_STRING_RENDERER renderer;
            mustache_string_renderer_init(&renderer);
            mustache_process(tpl, (MUSTACHE_RENDERER *)&renderer, &renderer, &provider, &data);
            
            char *output = mustache_string_renderer_get(&renderer);
            check_str_eq(output, "A:red,B:red,");
            
            free(output);
            mustache_string_renderer_free(&renderer);
            mustache_release(tpl);
        }
    }
}
