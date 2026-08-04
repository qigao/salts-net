add_library(TurboNet_LSQUIC STATIC turbo_lsquic.c)
add_library(TurboNet::LSQUIC ALIAS TurboNet_LSQUIC)

target_include_directories(
  TurboNet_LSQUIC
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../CoroNet/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(TurboNet_LSQUIC PUBLIC TurboNet::CoroNet_static lsquic)

set_target_properties(TurboNet_LSQUIC PROPERTIES FOLDER "LSQUIC")

if(BUILD_TESTS AND BUILD_TESTING)
  cmake_add_test(
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_turbo_lsquic.c
    FOLDER "LSQUIC/test"
    LIBS TurboNet_LSQUIC TurboUtils::TinyTest TurboNet::CoroNet_static
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../CoroNet/include)
endif()
