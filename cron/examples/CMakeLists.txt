add_executable(cron_example cron_example.c)

target_link_libraries(cron_example PRIVATE TurboNet::Cron TurboNet::Utils)

set_target_properties(cron_example PROPERTIES
  FOLDER cron/examples
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
