add_executable(gpu_tests
  offscreen_hash_tests.cpp
  transient_allocator_gpu_tests.cpp
  shader_watcher_tests.cpp
  gpu_stats_gpu_tests.cpp
)
target_link_libraries(gpu_tests PRIVATE
  ${ENGINE_TARGET}
  ${BACKEND_TARGET}
)

enable_test_support_modules(gpu_tests)

# `gpu` label: excluded by the default test presets, opt in with the *-gpu
# test presets or `ctest --preset debug --label-regex gpu`.
#
# Leak detection is disabled by the *-gpu test presets (see CMakePresets.json):
# creating a Vulkan instance loads the loader and an ICD whose allocations are
# not all released before exit (observed inside libvulkan/ICD frames, not in
# engine code). The device-free suites still run full leak detection.
setup_test_target(gpu_tests LABELS gpu)
