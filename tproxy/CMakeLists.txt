set(TARGET_NAME TProxy)

# Get all source files
file(GLOB_RECURSE HEADER_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

# Build SHARED library
add_library(${TARGET_NAME} SHARED ${HEADER_FILES} ${SOURCE_FILES})
add_library(TurboNet::TProxy ALIAS ${TARGET_NAME})
target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)

# Set target properties
set_target_properties(
  ${TARGET_NAME} PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION
                                                       ${PROJECT_VERSION_MAJOR}
                                                       FOLDER "TProxy")

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../netcore/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../common/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../vendor/memory/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/clib/src)

# Link libraries
target_link_libraries(
  ${TARGET_NAME}
  PUBLIC TurboNet::Core
     OpenSSL::SSL OpenSSL::Crypto)

 
# Build tests
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Build examples
if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
