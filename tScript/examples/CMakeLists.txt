add_executable(example_custom_module example_custom_module.c)

target_include_directories(example_custom_module PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../turbo_script/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../exprtk/include
  ${CMAKE_SOURCE_DIR}/shared/utils/include
  ${CMAKE_SOURCE_DIR}/parser/turbo_parser/include
)

target_link_libraries(example_custom_module PRIVATE turbo_script TurboNet::Parser)
set_target_properties(example_custom_module PROPERTIES FOLDER turbo_script/examples)
