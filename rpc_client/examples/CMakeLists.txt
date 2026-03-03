# RPC Client Examples

# Ethereum JSON-RPC example using Public Node endpoint
add_executable(rpc_eth_example eth_rpc_example.c)

target_link_libraries(rpc_eth_example PRIVATE rpc_client http_client
                                               TurboNet::Core)

set_target_properties(
  rpc_eth_example PROPERTIES FOLDER "http/rpc_client/examples"
                             OUTPUT_NAME "eth_rpc_example")
