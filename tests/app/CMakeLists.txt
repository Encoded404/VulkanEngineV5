find_package(SDL3 CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS stacktrace_basic)
find_package(CLI11 CONFIG REQUIRED)

add_executable(app_tests
  example_tests.cpp
  ${CMAKE_SOURCE_DIR}/src/app/Game.cpp
)
target_link_libraries(app_tests PRIVATE
  ${ENGINE_TARGET}
  ${BACKEND_TARGET}
  slang_shared_reflection
  SDL3::SDL3
  Boost::stacktrace_basic
  CLI11::CLI11
)
target_compile_options(app_tests PRIVATE -Wno-c23-extensions)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_sources(app_tests PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_SOURCE_DIR}/src/app
    FILES
      ${CMAKE_SOURCE_DIR}/src/app/Components/SimpleControllerComponent.cppm
      ${CMAKE_SOURCE_DIR}/src/app/Components/TransformControlComponent.cppm
      ${CMAKE_SOURCE_DIR}/src/app/Game.cppm
  )

  # Register app shader modules needed by Game.cpp
  # (engine shader modules + ShaderReflection inherited via PUBLIC on VulkanEngine)
  get_target_property(_app_cppm VulkanApp_shaders SLANG_CPPM_FILES)
  if(_app_cppm)
    target_sources(app_tests PRIVATE
      FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_BINARY_DIR}/generated/shaders/app"
      FILES ${_app_cppm}
    )
  endif()
endif()

# Ensure .spv files are built before test compilation
add_dependencies(app_tests VulkanEngine_shaders VulkanApp_shaders)
setup_test_target(app_tests)
