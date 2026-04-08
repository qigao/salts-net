add_library(turbo_parser SHARED src/turbo_parser.c)
cmake_config_target(turbo_parser
  ALIAS TurboNet::Parser
  EXPORT_NAME Parser
  FOLDER turbo_parser)

target_include_directories(
  turbo_parser
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
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
          dotenv_parser
          cmd_parser
          toon
          toml_parser
          datetime_parser
          cxml)

target_compile_definitions(turbo_parser PRIVATE SHARED_CXX)

# Install headers
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_parser.h
        DESTINATION include)

# Tests
cmake_add_test(
  SOURCES test/test_turbo_parser.c test/test_turbo_parser_bdd.c
  LIBS turbo_parser TurboNet::TinyTest
  FOLDER turbo_parser)

