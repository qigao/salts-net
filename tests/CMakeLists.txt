# Validate the selected prerelease SDK's installed C11 CNet strategy ABI.
add_executable(saltsnet_cnet23_strategy_test cnet_23_strategy_test.c)
target_link_libraries(saltsnet_cnet23_strategy_test PRIVATE Salts::CNet)
add_test(NAME saltsnet_cnet23_strategy_test COMMAND saltsnet_cnet23_strategy_test)

add_executable(saltsnet_sg_pipeline_test sg_pipeline_test.c)
target_link_libraries(saltsnet_sg_pipeline_test PRIVATE
  SaltsNet::LB SaltsNet::TCPProxy Salts::Core Salts::TinyTest)
if(WIN32)
  target_link_libraries(saltsnet_sg_pipeline_test PRIVATE ws2_32)
endif()
add_test(NAME saltsnet_sg_pipeline_test COMMAND saltsnet_sg_pipeline_test)
set_target_properties(saltsnet_sg_pipeline_test PROPERTIES FOLDER tests/integration)
set_tests_properties(saltsnet_sg_pipeline_test PROPERTIES TIMEOUT 30)
