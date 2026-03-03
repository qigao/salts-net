#include "ts_plugin.h"
const exprtk_module_t *exprtk_module_vec(void);
TS_PLUGIN_MODULE(vec, exprtk_module_vec)
