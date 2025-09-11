 file(GLOB EXAMPLE_SOURCES *.c) 
 

foreach(example_source ${EXAMPLE_SOURCES})
    get_filename_component(example_name ${example_source} NAME_WE)
    set(EXAMPLE_NAME ${example_name})

    add_executable(${EXAMPLE_NAME} ${example_source})

    target_link_libraries(
        ${EXAMPLE_NAME}
        PRIVATE   ${TARGET_NAME}
    )

    target_include_directories(${EXAMPLE_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    set_target_properties(${EXAMPLE_NAME} PROPERTIES FOLDER   ${TARGET_NAME}/examples)

endforeach()
