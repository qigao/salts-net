add_subdirectory(exprtk) 
add_subdirectory(turbo_script)
add_subdirectory(examples)
add_subdirectory(modules)

set(TURBO_SCRIPT_MODULE_DLL_TARGETS
    feeds_plugin
    net_plugin
    ta_plugin
    fin_plugin
    sqlite_plugin
    wasm_plugin)

# Subdirectories are responsible for their own target configuration and installation
# using turbonet_configure_target()
