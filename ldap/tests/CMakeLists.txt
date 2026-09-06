# LDAP Tests

# Unit tests
add_executable(test_ldap test_ldap.c)

target_link_libraries(test_ldap PRIVATE ldap_client SaltsNet::Asn1Parser
                                        Salts::Core Salts::TinyTest)

target_include_directories(test_ldap
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

add_test(NAME test_ldap COMMAND test_ldap)
set_target_properties(test_ldap PROPERTIES FOLDER ldap)

add_executable(test_ldap_cnet test_ldap_cnet.c)

target_link_libraries(test_ldap_cnet PRIVATE ldap_client Salts::Platform
                                               Salts::TinyTest)

target_include_directories(test_ldap_cnet
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

if(WIN32)
  target_link_libraries(test_ldap_cnet PRIVATE ws2_32)
endif()

add_test(NAME test_ldap_cnet COMMAND test_ldap_cnet)
set_target_properties(test_ldap_cnet PROPERTIES FOLDER ldap)

# Integration tests (requires network)
add_executable(test_ldap_integration test_ldap_integration.c)

target_link_libraries(test_ldap_integration PRIVATE ldap_client SaltsNet::Asn1Parser
                                                    Salts::Core Salts::TinyTest)

target_include_directories(test_ldap_integration
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(test_ldap_integration PROPERTIES FOLDER ldap)

# Don't add to CTest by default (requires network)
# add_test(NAME test_ldap_integration COMMAND test_ldap_integration)
