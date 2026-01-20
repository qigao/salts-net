set(TARGET_NAME snmp_parser)
add_subdirectory(asn1)

# =============================================================================
# SNMP Parser Library (respects BUILD_SHARED_LIBS)
# =============================================================================

# Source files
set(SOURCES src/snmp_parser.c src/snmp_builder.c src/snmp_client.c
            src/snmp_usm.c)

# Headers
set(HEADERS include/snmp_types.h include/snmp_parser.h include/snmp_builder.h
            include/snmp_client.h include/snmp_usm.h)

# Library
add_library(${TARGET_NAME} SHARED ${SOURCES} ${HEADERS})
add_library(TurboNet::SNMP ALIAS ${TARGET_NAME})
target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)

# Link dependencies
find_package(OpenSSL REQUIRED)

target_link_libraries(
  ${TARGET_NAME}
  PUBLIC TurboNet::Utils TurboNet::Core OpenSSL::Crypto
  PRIVATE asn1_parser)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER snmp/snmp
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME SNMPParser)

# Install
# install(
#     TARGETS ${TARGET_NAME}
#     EXPORT TurboNetTargets
#     LIBRARY DESTINATION lib
#     ARCHIVE DESTINATION lib
#     RUNTIME DESTINATION bin
# )

# install(
#     DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
#     DESTINATION include/snmp
#     FILES_MATCHING PATTERN "*.h"
# )

# =============================================================================
# Tests
# =============================================================================

if(BUILD_TESTING)
  add_subdirectory(test)
endif()

# =============================================================================
# Examples
# =============================================================================

add_executable(snmp_get_example examples/snmp_get_example.c)
target_link_libraries(snmp_get_example PRIVATE ${TARGET_NAME})
set_target_properties(snmp_get_example PROPERTIES FOLDER snmp/snmp/examples)

add_executable(snmp_walk_example examples/snmp_walk_example.c)
target_link_libraries(snmp_walk_example PRIVATE ${TARGET_NAME})
set_target_properties(snmp_walk_example PROPERTIES FOLDER snmp/snmp/examples)
