set(TEST_TARGET test_snmp)

add_executable(${TEST_TARGET}
    test_snmp.c
)

target_link_libraries(${TEST_TARGET} PRIVATE
    snmp_parser
    TurboNet::TinyTest
)

add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})

set_target_properties(${TEST_TARGET} PROPERTIES
    FOLDER snmp/snmp
)

# SNMPv3 USM tests
set(TEST_V3_TARGET test_snmp_v3)

add_executable(${TEST_V3_TARGET}
    test_snmp_v3.c
)

target_link_libraries(${TEST_V3_TARGET} PRIVATE
    snmp_parser
    TurboNet::TinyTest
)

add_test(NAME ${TEST_V3_TARGET} COMMAND ${TEST_V3_TARGET})

set_target_properties(${TEST_V3_TARGET} PROPERTIES
    FOLDER snmp/snmp
)

# ASN.1 Tests
add_executable(test_asn1_roundtrip test_asn1_roundtrip.c)
target_link_libraries(test_asn1_roundtrip PRIVATE asn1_parser TurboNet::TinyTest)
add_test(NAME test_asn1_roundtrip COMMAND test_asn1_roundtrip)
set_target_properties(test_asn1_roundtrip PROPERTIES FOLDER snmp/snmp)

add_executable(test_context_specific test_context_specific.c)
target_link_libraries(test_context_specific PRIVATE asn1_parser TurboNet::TinyTest)
add_test(NAME test_context_specific COMMAND test_context_specific)
set_target_properties(test_context_specific PROPERTIES FOLDER snmp/snmp)