# HTTP Client Examples

# Real example programs (files with main()).
set(HTTP_EXAMPLES
    http_resume_example
    asan_verify
    )

# Build each example
foreach(example_name ${HTTP_EXAMPLES})
  add_executable(${example_name} ${example_name}.c)
  target_link_libraries(${example_name} PRIVATE http_client)
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
