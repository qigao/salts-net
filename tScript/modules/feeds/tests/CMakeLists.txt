add_executable(test_feeds_module test_feeds.c)
target_link_libraries(test_feeds_module PRIVATE ts_loader exprtk TurboNet::TinyTest)
add_test(NAME test_feeds_module COMMAND test_feeds_module)
set_target_properties(test_feeds_module PROPERTIES FOLDER turbo_script/modules/feeds)

add_custom_command(
  TARGET test_feeds_module
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:feeds_plugin>
          $<TARGET_FILE_DIR:test_feeds_module>)
