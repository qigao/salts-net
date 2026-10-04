# SaltsNet ICE Module ICE/STUN/TURN implementation for NAT traversal (RFC
# 8445/5389/5766)
#
# This module provides general-purpose NAT traversal and can be used
# independently of WebRTC for P2P connectivity in any application.
# ============================================================================
# SaltsNet::ICE - ICE/STUN/TURN library
# ============================================================================

add_library(SaltsICE SHARED src/ice_cnet_datagram.c src/salts_stun.c src/salts_ice.c
                            src/salts_turn.c)
cmake_config_api(SaltsICE SALTSNET_ICE_C_API)
cmake_config_target(
  SaltsICE
  ALIAS SaltsNet::ICE
  FOLDER ice
  EXPORT_NAME ICE)

target_include_directories(
  SaltsICE
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  SaltsICE
  PUBLIC Salts::CNet Salts::CMeta
  PRIVATE Salts::Core Threads::Threads)
if(WIN32)
  target_link_libraries(SaltsICE PRIVATE ws2_32 iphlpapi psapi)
endif()

install(
  TARGETS SaltsICE
  EXPORT SaltsNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ice)

# ============================================================================
# Tests
# ============================================================================

if(BUILD_TESTING)
  add_executable(test_stun_cnet tests/test_stun_cnet.c src/ice_cnet_datagram.c src/salts_stun.c)
  target_include_directories(test_stun_cnet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                                    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_stun_cnet PRIVATE Salts::CNet Salts::Core Salts::TinyTest)
  if(WIN32)
    target_link_libraries(test_stun_cnet PRIVATE ws2_32)
  endif()
  add_test(NAME stun_cnet COMMAND test_stun_cnet)
  set_target_properties(test_stun_cnet PROPERTIES FOLDER ice/tests)

  add_executable(test_stun tests/test_stun.c src/ice_cnet_datagram.c src/salts_stun.c)
  target_include_directories(test_stun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                               ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_stun PRIVATE Salts::CNet Salts::Core Salts::TinyTest)
  if(WIN32)
    target_link_libraries(test_stun PRIVATE ws2_32)
  endif()
  add_test(NAME stun COMMAND test_stun)
  set_target_properties(test_stun PROPERTIES FOLDER ice/tests)

  add_executable(test_turn tests/test_turn.c src/ice_cnet_datagram.c src/salts_stun.c
                                       src/salts_turn.c)
  target_include_directories(test_turn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
                                               ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_turn PRIVATE Salts::CNet Salts::Core Salts::TinyTest)
  if(WIN32)
    target_link_libraries(test_turn PRIVATE ws2_32)
  endif()
  add_test(NAME turn COMMAND test_turn)
  set_target_properties(test_turn PROPERTIES FOLDER ice/tests)

  add_executable(test_ice tests/test_ice.c)
  target_include_directories(test_ice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_ice PRIVATE SaltsNet::ICE Salts::TinyTest)
  if(WIN32)
    target_link_libraries(test_ice PRIVATE ws2_32)
  endif()
  add_test(NAME ice COMMAND test_ice)
  set_target_properties(test_ice PROPERTIES FOLDER ice/tests)

  add_executable(test_ice_public_header tests/test_ice_public_header.cpp)
  target_link_libraries(test_ice_public_header PRIVATE SaltsNet::ICE)
  add_test(NAME ice_public_header COMMAND test_ice_public_header)
  set_target_properties(test_ice_public_header PROPERTIES FOLDER ice/tests)
endif()

# ============================================================================
# Examples
# ============================================================================

if(BUILD_EXAMPLES)
  add_executable(stun_discovery examples/stun_discovery.c)
  target_link_libraries(stun_discovery PRIVATE SaltsNet::ICE)
  set_target_properties(stun_discovery PROPERTIES FOLDER ice/examples)

  add_executable(ice_gather examples/ice_gather.c)
  target_link_libraries(ice_gather PRIVATE SaltsNet::ICE)
  set_target_properties(ice_gather PROPERTIES FOLDER ice/examples)

  add_executable(stun_service examples/stun_service.c src/ice_cnet_datagram.c)
  target_include_directories(stun_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(stun_service PRIVATE SaltsNet::ICE)
  if(WIN32)
    target_link_libraries(stun_service PRIVATE ws2_32)
  endif()
  set_target_properties(stun_service PROPERTIES FOLDER ice/examples)

  add_executable(turn_test examples/turn_test.c)
  target_link_libraries(turn_test PRIVATE SaltsNet::ICE)
  set_target_properties(turn_test PROPERTIES FOLDER ice/examples)
endif()
