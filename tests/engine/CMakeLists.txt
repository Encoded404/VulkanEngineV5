add_executable(engine_tests
  input_system_tests.cpp
  resource_system_texture_tests.cpp
)
target_link_libraries(engine_tests PRIVATE ${ENGINE_TARGET} ${BACKEND_TARGET})
setup_test_target(engine_tests)
