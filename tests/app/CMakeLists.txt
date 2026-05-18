find_package(SDL3 CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS stacktrace_basic)
find_package(CLI11 CONFIG REQUIRED)

add_executable(app_tests
  example_tests.cpp
  ${CMAKE_SOURCE_DIR}/src/app/Game.cpp
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_sources(app_tests PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_SOURCE_DIR}/src/app
    FILES
      ${CMAKE_SOURCE_DIR}/src/app/Components/DemoInputComponent.cppm
      ${CMAKE_SOURCE_DIR}/src/app/Game.cppm
  )
endif()
target_link_libraries(app_tests PRIVATE
  ${ENGINE_TARGET}
  ${BACKEND_TARGET}
  SDL3::SDL3
  Boost::stacktrace_basic
  CLI11::CLI11
)
setup_test_target(app_tests)
