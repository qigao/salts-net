add_executable(stricks_test stricks_test.c)
target_link_libraries(stricks_test PRIVATE stricks TurboNet::TinyTest)

add_test(NAME stricks_test COMMAND stricks_test)

set_target_properties(stricks_test PROPERTIES FOLDER "vendor/stricks")
