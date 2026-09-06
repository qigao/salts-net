set(TEST_TARGET test_snmp)

add_executable(${TEST_TARGET}
    test_snmp.c
)

target_link_libraries(${TEST_TARGET} PRIVATE
    SaltsNet::SNMP
    Salts::TinyTest
)

add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})

set_target_properties(${TEST_TARGET} PROPERTIES
    FOLDER snmp/snmp
)

add_executable(test_snmp_cnet test_snmp_cnet.c)
target_link_libraries(test_snmp_cnet PRIVATE
    SaltsNet::SNMP
    Salts::Platform
    Salts::TinyTest
)
target_include_directories(test_snmp_cnet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(WIN32)
  target_link_libraries(test_snmp_cnet PRIVATE ws2_32)
endif()
add_test(NAME test_snmp_cnet COMMAND test_snmp_cnet)
set_target_properties(test_snmp_cnet PROPERTIES FOLDER snmp/snmp)

# SNMPv3 USM tests
set(TEST_V3_TARGET test_snmp_v3)

add_executable(${TEST_V3_TARGET}
    test_snmp_v3.c
)

target_link_libraries(${TEST_V3_TARGET} PRIVATE
    SaltsNet::SNMP
    asn1_parser
    Salts::TinyTest
)

add_test(NAME ${TEST_V3_TARGET} COMMAND ${TEST_V3_TARGET})

set_target_properties(${TEST_V3_TARGET} PROPERTIES
    FOLDER snmp/snmp
)

add_executable(test_snmp_public_header_cpp test_snmp_public_header.cpp)
target_link_libraries(test_snmp_public_header_cpp PRIVATE SaltsNet::SNMP)
add_test(NAME test_snmp_public_header_cpp COMMAND test_snmp_public_header_cpp)
set_target_properties(test_snmp_public_header_cpp PROPERTIES FOLDER snmp/snmp)

# ASN.1 Tests
add_executable(test_asn1_roundtrip test_asn1_roundtrip.c)
target_link_libraries(test_asn1_roundtrip PRIVATE asn1_parser Salts::TinyTest)
add_test(NAME test_asn1_roundtrip COMMAND test_asn1_roundtrip)
set_target_properties(test_asn1_roundtrip PROPERTIES FOLDER snmp/snmp)

add_executable(test_context_specific test_context_specific.c)
target_link_libraries(test_context_specific PRIVATE asn1_parser Salts::TinyTest)
add_test(NAME test_context_specific COMMAND test_context_specific)
set_target_properties(test_context_specific PROPERTIES FOLDER snmp/snmp)
