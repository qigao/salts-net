cmake_minimum_required(VERSION 3.15)
project(turbonet_email C)

# ── Dependencies ───────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)

# ── Include directories ────────────────────────────────────────────────
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ── Source files ───────────────────────────────────────────────────────
set(EMAIL_SOURCES
  src/email_client.c
  src/email_message.c
  src/email_smtp.c
  src/email_imap.c
  src/email_pop3.c
)

set(EMAIL_HEADERS
  include/email/email_client.h
  include/email/email_message.h
  include/email/email_smtp.h
  include/email/email_imap.h
  include/email/email_pop3.h
)

# ── Library target ─────────────────────────────────────────────────────
add_library(turbonet_email STATIC ${EMAIL_SOURCES} ${EMAIL_HEADERS})

target_link_libraries(turbonet_email
  PUBLIC
    TurboNet::TinyTest
    mime_parser
    TurboNet::CoroNet
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_include_directories(turbonet_email
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

set_target_properties(turbonet_email
  PROPERTIES
    FOLDER "email"
    EXPORT_NAME Email
)

# ── Test target ────────────────────────────────────────────────────────
add_executable(test_email
  test/test_email_smtp.c
)

target_link_libraries(test_email
  turbonet_email
  TurboNet::TinyTest
)

target_include_directories(test_email PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
set_target_properties(test_email PROPERTIES FOLDER "email")
# ── Add to tests ───────────────────────────────────────────────────────
add_test(NAME email_smtp_tests COMMAND test_email)
# ── Test target ────────────────────────────────────────────────────────
add_executable(test_message
  test/test_email_message.c
 )

target_link_libraries(test_message
  turbonet_email
  TurboNet::TinyTest
)

target_include_directories(test_message PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
set_target_properties(test_message PROPERTIES FOLDER "email")
# ── Add to tests ───────────────────────────────────────────────────────
add_test(NAME email_message_tests COMMAND test_message)

add_executable(test_email_integration
  test/test_email_integration.c
)

target_link_libraries(test_email_integration
  turbonet_email
  TurboNet::TinyTest
)

target_include_directories(test_email_integration PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(test_email_integration PROPERTIES FOLDER "email")
add_test(NAME email_integration_tests COMMAND test_email_integration)

# ── Examples ───────────────────────────────────────────────────────────
add_executable(example_email_send
  examples/example_email_send.c
)

target_link_libraries(example_email_send
  turbonet_email
  TurboNet::TinyTest
)

target_include_directories(example_email_send PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(example_email_send PROPERTIES FOLDER "email/examples")

add_executable(example_email_receive
  examples/example_email_receive.c
)

target_link_libraries(example_email_receive
  turbonet_email
  TurboNet::TinyTest
)

target_include_directories(example_email_receive PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(example_email_receive PROPERTIES FOLDER "email/examples")

add_executable(example_email_imap
  examples/example_email_imap.c
)

target_link_libraries(example_email_imap
  turbonet_email
  TurboNet::TinyTest
)

target_include_directories(example_email_imap PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(example_email_imap PROPERTIES FOLDER "email/examples")
