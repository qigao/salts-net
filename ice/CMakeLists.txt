# TurboNet ICE Module ICE/STUN/TURN implementation for NAT traversal (RFC
# 8445/5389/5766)
#
# This module provides general-purpose NAT traversal and can be used
# independently of WebRTC for P2P connectivity in any application.

cmake_minimum_required(VERSION 3.20)

message(STATUS "Building turbo_ice module (ICE/STUN/TURN NAT traversal)")

# ============================================================================
# turbo_ice - ICE/STUN/TURN library (respects BUILD_SHARED_LIBS)
# ============================================================================

add_library(turbo_ice src/turbo_stun.c src/turbo_ice.c src/turbo_turn.c
                      src/stb_sprintf.c)

add_library(TurboNet::Ice ALIAS turbo_ice)

target_include_directories(
  turbo_ice PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                   $<INSTALL_INTERFACE:include>)

target_link_libraries(turbo_ice PUBLIC TurboNet::Core OpenSSL::SSL
                                       OpenSSL::Crypto TurboNet::Utils)

if(WIN32)
  target_link_libraries(turbo_ice PUBLIC ws2_32 iphlpapi)
endif()

set_target_properties(
  turbo_ice
  PROPERTIES FOLDER ice
             EXPORT_NAME Ice
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

# ============================================================================
# Install
# ============================================================================

install(
  TARGETS turbo_ice
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include/
  FILES_MATCHING
  PATTERN "*.h")

# ============================================================================
# Tests
# ============================================================================

if(BUILD_TESTING)
  add_executable(test_stun tests/test_stun.c)
  target_link_libraries(test_stun PRIVATE turbo_ice unity)
  add_test(NAME stun COMMAND test_stun)
  set_target_properties(test_stun PROPERTIES FOLDER ice/tests)

  add_executable(test_ice tests/test_ice.c)
  target_link_libraries(test_ice PRIVATE turbo_ice unity)
  add_test(NAME ice COMMAND test_ice)
  set_target_properties(test_ice PROPERTIES FOLDER ice/tests)
endif()

# ============================================================================
# Examples
# ============================================================================

if(BUILD_TESTING)
  add_executable(stun_discovery examples/stun_discovery.c)
  target_link_libraries(stun_discovery PRIVATE turbo_ice)
  set_target_properties(stun_discovery PROPERTIES FOLDER ice/examples)

  add_executable(ice_gather examples/ice_gather.c)
  target_link_libraries(ice_gather PRIVATE turbo_ice)
  set_target_properties(ice_gather PROPERTIES FOLDER ice/examples)

  add_executable(turn_test examples/turn_test.c)
  target_link_libraries(turn_test PRIVATE turbo_ice)
  set_target_properties(turn_test PROPERTIES FOLDER ice/examples)
endif()
