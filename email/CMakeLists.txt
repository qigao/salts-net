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

# ── Tests ──────────────────────────────────────────────────────────────
cmake_add_test(
  SOURCES
    test/test_email_smtp.c
    test/test_email_pop3.c
    test/test_email_cnet.c
    test/test_email_message.c
    test/test_email_integration.c
  LIBS   ${TARGET_NAME} Salts::TinyTest
  FOLDER "email/test")

# ── Examples ───────────────────────────────────────────────────────────
cmake_add_benchmark(
  SOURCES
    examples/example_email_send.c
    examples/example_email_receive.c
    examples/example_email_imap.c
  LIBS   ${TARGET_NAME}
  FOLDER "email/examples")
