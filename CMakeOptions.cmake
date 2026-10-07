set(CMAKE_COLOR_DIAGNOSTICS ON)

# BUILD_TESTING is provided by CTest and is the only test switch.
# Sanitizer switches are defined by cmake/Sanitizers.cmake.

option(BUILD_EXAMPLES "Build example programs" ON)
set(LEMON_EXECUTABLE "" CACHE FILEPATH
    "Host Lemon executable; required while cross-compiling")
