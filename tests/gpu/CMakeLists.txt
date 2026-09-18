add_executable(gpu_tests
  offscreen_hash_tests.cpp
)
target_link_libraries(gpu_tests PRIVATE
  ${ENGINE_TARGET}
  ${BACKEND_TARGET}
)

enable_test_support_modules(gpu_tests)

# `gpu` label: excluded by the default test presets, opt in with the *-gpu
# test presets or `ctest --preset debug --label-regex gpu`.
setup_test_target(gpu_tests LABELS gpu)
