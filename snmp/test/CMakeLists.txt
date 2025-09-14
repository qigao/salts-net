set(TEST_TARGET test_snmp)

add_executable(${TEST_TARGET}
    test_snmp.c
)

target_link_libraries(${TEST_TARGET} PRIVATE
    snmp_parser
    unity
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
    unity
)

add_test(NAME ${TEST_V3_TARGET} COMMAND ${TEST_V3_TARGET})

set_target_properties(${TEST_V3_TARGET} PROPERTIES
    FOLDER snmp/snmp
)