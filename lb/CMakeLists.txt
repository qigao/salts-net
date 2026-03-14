set(TARGET_NAME TurboLB)

# Get all source files
file(GLOB_RECURSE HEADER_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

# Build SHARED library
add_library(${TARGET_NAME} SHARED ${HEADER_FILES} ${SOURCE_FILES})
add_library(TurboNet::LB ALIAS ${TARGET_NAME})
target_compile_definitions(${TARGET_NAME} PRIVATE SHARED_CXX)

# Set target properties
set_target_properties(
  ${TARGET_NAME}
  PROPERTIES VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             FOLDER "TurboLB")

# Include directories
target_include_directories(
  ${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../CoroNet/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../common/include>)

# Link libraries
target_link_libraries(${TARGET_NAME} PUBLIC TurboNet::Core)

add_subdirectory(tests)

add_subdirectory(examples)
