# Add the test executable
add_executable(project_tests
    component_system_tests.cpp
    resource_system_texture_tests.cpp
    render_graph_tests.cpp
    render_graph_diagnostics_tests.cpp
    runtime_shell_tests.cpp
    graph_execution_context_tests.cpp
    graph_execution_bridge_tests.cpp
    platform_shell_tests.cpp
    event_system_tests.cpp
    input_system_tests.cpp
    vulkan_bootstrap_tests.cpp
    backend_factory_tests.cpp
)

# Find GTest installed via vcpkg and link it along with the main library
find_package(GTest CONFIG REQUIRED)
include(GoogleTest)
target_link_libraries(project_tests PRIVATE
    ${ENGINE_TARGET}
    ${BACKEND_TARGET}
    GTest::gtest
    GTest::gtest_main
)

target_compile_definitions(project_tests PRIVATE
    LOGIFACE_ENABLE_LOGGING=1
)

if(SYSTEM_VULKAN)
  set_target_properties(project_tests PROPERTIES BUILD_RPATH "/usr/lib64")
endif()

# Set compiler options for the test executable
target_compile_options(project_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
	target_compile_options(project_tests PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Let CTest discover individual GoogleTest tests in the binary
gtest_discover_tests(project_tests
    PROPERTIES
        ENVIRONMENT "TEST_LOG_DIR=${CMAKE_BINARY_DIR}/Testing/logs"
)
