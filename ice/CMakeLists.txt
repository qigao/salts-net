# TurboNet ICE Module ICE/STUN/TURN implementation for NAT traversal (RFC
# 8445/5389/5766)
#
# This module provides general-purpose NAT traversal and can be used
# independently of WebRTC for P2P connectivity in any application.
# ============================================================================
# TurboNet::Ice - ICE/STUN/TURN library
# ============================================================================

add_library(turbo_ice SHARED src/turbo_stun.c src/turbo_ice.c src/turbo_turn.c)
cmake_config_api(turbo_ice TURBONET_ICE_C_API)
cmake_config_target(
  turbo_ice
  ALIAS TurboNet::Ice
  FOLDER ice
  EXPORT_NAME Ice)

target_include_directories(
  turbo_ice
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  turbo_ice
  PUBLIC TurboNet::CoroNet
  PRIVATE OpenSSL::SSL OpenSSL::Crypto TurboUtils::Core)
if(WIN32)
  target_link_libraries(turbo_ice PRIVATE ws2_32 iphlpapi psapi)
endif()

install(
  TARGETS turbo_ice
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ice)

# ============================================================================
# Tests
# ============================================================================

if(BUILD_TESTS)
  add_executable(test_stun tests/test_stun.c)
  target_link_libraries(test_stun PRIVATE turbo_ice TurboUtils::TinyTest)
  add_test(NAME stun COMMAND test_stun)
  set_target_properties(test_stun PROPERTIES FOLDER ice/tests)

  add_executable(test_turn tests/test_turn.c)
  target_link_libraries(test_turn PRIVATE turbo_ice TurboUtils::TinyTest)
  add_test(NAME turn COMMAND test_turn)
  set_target_properties(test_turn PROPERTIES FOLDER ice/tests)

  add_executable(test_ice tests/test_ice.c)
  target_link_libraries(test_ice PRIVATE turbo_ice TurboUtils::TinyTest)
  if(WIN32)
    target_link_libraries(test_ice PRIVATE ws2_32)
  endif()
  add_test(NAME ice COMMAND test_ice)
  set_target_properties(test_ice PROPERTIES FOLDER ice/tests)
endif()

# ============================================================================
# Examples
# ============================================================================

if(BUILD_EXAMPLES)
  add_executable(stun_discovery examples/stun_discovery.c)
  target_link_libraries(stun_discovery PRIVATE turbo_ice)
  set_target_properties(stun_discovery PROPERTIES FOLDER ice/examples)

  add_executable(ice_gather examples/ice_gather.c)
  target_link_libraries(ice_gather PRIVATE turbo_ice)
  set_target_properties(ice_gather PROPERTIES FOLDER ice/examples)

  add_executable(stun_service examples/stun_service.c)
  target_link_libraries(stun_service PRIVATE turbo_ice)
  if(WIN32)
    target_link_libraries(stun_service PRIVATE ws2_32)
  endif()
  set_target_properties(stun_service PROPERTIES FOLDER ice/examples)

  add_executable(turn_test examples/turn_test.c)
  target_link_libraries(turn_test PRIVATE turbo_ice)
  set_target_properties(turn_test PROPERTIES FOLDER ice/examples)
endif()
