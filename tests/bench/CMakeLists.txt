# Device-free component-update benchmark.
#
# A plain executable, not a `setup_test_target`: that helper links
# GTest::gtest_main and registers a ctest case via gtest_discover_tests, so the
# timing loop would run (with --gtest_list_tests) on every build.
add_executable(component_update_bench
  component_update_bench.cpp
)
target_compile_features(component_update_bench PRIVATE cxx_std_23)
set_target_properties(component_update_bench PROPERTIES CXX_MODULE_STD ON)
target_compile_options(component_update_bench PRIVATE ${TEST_COMPILE_OPTS})

# Module BMIs are target-scoped: without the engine link, the
# `import VulkanEngine.ECS.ComponentRegistry;` above does not resolve. JobScheduler
# is linked explicitly because the engine consumes VulkanShared PRIVATE, so the
# modules are not transitively visible.
target_link_libraries(component_update_bench PRIVATE
  ${ENGINE_TARGET}
  VulkanShared
)

vkengine_target_config_flags(component_update_bench)

if(TARGET project_sections)
  target_link_libraries(component_update_bench PRIVATE project_sections)
endif()
if(TARGET project_size)
  target_link_libraries(component_update_bench PRIVATE project_size)
endif()
if(TARGET project_libcpp_static)
  target_link_libraries(component_update_bench PRIVATE project_libcpp_static)
endif()
if(TARGET project_libcpp_sanitized)
  add_dependencies(component_update_bench project_libcpp_sanitized)
endif()
if(TARGET project_sanitizers)
  target_link_libraries(component_update_bench PRIVATE project_sanitizers)
endif()
