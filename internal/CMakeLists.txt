# Shared implementation only; no installed headers or public runtime target.
add_library(saltsnet_sg_host STATIC sg_host.c)
target_link_libraries(saltsnet_sg_host PRIVATE Salts::CNet Salts::Core)
target_include_directories(saltsnet_sg_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(saltsnet_sg_host PROPERTIES FOLDER internal C_VISIBILITY_PRESET hidden)
