set(TARGET_NAME SaltsEmail)

add_subdirectory(uri_parser)
add_subdirectory(mime_parser)

# ── Source files ───────────────────────────────────────────────────────
cmake_add_source(SOURCE_FILES
  DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ── Library target ─────────────────────────────────────────────────────
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})

cmake_config_target(${TARGET_NAME}
  ALIAS SaltsNet::Email
  FOLDER "email"
  EXPORT_NAME Email)

target_link_libraries(${TARGET_NAME}
  PUBLIC
    SaltsNet::MimeParser
    Salts::CNet)

target_include_directories(${TARGET_NAME}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

install(
  TARGETS ${TARGET_NAME}
  EXPORT SaltsNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/email
        DESTINATION include
        FILES_MATCHING PATTERN "*.h")

if(BUILD_TESTING)
  add_subdirectory(test)
endif()

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
