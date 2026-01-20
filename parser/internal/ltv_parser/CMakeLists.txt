set(TARGET_NAME ltv_parser)

file(GLOB_RECURSE SOURCE_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)

add_library(${TARGET_NAME} STATIC ${SOURCE_FILES})
add_library(TurboNet::LtvParser ALIAS ${TARGET_NAME})

target_include_directories(${TARGET_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<INSTALL_INTERFACE:include/ltv_parser>
)

set_target_properties(${TARGET_NAME} PROPERTIES
    FOLDER turbo_parser/parser/ltv_parser/
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME LtvParser
)
target_link_libraries(${TARGET_NAME}
    PUBLIC
        TurboNet::UriParser
)
 

add_subdirectory(test)
