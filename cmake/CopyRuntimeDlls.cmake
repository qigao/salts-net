if(NOT DEFINED RUNTIME_DESTINATION)
  message(FATAL_ERROR "RUNTIME_DESTINATION is required")
endif()

if(NOT DEFINED RUNTIME_DLLS OR RUNTIME_DLLS STREQUAL "")
  return()
endif()

string(REPLACE "@@" ";" runtime_dlls "${RUNTIME_DLLS}")
foreach(runtime_dll IN LISTS runtime_dlls)
  get_filename_component(runtime_dll_name "${runtime_dll}" NAME)
  file(
    COPY_FILE "${runtime_dll}"
    "${RUNTIME_DESTINATION}/${runtime_dll_name}"
    ONLY_IF_DIFFERENT)
endforeach()
