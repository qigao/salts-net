set(TARGET_NAME NetCore )

# Support both static and dynamic builds
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)

# Create library with type determined by BUILD_SHARED_LIBS
if(BUILD_SHARED_LIBS)
    add_library(${TARGET_NAME} SHARED ${SOURCE_FILES})
    # Windows DLL export/import
    if(WIN32)
        target_compile_definitions(${TARGET_NAME} PRIVATE TURBONET_BUILDING_DLL)
        target_compile_definitions(${TARGET_NAME} INTERFACE TURBONET_USING_DLL)
    endif()
else()
    add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})
endif()

add_library(TurboNet::Core ALIAS NetCore)

# Set target properties
set_target_properties(${TARGET_NAME} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Include directories
target_include_directories(${TARGET_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set(PICOTLS_ROOT ${EXTERNAL_ROOT}/picotls)
find_package(PicoTLS REQUIRED)
set(PICOQUIC_ROOT ${EXTERNAL_ROOT}/picoquic)
find_package(PicoQUIC REQUIRED)
# Link libraries - vcpkg style
target_link_libraries(${TARGET_NAME}
    PUBLIC
       $<IF:$<TARGET_EXISTS:mimalloc-static>,mimalloc-static,mimalloc>
        OpenSSL::SSL
        OpenSSL::Crypto 
        ZLIB::ZLIB
        kcp::kcp c-ares::cares
        libuv::uv clog TurboParser::URI
        picotls::picotls
        picoquic::picoquic
        Threads::Threads
 )

# Platform-specific fixes
if(WIN32)
    # Windows-specific compile definitions
    target_compile_definitions(${TARGET_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(${TARGET_NAME} PRIVATE ws2_32 advapi32)
endif()
 
# Export core headers for external users
set(TURBONET_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/turbonet.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/turbonet_fs.h  
    ${CMAKE_CURRENT_SOURCE_DIR}/include/turbonet_mdns.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/platform.h
)

# Set target properties
set_target_properties( ${TARGET_NAME} PROPERTIES 
    FOLDER  ${TARGET_NAME}
    # Library versioning
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    # Export headers
    PUBLIC_HEADER "${TURBONET_PUBLIC_HEADERS}"
)

 
add_subdirectory(examples) 

# Install configuration for external users
include(GNUInstallDirs)

# Install library and headers only (no complex export dependencies)
install(TARGETS ${TARGET_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbonet
)

# Create simple package config for external users
include(CMakePackageConfigHelpers)

# Create a basic config file for find_package() support
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/TurboNetConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/TurboNetConfig.cmake
    @ONLY
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/TurboNetConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

# Install config files
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/TurboNetConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/TurboNetConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TurboNet
)

# Add tests if enabled
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
 
 