add_subdirectory(croar) 
add_subdirectory(reed)

set(CARES_STATIC ON CACHE BOOL "Build vendored c-ares as a static library" FORCE)
set(CARES_SHARED OFF CACHE BOOL "Disable the vendored c-ares shared library" FORCE)
set(CARES_STATIC_PIC ON CACHE BOOL "Build vendored c-ares with PIC" FORCE)
set(CARES_INSTALL OFF CACHE BOOL "Do not install vendored c-ares separately" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "Do not build vendored c-ares tests" FORCE)
set(CARES_BUILD_CONTAINER_TESTS OFF CACHE BOOL
    "Do not build vendored c-ares container tests" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "Do not build vendored c-ares tools" FORCE)
add_subdirectory(c-ares)

# Group all c-ares targets under vendor/c-ares in IDE folder view
foreach(_cares_target c-ares c-ares_static)
  if(TARGET ${_cares_target})
    set_target_properties(${_cares_target} PROPERTIES FOLDER "vendor/c-ares")
  endif()
endforeach()
add_subdirectory(turbo_crypto)
