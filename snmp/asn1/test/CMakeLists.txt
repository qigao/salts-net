# Crypto Random tests
set(TEST_RANDOM_TARGET test_crypto_random)

add_executable(${TEST_RANDOM_TARGET} test_crypto_random.c)

target_link_libraries(${TEST_RANDOM_TARGET} PRIVATE SaltsNet::SNMPSupport Salts::TinyTest)

add_test(NAME ${TEST_RANDOM_TARGET} COMMAND ${TEST_RANDOM_TARGET})

set_target_properties(${TEST_RANDOM_TARGET} PROPERTIES FOLDER snmp/asn1)

# X.509 Integration tests
set(TEST_X509_INTEGRATION_TARGET test_x509_integration)

add_executable(${TEST_X509_INTEGRATION_TARGET} test_x509_integration.c)

target_link_libraries(${TEST_X509_INTEGRATION_TARGET} PRIVATE SaltsNet::SNMPSupport Salts::TinyTest)

add_test(NAME ${TEST_X509_INTEGRATION_TARGET}
         COMMAND ${TEST_X509_INTEGRATION_TARGET})

set_target_properties(${TEST_X509_INTEGRATION_TARGET}
                      PROPERTIES FOLDER snmp/asn1)
