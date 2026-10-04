set(TARGET_NAME mime_parser)

# ── Source files ───────────────────────────────────────────────────────
cmake_add_source(SOURCE_FILES DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src
                 ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ── Library target ─────────────────────────────────────────────────────
add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})

cmake_config_target(
  ${TARGET_NAME}
  ALIAS
  SaltsNet::MimeParser
  FOLDER
  "email/${TARGET_NAME}"
  EXPORT_NAME
  MimeParser)

target_link_libraries(
  ${TARGET_NAME}
  PUBLIC Salts::Core
  PRIVATE uri_parser SaltsNet::Asn1Parser)

target_include_directories(
  ${TARGET_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                        $<INSTALL_INTERFACE:include>)

install(
  TARGETS ${TARGET_NAME}
  EXPORT SaltsNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION include
  FILES_MATCHING
  PATTERN "*.h")

# ── Tests ──────────────────────────────────────────────────────────────
cmake_add_test(
  SOURCES
  test/test_mime_parser.c
  test/test_mime_utils.c
  test/test_mime_content_disposition.c
  test/test_mime_encoded_word.c
  test/test_mime_rfc2231.c
  test/test_mime_rfc2822.c
  test/test_mime_smime.c
  test/test_mime_mhtml.c
  LIBS
  ${TARGET_NAME}
  Salts::TinyTest
  FOLDER
  "email/${TARGET_NAME}/test")
