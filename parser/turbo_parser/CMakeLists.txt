add_library(turbo_parser SHARED src/turbo_parser.c)
add_library(TurboNet::Parser ALIAS turbo_parser) 

target_include_directories(
  turbo_parser
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../internal/csv_parser/include
          ${CMAKE_CURRENT_SOURCE_DIR}/../internal/ini_parser/include
          ${CMAKE_CURRENT_SOURCE_DIR}/../internal/ltv_parser/include
          ${CMAKE_CURRENT_SOURCE_DIR}/../internal/soa_parser/include
          ${CMAKE_CURRENT_SOURCE_DIR}/../internal/cmd_parser/include
          ${CMAKE_SOURCE_DIR}/common/include # for platform.h used by uri_parser
)

 
target_link_libraries(
  turbo_parser
  PUBLIC TurboNet::Utils
  PRIVATE csv_parser
          ini_parser
          json_parser
          uri_parser
          tlv_parser
          ltv_parser
          soa_parser
          cmd_parser)

set_target_properties(
  turbo_parser
  PROPERTIES VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME Parser
             FOLDER turbo_parser)

target_compile_definitions(turbo_parser PRIVATE SHARED_CXX)

install(
  TARGETS turbo_parser
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

# Install headers
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_parser.h
        DESTINATION include)

if(BUILD_TESTING)
  add_executable(test_turbo_parser test/test_turbo_parser.c)
  target_link_libraries(test_turbo_parser PRIVATE turbo_parser)
  set_target_properties(test_turbo_parser PROPERTIES FOLDER turbo_parser)
  add_sanitizers(test_turbo_parser)
  add_test(NAME test_turbo_parser COMMAND test_turbo_parser)
endif()
