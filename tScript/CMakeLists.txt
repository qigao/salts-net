add_subdirectory(exprtk)
add_subdirectory(ts_loader)
add_subdirectory(executor)
add_subdirectory(turbo_script)
add_subdirectory(examples)
add_subdirectory(modules)

set(TURBO_SCRIPT_MODULE_DLL_TARGETS
    feeds_plugin
    net_plugin
    vec_plugin
    ta_plugin
    fin_plugin
    ts_plugin
    sqlite_plugin
    wasm_plugin)

install(
  TARGETS turbo_script ${TURBO_SCRIPT_MODULE_DLL_TARGETS}
  EXPORT TurboNetTargets
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION bin
  ARCHIVE DESTINATION lib)
