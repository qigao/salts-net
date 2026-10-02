add_library(SaltsLB SHARED src/salts_lb.c)
cmake_config_api(SaltsLB SALTSNET_LB_C_API)
cmake_config_target(
  SaltsLB
  ALIAS SaltsNet::LB
  EXPORT_NAME LB
  FOLDER lb)

target_include_directories(
  SaltsLB
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(SaltsLB PUBLIC Salts::CNet PRIVATE Salts::Core)

install(
  TARGETS SaltsLB
  EXPORT SaltsNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
