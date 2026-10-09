cmake_minimum_required(VERSION 3.27)
project(SaltsNetInstalledSDK C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

if(NOT DEFINED ENV{SALTSNET_ROOT} OR
   NOT IS_DIRECTORY "$ENV{SALTSNET_ROOT}")
  message(FATAL_ERROR "SALTSNET_ROOT must be an installed SaltsNet SDK prefix")
endif()
find_package(SaltsNet CONFIG REQUIRED
  PATHS "$ENV{SALTSNET_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)

foreach(component ICE LB TCPProxy)
  if(NOT TARGET "SaltsNet::${component}")
    message(FATAL_ERROR "Missing installed public target: SaltsNet::${component}")
  endif()
endforeach()

add_executable(installed_saltsnet_c11 c_client.c)
target_link_libraries(installed_saltsnet_c11 PRIVATE
  SaltsNet::ICE SaltsNet::LB SaltsNet::TCPProxy)
add_test(NAME installed_saltsnet_c11 COMMAND installed_saltsnet_c11)

add_executable(installed_saltsnet_cpp17 cpp_client.cpp)
target_link_libraries(installed_saltsnet_cpp17 PRIVATE
  SaltsNet::ICE SaltsNet::LB SaltsNet::TCPProxy)
add_test(NAME installed_saltsnet_cpp17 COMMAND installed_saltsnet_cpp17)
