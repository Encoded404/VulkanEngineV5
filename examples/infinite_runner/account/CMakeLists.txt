# Client-side profile persistence. It links VulkanShared for the per-user
# Storage layer, which is why it is separate from the engine-free leaderboard
# library that the headless server links.
add_library(infinite_runner_account STATIC
    ProfileStore.cpp
)
target_compile_features(infinite_runner_account PUBLIC cxx_std_23)

target_link_libraries(infinite_runner_account PUBLIC
    infinite_runner_leaderboard
    VulkanShared
)

target_include_directories(infinite_runner_account PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_options(infinite_runner_account PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(infinite_runner_account PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(infinite_runner_account PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            ProfileStore.cppm
    )
endif()

if(TARGET project_sanitizers)
    target_link_libraries(infinite_runner_account PRIVATE project_sanitizers)
endif()
if(TARGET project_sections)
    target_link_libraries(infinite_runner_account PRIVATE project_sections)
endif()
if(TARGET project_size)
    target_link_libraries(infinite_runner_account PRIVATE project_size)
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(infinite_runner_account)
endif()
