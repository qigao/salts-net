# turl: Postman-like CLI for TurboNet
set(turl_name turl)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
     ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h*)
add_library(turl_lib STATIC ${SOURCE_FILES})

# Main Executable
add_executable(${turl_name} turl.c)

# Dependencies for the library
target_link_libraries(
  turl_lib
  PUBLIC 
         TurboNet::CoroNet
         TurboUtils::Mustache
         TurboUtils::Parser
         TurboUtils::Utils
         http_client
  PRIVATE 
         cjwt 
         )  

target_include_directories(
  turl_lib
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_SOURCE_DIR}/parser/internal/dotenv_parser/include)

target_link_libraries(${turl_name} PRIVATE turl_lib)
set_target_properties( turl_lib  PROPERTIES FOLDER http/tools)
# Standard project setup
set_target_properties(${turl_name} PROPERTIES FOLDER http/tools)

# Unit Testing
if(BUILD_TESTS)
  add_executable(test_turl test/test_turl.c)
  target_link_libraries(test_turl PRIVATE turl_lib TurboUtils::TinyTest)
  target_include_directories(test_turl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(test_turl PROPERTIES FOLDER http/tools/test)
  add_test(NAME test_turl COMMAND test_turl)

  add_executable(test_turl_integration test/test_turl_integration.c)
  target_link_libraries(test_turl_integration PRIVATE turl_lib TurboUtils::TinyTest)
  target_include_directories(test_turl_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(test_turl_integration PROPERTIES FOLDER http/tools/test)
  # Do not add to 'add_test' by default as it requires internet
endif()

# Installation
install(TARGETS ${turl_name} RUNTIME DESTINATION bin)
