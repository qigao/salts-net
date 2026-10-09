# Validate the selected prerelease SDK's installed C11 CNet strategy ABI.
add_executable(saltsnet_cnet23_strategy_test cnet_23_strategy_test.c)
target_link_libraries(saltsnet_cnet23_strategy_test PRIVATE Salts::CNet)
add_test(NAME saltsnet_cnet23_strategy_test COMMAND saltsnet_cnet23_strategy_test)
