set(RUNTIME_TARGET "EngineRuntime")
add_library(${RUNTIME_TARGET} STATIC)
target_compile_features(${RUNTIME_TARGET} PUBLIC cxx_std_20)
target_link_libraries(${RUNTIME_TARGET} PUBLIC ${ENGINE_TARGET} ${BACKEND_TARGET})
target_include_directories(${RUNTIME_TARGET} PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_compile_options(${RUNTIME_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${RUNTIME_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            RunApplication.cppm
    )
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${RUNTIME_TARGET})
endif()
