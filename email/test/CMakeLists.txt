foreach(test_name IN ITEMS test_email_smtp test_email_pop3 test_email_cnet
                           test_email_message test_email_integration)
  cmake_add_test(${test_name}
    SOURCES ${test_name}.c
    LIBS SaltsNet::Email Salts::TinyTest
    FOLDER email/test)
endforeach()
