# WebSocket Parser - Hand-written parsers for netcore

# Find llhttp for handshake parsing
find_package(llhttp CONFIG REQUIRED)

# Create parser library using source .c files directly
add_library(websocket_parser STATIC websocket_frame_parser.c
                                    websocket_handshake_parser.c)

# Include current source directory for headers
target_include_directories(
  websocket_parser PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# Link llhttp for HTTP handshake parsing
target_link_libraries(websocket_parser PRIVATE llhttp::llhttp_static
                                               TurboNet::Utils)

# Export include directories for parent scope
set(WEBSOCKET_PARSER_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    PARENT_SCOPE)

set_target_properties(websocket_parser PROPERTIES FOLDER NetCore
                                                  EXPORT_NAME WebSocketParser)
add_library(TurboNet::WebSocketParser ALIAS websocket_parser)

install(
  TARGETS websocket_parser
  EXPORT TurboNetTargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin)

install(FILES websocket_frame_parser.h websocket_handshake_parser.h
        DESTINATION include/netcore/parser)
