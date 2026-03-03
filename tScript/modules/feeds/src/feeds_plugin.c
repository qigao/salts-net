#include "ts_plugin.h"

void *feeds_ctx_create(void);
void feeds_load(void *ctx, void *env, void *scratch);
void feeds_ctx_destroy(void *ctx);

TS_PLUGIN_STATEFUL(feeds, feeds_ctx_create, feeds_load, feeds_ctx_destroy)
