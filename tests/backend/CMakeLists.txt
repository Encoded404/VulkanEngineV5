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
setup_test_target(backend_tests)
