set(folder_name ${TARGET_NAME}/examples)

file(GLOB EXTRA_SOURCES *.c)
foreach(test_source ${EXTRA_SOURCES})
  get_filename_component(test_name ${test_source} NAME_WE)
  if(test_name STREQUAL "sync_client")
    add_executable(sync_client ${CMAKE_CURRENT_SOURCE_DIR}/sync_client.c)
  else()
    add_executable(${test_name} ${test_source})
  endif()

  target_link_libraries(${test_name} PRIVATE ${TARGET_NAME} TurboUtils::TinyTest)

  # Certificate-generation examples need ASN.1/X.509 helpers.
  if(test_name STREQUAL "test_ed25519_minimal"
     OR test_name STREQUAL "coro_tls_cert_demo_ed25519"
     OR test_name STREQUAL "coro_tls_cert_demo_simple")
    target_link_libraries(${test_name} PRIVATE TurboNet::Asn1Parser)
  endif()

  set_target_properties(${test_name} PROPERTIES FOLDER ${folder_name})
endforeach()
