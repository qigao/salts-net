add_executable(salts_lb_example salts_lb_example.c)
target_link_libraries(salts_lb_example PRIVATE SaltsNet::LB)
set_target_properties(salts_lb_example PROPERTIES FOLDER lb/examples)

add_executable(salts_lb_worker salts_lb_worker.c)
target_link_libraries(salts_lb_worker PRIVATE Salts::CNet Salts::Core)
set_target_properties(salts_lb_worker PROPERTIES FOLDER lb/examples)
