add_library(VulkanShared STATIC)
target_compile_features(VulkanShared PUBLIC cxx_std_20)

find_package(Vulkan REQUIRED)
target_link_libraries(VulkanShared PUBLIC Vulkan::HppModule)

target_include_directories(VulkanShared PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(VulkanShared PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(VulkanShared PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            CallbackList.cppm
            ThreadPool.cppm
            Timer.cppm
    )
endif()

target_sources(VulkanShared PRIVATE
    Timer.cpp
)

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(VulkanShared)
endif()
