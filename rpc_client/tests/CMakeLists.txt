# RPC Client Tests

# Test: RPC Error
add_executable(test_rpc_error test_rpc_error.c)
target_link_libraries(test_rpc_error PRIVATE rpc_client http_client
                                             TurboNet::CoroNet TurboNet::TinyTest)
set_target_properties(test_rpc_error PROPERTIES FOLDER
                                                "http/rpc_client/tests")
add_test(NAME rpc_error COMMAND test_rpc_error)

# Test: RPC Stats
add_executable(test_rpc_stats test_rpc_stats.c)
target_link_libraries(test_rpc_stats PRIVATE rpc_client http_client
                                             TurboNet::CoroNet TurboNet::TinyTest)
set_target_properties(test_rpc_stats PROPERTIES FOLDER
                                                "http/rpc_client/tests")
add_test(NAME rpc_stats COMMAND test_rpc_stats)

# Test: RPC Retry
add_executable(test_rpc_retry test_rpc_retry.c)
target_link_libraries(test_rpc_retry PRIVATE rpc_client http_client
                                             TurboNet::CoroNet TurboNet::TinyTest)
set_target_properties(test_rpc_retry PROPERTIES FOLDER
                                                "http/rpc_client/tests")
add_test(NAME rpc_retry COMMAND test_rpc_retry)

# Integration Test: Ethereum JSON-RPC (Cloudflare ETH)
add_executable(test_eth_rpc_integration test_eth_rpc_integration.c)
target_link_libraries(
  test_eth_rpc_integration PRIVATE rpc_client http_client
                                   TurboNet::CoroNet TurboNet::TinyTest)
set_target_properties(test_eth_rpc_integration
                      PROPERTIES FOLDER "http/rpc_client/tests")
add_test(NAME eth_rpc_integration COMMAND test_eth_rpc_integration)

# Test: Ethereum RPC (BDD)
add_executable(test_eth_rpc_bdd test_eth_rpc_bdd.c)
target_link_libraries(
  test_eth_rpc_bdd PRIVATE rpc_client http_client
                           TurboNet::CoroNet TurboNet::TinyTest)
set_target_properties(test_eth_rpc_bdd
                      PROPERTIES FOLDER "http/rpc_client/tests")
add_test(NAME eth_rpc_bdd COMMAND test_eth_rpc_bdd)
