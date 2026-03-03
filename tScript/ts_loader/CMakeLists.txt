set(TARGET_NAME ts_loader)

add_library(${TARGET_NAME} STATIC src/ts_plugin_loader.c)
add_library(TurboNet::TsLoader ALIAS ${TARGET_NAME})

target_include_directories(${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

# dlopen support on Linux/macOS
if(NOT WIN32)
  target_link_libraries(${TARGET_NAME} PUBLIC ${CMAKE_DL_LIBS})
endif()

set_target_properties(${TARGET_NAME} PROPERTIES
  FOLDER turbo_script
  EXPORT_NAME TsLoader)
