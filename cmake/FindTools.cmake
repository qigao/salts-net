if(NOT DEFINED ENV{RE2C_ROOT} OR NOT IS_DIRECTORY "$ENV{RE2C_ROOT}")
    message(FATAL_ERROR
        "RE2C_ROOT must name the host tools directory from Qigao.Re2c.Binary. "
        "Run cmake/ci/restore-native-sdk.ps1 to restore it from GitHub Packages.")
endif()
unset(RE2C_EXECUTABLE CACHE)
unset(RE2C_EXECUTABLE)
find_program(RE2C_EXECUTABLE re2c
             PATHS "$ENV{RE2C_ROOT}/bin"
             NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)

# Both native and cross builds consume the target selected by tools/lemon.
if(NOT TARGET lemon)
    message(FATAL_ERROR "The required lemon target is missing")
endif()
set(LEMON_EXECUTABLE $<TARGET_FILE:lemon>)
set(LEMON_DEPENDS lemon)
set(LEMPAR "${PROJECT_SOURCE_DIR}/tools/lemon/lempar.c" CACHE FILEPATH
    "Path to the matching Lemon parser template")
if(NOT EXISTS "${LEMPAR}" OR IS_DIRECTORY "${LEMPAR}")
    message(FATAL_ERROR "The required Lemon template is missing: ${LEMPAR}")
endif()
