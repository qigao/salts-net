# HTTP Client Examples

# List of example programs
set(HTTP_EXAMPLES
    test_https
    chunked_response
    file_transfer_example
    file_transfer_stream_example
    advanced_features_example
    jwt_auth_example
    http_client_multithread
    asan_verify
    )

# Build each example
foreach(example_name ${HTTP_EXAMPLES})
  add_executable(${example_name} ${example_name}.c)
  target_link_libraries(${example_name} PRIVATE http_client TurboNet::TinyTest)
  set_target_properties(${example_name} PROPERTIES FOLDER http/examples)
  add_sanitizers(${example_name})
# Platform-specific settings
  if(WIN32)
      target_compile_definitions(${example_name} PRIVATE
          _CRT_SECURE_NO_WARNINGS
          WIN32_LEAN_AND_MEAN
      )
  endif()
endforeach()
