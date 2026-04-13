add_library(
  turbo_langchain SHARED
  src/turbo_event.c
  src/turbo_event_log.c
  src/turbo_graph.c
  src/turbo_graph_run_log.c
  src/turbo_prompt.c
  src/turbo_runnable.c
  src/turbo_chain.c
  src/turbo_model_provider.c
  src/turbo_runtime_binary_reader.c
  src/turbo_runtime_binary_mir.c
  src/turbo_runtime_binary_schema.c
  src/turbo_runtime_data_bind.c
  src/turbo_tool_runtime.c
  src/turbo_tool_runtime_wasm3.c
  src/turbo_tool_registry.c
  src/turbo_tool_schema.c
  src/turbo_openai_agent.c
  src/turbo_action_tool.c
  src/turbo_agent_policy.c)
cmake_config_target(
  turbo_langchain
  ALIAS TurboNet::LangChain
  EXPORT_NAME LangChain
  FOLDER langchain)

target_include_directories(
  turbo_langchain
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_SOURCE_DIR}/vendor/mir)

target_link_libraries(
  turbo_langchain
  PUBLIC TurboNet::Parser TurboNet::HttpClient
  PRIVATE TurboNet::Wasm3 mir)
target_compile_definitions(turbo_langchain PRIVATE SHARED_CXX)

install(
  FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_langchain.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_event.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_event_log.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_graph.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_graph_run_log.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_model.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_prompt.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_runnable.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_chain.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_tool.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_runtime_binary_reader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_runtime_binary_mir.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_runtime_binary_schema.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_runtime_data_bind.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_tool_runtime.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_tool_runtime_wasm3.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_tool_registry.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_tool_schema.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_openai_agent.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_openai_agent_extensions.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_openai_agent_state.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_openai_agent_sse.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_openai_agent_graph.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_openai_agent_workflow.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_action_tool.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_agent_policy.h
  DESTINATION include)

cmake_add_test(
  SOURCES tests/test_turbo_event.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_event_log.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_graph.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_graph_run_log.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_prompt.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_runnable.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_langchain_public_api.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_openai_agent_graph_api.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_openai_agent_state_api.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_openai_agent_extensions_api.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_openai_agent_workflow_api.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_chain.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_runtime_binary_reader.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_runtime_binary_mir.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_runtime_binary_schema.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_runtime_data_bind.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_tool_runtime.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

find_program(LANGCHAIN_WASM3_CLANG NAMES clang clang.exe)
if(LANGCHAIN_WASM3_CLANG)
  set(LANGCHAIN_WASM3_TOOL_WASM
      "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_test.wasm")

  add_custom_command(
    OUTPUT ${LANGCHAIN_WASM3_TOOL_WASM}
    COMMAND
      ${LANGCHAIN_WASM3_CLANG}
      --target=wasm32-unknown-unknown
      -O2
      -nostdlib
      -Wl,--no-entry
      -Wl,--export=turbo_tool_count
      -Wl,--export=turbo_tool_name
      -Wl,--export=turbo_tool_description
      -Wl,--export=turbo_tool_parameters
      -Wl,--export=turbo_tool_strict
      -Wl,--export=turbo_tool_input_ptr
      -Wl,--export=turbo_tool_input_capacity
      -Wl,--export=turbo_tool_output_ptr
      -Wl,--export=turbo_tool_output_capacity
      -Wl,--export=turbo_tool_invoke
      -Wl,--export-memory
      -Wl,--initial-memory=131072
      -Wl,--max-memory=131072
      -Wl,--allow-undefined
      -o ${LANGCHAIN_WASM3_TOOL_WASM}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3.c
    VERBATIM)

  cmake_add_test(
    SOURCES tests/test_turbo_tool_runtime_wasm3.c
    LIBS turbo_langchain TurboNet::TinyTest
    FOLDER langchain)

  if(TARGET test_turbo_tool_runtime_wasm3)
    add_custom_target(langchain_wasm3_tool_guest_test
                      DEPENDS ${LANGCHAIN_WASM3_TOOL_WASM})
    set_target_properties(langchain_wasm3_tool_guest_test
                          PROPERTIES FOLDER "langchain")
    add_dependencies(test_turbo_tool_runtime_wasm3 langchain_wasm3_tool_guest_test)
    target_compile_definitions(
      test_turbo_tool_runtime_wasm3 PRIVATE
      LANGCHAIN_WASM3_TOOL_WASM_PATH=\"${LANGCHAIN_WASM3_TOOL_WASM}\")
  endif()
endif()

cmake_add_test(
  SOURCES tests/test_turbo_tool_schema.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_model_provider.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_action_tool.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)

cmake_add_test(
  SOURCES tests/test_turbo_agent_policy.c
  LIBS turbo_langchain TurboNet::TinyTest
  FOLDER langchain)
