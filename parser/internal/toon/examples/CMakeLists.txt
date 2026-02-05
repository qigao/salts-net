# Examples for toon library

file(GLOB EXAMPLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

foreach(EXAMPLE_SOURCE ${EXAMPLE_SOURCES})
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_SOURCE} NAME_WE)
    
    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SOURCE})
    target_link_libraries(${EXAMPLE_NAME} PRIVATE toon)
    
    if(UNIX)
        target_link_libraries(${EXAMPLE_NAME} PRIVATE m)
    endif()
    
    set_target_properties(${EXAMPLE_NAME} PROPERTIES FOLDER turbo_parser/parser/toon/examples)
endforeach()

# Copy .toon files to the target directory
file(GLOB TOON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.toon")
add_custom_target(copy_toon_examples ALL
    COMMENT "Copying .toon examples"
)
foreach(TOON_FILE ${TOON_FILES})
    add_custom_command(TARGET copy_toon_examples POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TOON_FILE}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/"
    )
endforeach()
