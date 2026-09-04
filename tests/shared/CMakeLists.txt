add_executable(shared_tests
  teardown_tests.cpp
)
target_link_libraries(shared_tests PRIVATE
  VulkanShared
)

setup_test_target(shared_tests)
