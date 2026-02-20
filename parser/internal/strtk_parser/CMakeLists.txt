set(TARGET_NAME strtk_parser)

# =============================================================================
# Library (Static)
# =============================================================================

file(GLOB_RECURSE SOURCE_FILES src/*.c)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})
add_library(TurboNet::strtkParser ALIAS ${TARGET_NAME})

target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME strtkParser)

# =============================================================================
# Tests
# =============================================================================

if(BUILD_TESTING)
  add_executable(test_${TARGET_NAME} test/test_strtk.c)
  target_link_libraries(test_${TARGET_NAME} PRIVATE ${TARGET_NAME} TurboNet::TinyTest)
  add_test(NAME test_${TARGET_NAME} COMMAND test_${TARGET_NAME})
  set_target_properties(test_${TARGET_NAME} PROPERTIES FOLDER turbo_parser/parser/${TARGET_NAME}/tests) 
endif()
