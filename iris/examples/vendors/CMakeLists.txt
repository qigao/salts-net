# re2c generation for dotenv lexer
set(DOTENV_LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/dotenv_lexer.re)
set(DOTENV_LEXER_GEN ${CMAKE_CURRENT_BINARY_DIR}/dotenv_lexer_gen.c)

add_custom_command(
    OUTPUT ${DOTENV_LEXER_GEN}
    COMMAND ${RE2C_EXECUTABLE} -o ${DOTENV_LEXER_GEN} ${DOTENV_LEXER_RE}
    DEPENDS ${DOTENV_LEXER_RE} ${CMAKE_CURRENT_SOURCE_DIR}/dotenv_lexer.h
    COMMENT "Generating dotenv lexer with re2c"
    VERBATIM
)

# Create iris_vendors library
add_library(iris_vendors STATIC 
    dotenv.c 
    ${DOTENV_LEXER_GEN}
    slugify.c
    session.c
)

target_include_directories(iris_vendors PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(iris_vendors PRIVATE 
    iris
)

set_target_properties(iris_vendors PROPERTIES 
    FOLDER iris/examples/vendors
)
