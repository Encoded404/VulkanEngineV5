add_executable(security_tests
  data_cipher_tests.cpp
  password_hash_tests.cpp
  key_exchange_tests.cpp
)
target_link_libraries(security_tests PRIVATE VulkanSecurity)
setup_test_target(security_tests)
