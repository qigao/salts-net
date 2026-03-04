/**
 * @file ts_plugin_loader.c
 * @brief Cross-platform plugin loader implementation.
 */
#include "ts_plugin_loader.h"
#include <stdlib.h>

/* ── Platform dlopen shim ─────────────────────────────────────────── */
#ifdef _WIN32
  #include <windows.h>
static void *pl_dlopen(const char *path) { return (void *)LoadLibraryA(path); }
static void *pl_dlsym(void *h, const char *name) {
  return (void *)GetProcAddress((HMODULE)h, name);
}
static void pl_dlclose(void *h) { FreeLibrary((HMODULE)h); }
#else
  #include <dlfcn.h>
static void *pl_dlopen(const char *path) { return dlopen(path, RTLD_LAZY); }
static void *pl_dlsym(void *h, const char *name) { return dlsym(h, name); }
static void pl_dlclose(void *h) { dlclose(h); }
#endif

ts_plugin_handle_t *ts_plugin_load(const char *path) {
  if (!path)
    return NULL;

  void *dl = pl_dlopen(path);
  if (!dl)
    return NULL;

  ts_api_create_fn create_fn = (ts_api_create_fn)pl_dlsym(dl, "ts_api_create");
  if (!create_fn) {
    pl_dlclose(dl);
    return NULL;
  }

  const ts_plugin_t *plugin = create_fn();
  if (!plugin || !plugin->load) {
    pl_dlclose(dl);
    return NULL;
  }

  ts_plugin_handle_t *h = (ts_plugin_handle_t *)calloc(1, sizeof(*h));
  if (!h) {
    pl_dlclose(dl);
    return NULL;
  }

  h->dl_handle = dl;
  h->plugin = plugin;
  h->instance = NULL;
  return h;
}

int ts_plugin_init(ts_plugin_handle_t *h, void *env, void *scratch) {
  if (!h || !h->plugin || !h->plugin->load)
    return -1;
  h->instance = h->plugin->load(env, scratch);
  if (!h->instance)
    return -1;
  return 0;
}

void ts_plugin_unload(ts_plugin_handle_t *h) {
  if (!h)
    return;
  if (h->plugin && h->plugin->unload)
    h->plugin->unload(h->instance);
  if (h->dl_handle)
    pl_dlclose(h->dl_handle);
  free(h);
}
