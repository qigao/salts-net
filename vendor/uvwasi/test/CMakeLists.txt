file(GLOB test_files "test-*.c")

foreach(file ${test_files})
    get_filename_component(test_name ${file} NAME_WE)
    add_executable(${test_name} ${file})
    add_test(NAME ${test_name} COMMAND ${test_name})
    
    target_include_directories(${test_name}
        PRIVATE
        ${PROJECT_SOURCE_DIR}/include)
    
    target_link_libraries(${test_name} PRIVATE uvwasi_a)
    
    set_target_properties(${test_name} PROPERTIES FOLDER "vendor/uvwasi/tests")
endforeach()
