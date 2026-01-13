set(folder_name quick/examples)

add_executable(qjsimp qjsimp.c)
target_link_libraries(qjsimp qjs)
set_target_properties(qjsimp PROPERTIES FOLDER ${folder_name})

# String Utils Demo
add_executable(string_utils_demo string_utils_demo.c)
target_link_libraries(string_utils_demo TurboNet::Quick)
set_target_properties(string_utils_demo PROPERTIES FOLDER ${folder_name})

# Debug String Utils
add_executable(debug_string_utils debug_string_utils.c)
target_link_libraries(debug_string_utils TurboNet::Quick)
set_target_properties(debug_string_utils PROPERTIES FOLDER ${folder_name})

# HTTP JS Runner
add_executable(http_runner http_runner.c)
target_link_libraries(http_runner TurboNet::Quick)
set_target_properties(http_runner PROPERTIES FOLDER ${folder_name})

# Copy JS example to output directory
add_custom_command(
  TARGET http_runner
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/http_example.js
          $<TARGET_FILE_DIR:http_runner>/http_example.js
  COMMENT "Copying http_example.js to build directory")
