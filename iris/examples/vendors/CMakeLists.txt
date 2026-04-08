# Create iris_vendors library
add_library(iris_vendors STATIC 
    slugify.c
    session.c
)

target_include_directories(iris_vendors PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(iris_vendors 
    PUBLIC 
        TurboNet::Parser
    PRIVATE 
        iris
)

set_target_properties(iris_vendors PROPERTIES 
    FOLDER iris/examples/vendors
)
