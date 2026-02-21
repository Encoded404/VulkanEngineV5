# Executable that links the static library
add_executable(${APP_TARGET})
target_sources(${APP_TARGET} PRIVATE
    main.cpp
)

target_link_libraries(${APP_TARGET} PRIVATE ${LIBRARY_TARGET})

# Optionally set executable output name; keep simple default
set_target_properties(${APP_TARGET} PROPERTIES OUTPUT_NAME ${APP_TARGET})

if(OUTPUT_PATH)
    set_target_properties(${APP_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_PATH})
endif()

# Set the C++ standard and enable compiler features
target_compile_features(${APP_TARGET} PRIVATE cxx_std_20)

# Compiler-specific warning options for the library target
target_compile_options(${APP_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${APP_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Include directories
target_include_directories(${APP_TARGET} PRIVATE
    # include any app-specific includes here
)

# Add compile definitions for logging toggle
# Define LOGIFACE_ENABLE_LOGGING based on ENABLE_LOGGING
target_compile_definitions(${APP_TARGET}
    PUBLIC
        $<$<BOOL:${ENABLE_LOGGING}>:LOGIFACE_ENABLE_LOGGING=1>
        $<$<NOT:$<BOOL:${ENABLE_LOGGING}>>:LOGIFACE_ENABLE_LOGGING=0>
        LOGIFACE_DEFAULT_TO_FUNCTION=1
)
