# turl: Postman-like CLI for TurboNet
set(turl_name turl)

# Sources
set(turl_srcs 
    turl.c
)

add_executable(${turl_name} ${turl_srcs})

# Dependencies
target_link_libraries(${turl_name} PRIVATE 
    http_client 
    TurboNet::Core
    TurboNet::Mustache 
    TurboNet::JsonParser 
    TurboNet::CmdParser
    TurboNet::Quick 
    qjs
)

# Standard project setup
set_target_properties(${turl_name} PROPERTIES FOLDER http/tools)
add_sanitizers(${turl_name})

# Installation
install(TARGETS ${turl_name} RUNTIME DESTINATION bin)
