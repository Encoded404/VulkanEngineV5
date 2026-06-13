add_executable(backend_tests
  backend_factory_tests.cpp
  vulkan_bootstrap_tests.cpp
  platform_shell_tests.cpp
  graph_execution_context_tests.cpp
  graph_execution_bridge_tests.cpp
  render_graph_tests.cpp
  render_graph_diagnostics_tests.cpp
  runtime_shell_tests.cpp
  event_system_tests.cpp
  component_system_tests.cpp
)
target_link_libraries(backend_tests PRIVATE ${BACKEND_TARGET})

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_sources(backend_tests PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_SOURCE_DIR}/tests/app
    FILES
      ${CMAKE_SOURCE_DIR}/tests/app/test_logging.cppm
  )
endif()

setup_test_target(backend_tests)
