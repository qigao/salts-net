add_library(SaltsTCPProxy SHARED src/salts_tcp_proxy.c src/salts_tcp_proxy_sg.c)
cmake_config_api(SaltsTCPProxy SALTSNET_TCP_PROXY_C_API)
cmake_config_target(
  SaltsTCPProxy
  ALIAS SaltsNet::TCPProxy
  EXPORT_NAME TCPProxy
  FOLDER tproxy)

target_include_directories(
  SaltsTCPProxy
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(SaltsTCPProxy PUBLIC Salts::CNet Salts::CMeta PRIVATE Salts::Core saltsnet_sg_host)

install(
  TARGETS SaltsTCPProxy
  EXPORT SaltsNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)

if(BUILD_TESTING OR BUILD_BENCHMARKS)
  add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
