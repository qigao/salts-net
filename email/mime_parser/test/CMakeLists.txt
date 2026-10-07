set(MIME_TESTS
    test_mime_content_disposition
    test_mime_encoded_word
    test_mime_mhtml
    test_mime_parser
    test_mime_rfc2231
    test_mime_rfc2822
    test_mime_smime
    test_mime_utils)

foreach(test_name IN LISTS MIME_TESTS)
  cmake_add_test(${test_name}
    SOURCES ${test_name}.c
    LIBS SaltsNet::MimeParser Salts::TinyTest
    FOLDER email/mime_parser/test)
endforeach()
