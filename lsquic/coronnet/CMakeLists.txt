add_library(TurboNet_LSQUIC STATIC turbo_lsquic.c)
cmake_config_target(
  TurboNet_LSQUIC
  ALIAS TurboNet::LSQUIC
  FOLDER "LSQUIC"
  EXPORT_NAME LSQUIC)

target_include_directories(
  TurboNet_LSQUIC
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../CoroNet/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(TurboNet_LSQUIC PUBLIC TurboNet::CoroNet lsquic)

install(
  TARGETS TurboNet_LSQUIC
  EXPORT TurboNetTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(BUILD_EXAMPLES)
  add_executable(lsquic_echo_client_example lsquic_echo_client_example.c)
  target_link_libraries(lsquic_echo_client_example PRIVATE TurboNet_LSQUIC)
  cmake_config_target(lsquic_echo_client_example NO_INSTALL
                      FOLDER "LSQUIC/examples")

  if(WIN32)
    add_custom_target(
      test_lsquic_public_http3
      COMMAND
        powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy
        Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/test_public_http3.ps1"
        -Client "$<TARGET_FILE:lsquic_echo_client_example>"
      DEPENDS lsquic_echo_client_example
      USES_TERMINAL
      VERBATIM)
    set_target_properties(test_lsquic_public_http3
                          PROPERTIES FOLDER "LSQUIC/test")
  endif()
endif()

if(BUILD_TESTS AND BUILD_TESTING)
  cmake_add_test(
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_turbo_lsquic.c
    FOLDER "LSQUIC/test"
    LIBS TurboNet_LSQUIC Rocida::TinyTest
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../CoroNet/include)
endif()
