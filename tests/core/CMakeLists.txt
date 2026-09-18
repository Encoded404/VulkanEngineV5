add_executable(engine_tests
  input_system_tests.cpp
  resource_system_texture_tests.cpp
  gpl_resolution_tests.cpp
  tlsf_allocator_tests.cpp
  transient_allocator_tests.cpp
  render_pipeline_resolution_tests.cpp
  render_pass_context_tests.cpp
  descriptor_layout_tests.cpp
  pass_pipeline_tests.cpp
  render_pipeline_registration_tests.cpp
  builtin_pass_declarations_tests.cpp
  render_resize_tests.cpp
  gpu_stats_tests.cpp
)
target_link_libraries(engine_tests PRIVATE ${ENGINE_TARGET} ${BACKEND_TARGET})
setup_test_target(engine_tests)
