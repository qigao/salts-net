set(TARGET_NAME parrser_uri)
find_package(RE2C REQUIRED)

re2c_target(
    MyLexer ${CMAKE_CURRENT_SOURCE_DIR}/parser/lexer.re
    ${CMAKE_CURRENT_BINARY_DIR}/lexer.c
)

# Find all files recursively
file(
    GLOB_RECURSE
    SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c*
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h* 
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/*.re
)

# Libary part
add_library(${TARGET_NAME} STATIC)
add_library(TurboParser::URI  ALIAS parrser_uri )

target_sources(${TARGET_NAME} PRIVATE ${SOURCE_FILES}  ${CMAKE_CURRENT_BINARY_DIR}/lexer.c)

 
target_include_directories(${TARGET_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${TARGET_NAME})
# tests
if(ENABLE_TESTS)
    add_subdirectory(test)
endif()
