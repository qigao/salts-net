include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# BUILD_TESTING is provided by CTest and is the only test switch.
# Sanitizer switches are defined by cmake/Sanitizers.cmake.

# if(MSVC) add_compile_options(/bigobj) endif()

option(BUILD_EXAMPLES "Build example programs" ON)
cmake_dependent_option(BUILD_BENCHMARKS "Build benchmark executables" ON
                       "BUILD_TESTING" OFF)


set_property(GLOBAL PROPERTY USE_FOLDERS ON)
