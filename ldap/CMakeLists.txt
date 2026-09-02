set(TARGET_NAME ldap_client)

# Source files
file(GLOB_RECURSE SOURCES src/*.c)

# Headers
file(GLOB_RECURSE HEADERS include/*.h)

# Library
add_library(${TARGET_NAME} SHARED ${SOURCES} ${HEADERS})
add_library(TurboNet::LDAP ALIAS ${TARGET_NAME})
cmake_config_api(${TARGET_NAME} TURBONET_LDAP_C_API)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)

# Link dependencies
find_package(OpenSSL REQUIRED)

target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Rocida::Core TurboNet::CoroNet OpenSSL::Crypto
  PRIVATE TurboNet::Asn1Parser)

set_target_properties(
  ${TARGET_NAME}
  PROPERTIES FOLDER ldap
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             EXPORT_NAME LDAPClient)

# Install
install(
  TARGETS ${TARGET_NAME}
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include/ldap
  FILES_MATCHING
  PATTERN "*.h")

# Tests
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
