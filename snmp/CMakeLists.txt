set(TARGET_NAME SaltsSNMP)
add_subdirectory(asn1)

# =============================================================================
# SNMP Parser Library (respects BUILD_SHARED_LIBS)
# =============================================================================

# Source files
set(SOURCES src/snmp_parser.c src/snmp_builder.c src/snmp_client.c
            src/snmp_usm.c src/snmp_usm_wire.c)

# Headers
set(HEADERS include/salts_snmp.h include/snmp_types.h include/snmp_parser.h
            include/snmp_builder.h include/snmp_client.h include/snmp_usm.h
            include/snmp_api.h)

# Library
add_library(${TARGET_NAME} SHARED ${SOURCES} ${HEADERS})
add_library(SaltsNet::SNMP ALIAS ${TARGET_NAME})
cmake_config_api(${TARGET_NAME} SALTSNET_SNMP_C_API)
target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)

# Link dependencies
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Salts::Core Salts::CMeta
  PRIVATE Salts::CNet asn1_parser)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER snmp/snmp
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME SNMP)

# Install
install(
  TARGETS ${TARGET_NAME}
  EXPORT SaltsNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)
install(FILES ${HEADERS} DESTINATION include)

# =============================================================================
# Tests
# =============================================================================

if(BUILD_TESTING)
  add_subdirectory(test)
endif()

# =============================================================================
# Examples
# =============================================================================

if(BUILD_EXAMPLES)
  add_executable(snmp_get_example examples/snmp_get_example.c)
  target_link_libraries(snmp_get_example PRIVATE SaltsNet::SNMP)
  set_target_properties(snmp_get_example PROPERTIES FOLDER snmp/snmp/examples)

  add_executable(snmp_walk_example examples/snmp_walk_example.c)
  target_link_libraries(snmp_walk_example PRIVATE SaltsNet::SNMP)
  set_target_properties(snmp_walk_example PROPERTIES FOLDER snmp/snmp/examples)
endif()
