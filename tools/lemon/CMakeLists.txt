if(CMAKE_CROSSCOMPILING OR LEMON_EXECUTABLE)
  if(NOT LEMON_EXECUTABLE OR NOT EXISTS "${LEMON_EXECUTABLE}" OR
     IS_DIRECTORY "${LEMON_EXECUTABLE}")
    message(FATAL_ERROR
            "LEMON_EXECUTABLE must name the Lemon executable from a completed host preset build")
  endif()
  add_executable(lemon IMPORTED GLOBAL)
  set_target_properties(lemon PROPERTIES IMPORTED_LOCATION "${LEMON_EXECUTABLE}")
else()
  add_executable(lemon lemon.c)
  target_compile_features(lemon PRIVATE c_std_11)
  set_target_properties(lemon PROPERTIES FOLDER tools)
endif()
add_executable(SaltsNet::Lemon ALIAS lemon)
