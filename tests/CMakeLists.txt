find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

# Shared options and helper shared by all test subdirectories
set(TEST_COMPILE_OPTS
  $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
  $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)
set(TEST_COMPILE_OPTS_DEBUG
  "$<$<CONFIG:Debug>:-g>"
  "$<$<CONFIG:Debug>:-O0>"
  "$<$<CONFIG:RelWithDebInfo>:-g>"
  "$<$<CONFIG:RelWithDebInfo>:-O2>"
  "$<$<CONFIG:Release>:-O3>"
)

function(setup_test_target TARGET)
  target_link_libraries(${TARGET} PRIVATE GTest::gtest GTest::gtest_main)
  target_compile_definitions(${TARGET} PRIVATE LOGIFACE_ENABLE_LOGGING=1)
  target_compile_options(${TARGET} PRIVATE ${TEST_COMPILE_OPTS})
  if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${TARGET} PRIVATE ${TEST_COMPILE_OPTS_DEBUG})
  endif()
  target_compile_options(${TARGET} PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffunction-sections>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fdata-sections>
  )
  target_link_options(${TARGET} PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wl,--gc-sections>
  )
  if(SYSTEM_VULKAN)
    set_target_properties(${TARGET} PROPERTIES BUILD_RPATH "/usr/lib64")
  endif()
  gtest_discover_tests(${TARGET}
    PROPERTIES
      ENVIRONMENT "TEST_LOG_DIR=${CMAKE_BINARY_DIR}/Testing/logs"
  )
endfunction()

add_subdirectory(backend)
add_subdirectory(engine)
add_subdirectory(app)
