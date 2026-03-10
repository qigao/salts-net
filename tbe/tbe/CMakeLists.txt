set(TARGET_NAME tbe)

# =============================================================================
# re2c Generated Files
# =============================================================================

set(SCHEMA_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/schema_lexer.re)
set(SCHEMA_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/schema_lexer_gen.c)

add_custom_command(
  OUTPUT ${SCHEMA_LEXER_GEN}
  COMMAND ${RE2C_EXECUTABLE} -o ${SCHEMA_LEXER_GEN} ${SCHEMA_LEXER_RE}
  DEPENDS ${SCHEMA_LEXER_RE}
  COMMENT "Generating schema DSL lexer with re2c"
  VERBATIM)

# =============================================================================
# Lemon Generated Files
# =============================================================================

set(SCHEMA_GRAMMAR_Y ${CMAKE_CURRENT_SOURCE_DIR}/parser/schema_grammar.y)
set(SCHEMA_GRAMMAR_GEN ${CMAKE_CURRENT_BINARY_DIR}/schema_grammar_gen.c)
set(SCHEMA_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/schema_grammar_gen.h)

add_custom_command(
  OUTPUT ${SCHEMA_GRAMMAR_GEN} ${SCHEMA_GRAMMAR_H}
  COMMAND ${CMAKE_COMMAND} -E copy ${SCHEMA_GRAMMAR_Y}
          ${CMAKE_CURRENT_BINARY_DIR}/schema_grammar_gen.y
  COMMAND ${LEMON_EXECUTABLE} -T${LEMPAR}
          ${CMAKE_CURRENT_BINARY_DIR}/schema_grammar_gen.y
  DEPENDS ${SCHEMA_GRAMMAR_Y} ${LEMON_DEPENDS}
  COMMENT "Generating schema DSL parser with lemon"
  VERBATIM)

# =============================================================================
# Library
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)

add_library(
  ${TARGET_NAME} STATIC
  ${SOURCE_FILES} ${SCHEMA_LEXER_GEN} ${SCHEMA_GRAMMAR_GEN} ${SCHEMA_GRAMMAR_Y}
  ${SCHEMA_LEXER_RE})
add_library(TurboNet::SchemaBE ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/parser ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Utils  )

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER tbe/${TARGET_NAME}
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME SchemaBE)

# =============================================================================
# Tests
# =============================================================================

add_executable(test_tbe_parser test/test_tbe_parser.c)
target_link_libraries(test_tbe_parser PRIVATE TurboNet::SchemaBE
                                              TurboNet::TinyTest)
set_target_properties(test_tbe_parser PROPERTIES FOLDER tbe/${TARGET_NAME})
add_sanitizers(test_tbe_parser)
add_test(NAME test_tbe_parser COMMAND test_tbe_parser)

add_executable(test_flags_bdd test/test_flags_bdd.c)
target_link_libraries(test_flags_bdd PRIVATE TurboNet::SchemaBE
                                             TurboNet::TinyTest)
set_target_properties(test_flags_bdd PROPERTIES FOLDER tbe/tbe)
add_sanitizers(test_flags_bdd)
add_test(NAME test_flags_bdd COMMAND test_flags_bdd)

add_executable(test_tbe_parser_bdd test/test_tbe_parser_bdd.c)
target_link_libraries(test_tbe_parser_bdd PRIVATE TurboNet::SchemaBE
                                                  TurboNet::TinyTest)
set_target_properties(test_tbe_parser_bdd PROPERTIES FOLDER tbe/tbe)
add_sanitizers(test_tbe_parser_bdd)
add_test(NAME test_tbe_parser_bdd COMMAND test_tbe_parser_bdd)

add_executable(test_benchmark_schemas_bdd test/test_benchmark_schemas_bdd.c)
target_link_libraries(test_benchmark_schemas_bdd PRIVATE TurboNet::SchemaBE
                                                         TurboNet::TinyTest)
set_target_properties(test_benchmark_schemas_bdd PROPERTIES FOLDER tbe/tbe)
add_sanitizers(test_benchmark_schemas_bdd)
add_test(NAME test_benchmark_schemas_bdd COMMAND test_benchmark_schemas_bdd)

# =============================================================================
# Benchmarks
# =============================================================================

add_executable(benchmark_tbe_simple test/benchmark_tbe_simple.c)
target_link_libraries(benchmark_tbe_simple PRIVATE TurboNet::SchemaBE
                                                   TurboNet::TinyTest)
set_target_properties(benchmark_tbe_simple PROPERTIES FOLDER tbe/tbe)
add_sanitizers(benchmark_tbe_simple)
 

add_executable(benchmark_encoding test/benchmark_encoding.c)
target_link_libraries(benchmark_encoding PRIVATE TurboNet::TinyTest)
set_target_properties(benchmark_encoding PROPERTIES FOLDER tbe/tbe)
add_sanitizers(benchmark_encoding)

# =============================================================================
# TBE vs Native Struct Benchmark
# =============================================================================

set(BENCH_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/test/bench.schema)
set(BENCH_HEADER ${CMAKE_CURRENT_BINARY_DIR}/bench_data.h)
set(C_STRUCT_TEMPLATE ${CMAKE_SOURCE_DIR}/tbe/tbe_compiler/templates/c_structs.mustache)

add_custom_command(
  OUTPUT ${BENCH_HEADER}
  COMMAND tbe_compiler --schema ${BENCH_SCHEMA}
                       --template ${C_STRUCT_TEMPLATE}
                       --lang c
                       --output ${BENCH_HEADER}
  DEPENDS tbe_compiler ${BENCH_SCHEMA} ${C_STRUCT_TEMPLATE}
  COMMENT "Generating benchmark headers with tbe_compiler"
  VERBATIM)

 