import re

file_path = r"c:\projects\cpp\turbonet\turbonet\tScript\modules\fin\src\exprtk_mod_strategy.c"
with open(file_path, "r", encoding="utf-8") as f:
    content = f.read()

# Find all internal static functions to export
matches = re.findall(r"^static\s+exprtk_value_t\s+(fn_\w+)\s*\(", content, re.MULTILINE)

# Remove 'static ' from those function definitions
for func in matches:
    content = re.sub(r"^static\s+exprtk_value_t\s+" + func + r"\b", r"exprtk_value_t " + func, content, count=1, flags=re.MULTILINE)

# Also remove the exprtk_module_strategy return at the bottom
content = re.sub(r"const\s+exprtk_module_t\s*\*\s*exprtk_module_strategy\s*\(\s*void\s*\)\s*\{[^}]*\}", "", content)

with open(file_path, "w", encoding="utf-8") as f:
    # Also add the include for the new header at the top
    if '#include "exprtk_mod_strategy.h"' not in content:
        content = re.sub(r'#include "exprtk\.h"', '#include "exprtk.h"\n#include "exprtk_mod_strategy.h"', content, count=1)
    f.write(content)

# Now generate the header file
header_path = r"c:\projects\cpp\turbonet\turbonet\tScript\modules\fin\src\exprtk_mod_strategy.h"
header_content = """#ifndef EXPRTK_MOD_STRATEGY_H
#define EXPRTK_MOD_STRATEGY_H

#include "exprtk.h"
#include "exprtk_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exported function prototypes */
"""
for func in matches:
    header_content += f"exprtk_value_t {func}(size_t argc, exprtk_value_t *args, exprtk_env_t *env, turbo_pool_t *arena);\n"

header_content += """
/* Module entry point */
const exprtk_module_t *exprtk_module_strategy(void);

#ifdef __cplusplus
}
#endif

#endif /* EXPRTK_MOD_STRATEGY_H */
"""

with open(header_path, "w", encoding="utf-8") as f:
    f.write(header_content)

print(f"Exported {len(matches)} functions.")
