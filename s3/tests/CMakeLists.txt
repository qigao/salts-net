# Tests (TinyTest BDD)
add_executable(test_s3_client test_s3_client.c)
target_link_libraries(test_s3_client PRIVATE TurboNet::S3
                                                TurboNet::TinyTest)
set_target_properties(test_s3_client PROPERTIES FOLDER "s3/tests")
add_test(NAME test_s3_client COMMAND test_s3_client)

add_executable(test_xml_s3 test_xml_s3.c)
target_link_libraries(test_xml_s3 PRIVATE TurboNet::S3
                                            TurboNet::TinyTest)
set_target_properties(test_xml_s3 PROPERTIES FOLDER "s3/tests")
add_test(NAME test_xml_s3 COMMAND test_xml_s3)

add_executable(test_s3_multipart test_s3_multipart.c)
target_link_libraries(test_s3_multipart PRIVATE TurboNet::S3
                                                   TurboNet::TinyTest)
set_target_properties(test_s3_multipart PROPERTIES FOLDER "s3/tests")
add_test(NAME test_s3_multipart COMMAND test_s3_multipart)
