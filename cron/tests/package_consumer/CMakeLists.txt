cmake_minimum_required(VERSION 3.20)

project(cron_package_consumer LANGUAGES C)

find_package(TurboNet CONFIG REQUIRED PATHS "${TurboNet_DIR}" NO_DEFAULT_PATH)

add_executable(cron_package_consumer main.c)
target_link_libraries(cron_package_consumer PRIVATE TurboNet::Cron)

set_target_properties(cron_package_consumer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
