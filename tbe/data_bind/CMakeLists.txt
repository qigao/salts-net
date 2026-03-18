# =============================================================================
# Library
# =============================================================================

set(TARGET_NAME data_bind)

add_library(${TARGET_NAME} SHARED data_bind.c data_bind.h 
tbe_helpers.c mustache_node_driver.c mustache_node_driver.h
)
add_library(TurboNet::DataBind ALIAS ${TARGET_NAME})

target_compile_definitions(${TARGET_NAME} PRIVATE DATA_BIND_BUILD_DLL)

target_include_directories(${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
         $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_SOURCE_DIR}/tbe/tbe/include
    ${CMAKE_SOURCE_DIR}/vendor/mir
)

target_link_libraries(${TARGET_NAME}
  PRIVATE
    TurboNet::SchemaBE
    mir_static
  PUBLIC
    TurboNet::Utils
    TurboNet::Mustache
)

set_target_properties(${TARGET_NAME} PROPERTIES 
  FOLDER tbe/data_bind
  EXPORT_NAME DataBind
)

# =============================================================================
# Installation
# =============================================================================

install(TARGETS ${TARGET_NAME}
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

install(FILES data_bind.h DESTINATION include)

# =============================================================================
# Tests
# =============================================================================

add_executable(test_data_bind test_data_bind.c tbe_helpers.c)
target_link_libraries(test_data_bind
  PRIVATE 
    TurboNet::DataBind
    TurboNet::TinyTest
)
set_target_properties(test_data_bind PROPERTIES FOLDER tbe/data_bind)
add_test(NAME test_data_bind COMMAND test_data_bind)
