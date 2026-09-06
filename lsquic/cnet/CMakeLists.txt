add_library(SaltsLSQUIC STATIC salts_lsquic.c)
cmake_config_target(
  SaltsLSQUIC
  ALIAS SaltsNet::LSQUIC
  EXPORT_NAME LSQUIC
  FOLDER LSQUIC)

target_include_directories(
  SaltsLSQUIC
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(SaltsLSQUIC PUBLIC Salts::CNet lsquic PRIVATE Salts::Core)

install(
  TARGETS SaltsLSQUIC
  EXPORT SaltsNetTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES salts_lsquic.h salts_lsquic_api.h DESTINATION include)

if(BUILD_EXAMPLES)
  add_executable(salts_lsquic_handshake_example salts_lsquic_handshake_example.c)
  target_link_libraries(salts_lsquic_handshake_example PRIVATE SaltsNet::LSQUIC Salts::Core)
  cmake_copy_runtime_dlls(salts_lsquic_handshake_example)
  set_target_properties(salts_lsquic_handshake_example PROPERTIES FOLDER LSQUIC/examples)
endif()

if(WIN32)
  get_target_property(_saltsnet_zlib_import ZLIB::ZLIB IMPORTED_LOCATION_RELEASE)
  if(NOT _saltsnet_zlib_import)
    get_target_property(_saltsnet_zlib_import ZLIB::ZLIB IMPORTED_LOCATION)
  endif()
  get_filename_component(_saltsnet_zlib_lib_dir "${_saltsnet_zlib_import}" DIRECTORY)
  get_filename_component(_saltsnet_zlib_prefix "${_saltsnet_zlib_lib_dir}" DIRECTORY)
  set(_saltsnet_zlib_runtime "${_saltsnet_zlib_prefix}/bin/zlib1.dll")
  if(NOT EXISTS "${_saltsnet_zlib_runtime}")
    message(FATAL_ERROR "SALTSNET_ENABLE_LSQUIC requires the zlib runtime DLL")
  endif()
  install(FILES "${_saltsnet_zlib_runtime}" DESTINATION bin)
  if(TARGET salts_lsquic_handshake_example)
    add_custom_command(
      TARGET salts_lsquic_handshake_example
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_saltsnet_zlib_runtime}"
              "$<TARGET_FILE_DIR:salts_lsquic_handshake_example>/zlib1.dll")
  endif()
endif()

if(BUILD_TESTS AND BUILD_TESTING)
  add_executable(test_salts_lsquic test_salts_lsquic.c)
  target_include_directories(test_salts_lsquic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_salts_lsquic PRIVATE SaltsNet::LSQUIC Salts::TinyTest Salts::Core)
  if(WIN32)
    target_link_libraries(test_salts_lsquic PRIVATE ws2_32)
  endif()
  cmake_copy_runtime_dlls(test_salts_lsquic)
  if(WIN32)
    add_custom_command(
      TARGET test_salts_lsquic
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_saltsnet_zlib_runtime}"
              "$<TARGET_FILE_DIR:test_salts_lsquic>/zlib1.dll")
  endif()
  add_test(NAME salts_lsquic COMMAND test_salts_lsquic)
  set_target_properties(test_salts_lsquic PROPERTIES FOLDER LSQUIC/tests)
endif()
