# HTTP Client Tests
set(folder_name http/http_client/tests)

set(HTTP_TESTS
    test_basic
    test_requests
    test_parsing
    test_headers_auth
    test_stats_forms
    test_cookies
    test_multipart
    test_interceptors
    test_async_stream
    test_streaming_multipart
    test_retry
    test_resiliency
    test_coro_http)

foreach(test_name ${HTTP_TESTS})
  add_executable(${test_name} ${test_name}.c)
  target_link_libraries(${test_name} PRIVATE TurboNet::HttpCoroClient TurboNet::TinyTest TurboNet::JsonParser)
  set_target_properties(${test_name} PROPERTIES FOLDER ${folder_name})

  if(WIN32)
    target_compile_definitions(${test_name} PRIVATE WIN32_LEAN_AND_MEAN
                                                    _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${test_name} PRIVATE ws2_32 advapi32)
  endif()

  add_sanitizers(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
