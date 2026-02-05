
# =============================================================================
# TOON Tests
# =============================================================================

function(add_toon_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE toon unity)
    
    if(UNIX)
        target_link_libraries(${test_name} PRIVATE m)
    endif()
    
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_target_properties(${test_name} PROPERTIES FOLDER turbo_parser/parser/toon/tests)
endfunction()

add_toon_test(toon_debug_test debug_test.c)

add_toon_test(test_toonc test_toonc.c)

add_toon_test(benchmark_toon benchmark_toon.c)
