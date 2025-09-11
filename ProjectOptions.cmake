include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" OFF)

# SSL support
option(ENABLE_SSL "Enable SSL support" ON)
cmake_dependent_option(
    USE_OPENSSL "Use OpenSSL" ON "ENABLE_SSL;NOT USE_MBEDTLS" OFF
)
cmake_dependent_option(
    USE_MBEDTLS "Use MbedTLS" OFF "ENABLE_SSL;NOT USE_OPENSSL" OFF
)

if(ENABLE_SSL)
    if(USE_OPENSSL)
        set(SSL_BACKEND_USED "OpenSSL")
    elseif(USE_MBEDTLS)
        set(SSL_BACKEND_USED "MbedTLS")
    else()
        message(
            FATAL_ERROR
                "No valid SSL backend selected. Please enable either USE_OPENSSL or USE_MBEDTLS."
        )
    endif()
endif()
message(STATUS "SSL backend used: ${SSL_BACKEND_USED}")
# if(MSVC)
#     add_compile_options(/bigobj)
# endif()

# zlib support
option(ENABLE_ZLIB "Use zlib" ON)

option(BUILD_SHARED_LIBS "Build shared instead of static libraries." ON)
# Build options - unified and simplified
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_BENCHMARKS "Build performance benchmarks" ON)
option(BUILD_CPP_EXTENSIONS "Build C++ extensions (tlsnet-cpp)" ON)
option(BUILD_DOCS "Build documentation" OFF)

# TLSNet specific options
option(TLSNET_USE_OPENSSL "Use OpenSSL as TLS backend" ON)
option(TLSNET_ENABLE_SIMD "Enable SIMD optimizations" ON)
option(TLSNET_ENABLE_NUMA "Enable NUMA awareness" OFF)
option(TLSNET_ENABLE_MMAP "Enable memory mapping support" OFF)
option(TLSNET_ENABLE_SENDFILE "Enable sendfile support" OFF)

# Polly Plugin System Options
option(POLLY_ENABLE_LUA "Enable Lua plugin support" ON)
option(POLLY_ENABLE_WASM "Enable WebAssembly plugin support" ON)

# Polly Compilation Mode Options
option(POLLY_ENABLE_AOT "Enable Ahead-of-Time compilation support" ON)
option(POLLY_ENABLE_JIT "Enable Just-in-Time compilation support" ON)
 
# Compilation mode validation
if(NOT POLLY_ENABLE_AOT AND NOT POLLY_ENABLE_JIT)
    message(FATAL_ERROR "At least one compilation mode (AOT or JIT) must be enabled")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

find_package(Threads REQUIRED)
