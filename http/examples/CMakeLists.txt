# HTTP Client Examples
set(folder_name ${TARGET_NAME}/examples)

# List of example programs (coro client)
set(HTTP_EXAMPLES
    simple_get
    post_json
    custom_headers
    chunked_response
    test_https
    headers_example
    auth_example
    stats_example
    form_post_example
    cookies_example
    multipart_upload_example
    interceptors_example
    retry_example
    file_transfer_example
    advanced_features_example
    login_request
    asan_verify
    jwt_auth_example
    coro_http_example)

# Build each example
foreach(example_name ${HTTP_EXAMPLES})
  add_executable(${example_name} ${example_name}.c)
  target_link_libraries(${example_name} PRIVATE http_coro_client TurboNet::TinyTest)
  set_target_properties(${example_name} PROPERTIES FOLDER http/${folder_name})
  add_sanitizers(${example_name})
# Platform-specific settings
  if(WIN32)
      target_compile_definitions(${example_name} PRIVATE
          _CRT_SECURE_NO_WARNINGS
          WIN32_LEAN_AND_MEAN
      )
  endif()
endforeach()
