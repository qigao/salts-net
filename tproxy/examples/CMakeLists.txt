set(folder_name ${TARGET_NAME}/examples)

file(GLOB EXTRA_SOURCES *.c)
foreach(test_source ${EXTRA_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE ${TARGET_NAME})
    set_target_properties(${test_name} PROPERTIES FOLDER ${folder_name})
endforeach()
