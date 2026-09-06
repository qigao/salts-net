set(MIME_TESTS
    test_mime_content_disposition
    test_mime_encoded_word
    test_mime_mhtml
    test_mime_parser
    test_mime_rfc2231
    test_mime_rfc2822
    test_mime_smime
    test_mime_utils)

foreach(TEST_NAME IN LISTS MIME_TESTS)
  add_executable(${TEST_NAME} ${TEST_NAME}.c)

  target_link_libraries(${TEST_NAME} mime_parser Salts::TinyTest)

  target_include_directories(${TEST_NAME}
                             PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

  set_target_properties(${TEST_NAME} PROPERTIES FOLDER
                                                "email/mime_parser/test")

  string(REGEX REPLACE "^test_(.*)$" "\\1_tests" TEST_RUN_NAME ${TEST_NAME})
  add_test(NAME ${TEST_RUN_NAME} COMMAND ${TEST_NAME})
endforeach()
