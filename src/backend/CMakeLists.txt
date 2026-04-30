add_library(${BACKEND_TARGET} STATIC)
target_compile_features(${BACKEND_TARGET} PUBLIC cxx_std_20)
target_link_libraries(${BACKEND_TARGET} PUBLIC logiface::logiface)

find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)

target_link_libraries(${BACKEND_TARGET} PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
)

target_include_directories(${BACKEND_TARGET} PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(${BACKEND_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${BACKEND_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

target_sources(${BACKEND_TARGET} PRIVATE
    Event/Event.cpp
    Platform/SdlPlatform.cpp
    Platform/SdlPlatformBackend.cpp
    RenderGraph/RenderGraph.cpp
    Runtime/FrameLoop.cpp
    Runtime/VulkanBootstrap.cpp
    Runtime/VulkanDispatchLoader.cpp
    Runtime/VulkanBootstrapBackend.cpp
    Runtime/VulkanInstance.cpp
    Runtime/VulkanDevice.cpp
    Runtime/VulkanSwapchain.cpp
    Utils/MemoryUtils.cpp
    Utils/ImageUtils.cpp
    Utils/PipelineUtils.cpp
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${BACKEND_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Component/Component.cppm
            Event/Event.cppm
            Platform/SdlPlatform.cppm
            Platform/SdlPlatformBackend.cppm
            RenderGraph/RenderGraph.cppm
            RenderGraph/GraphExecutionContext.cppm
            RenderGraph/GraphExecutionBridge.cppm
            Runtime/FrameLoop.cppm
            Runtime/CommonTypes.cppm
            Runtime/VulkanBootstrap.cppm
            Runtime/VulkanBootstrapBackend.cppm
            Runtime/VulkanInstance.cppm
            Runtime/VulkanDevice.cppm
            Runtime/VulkanSwapchain.cppm
            Utils/MemoryUtils.cppm
            Utils/ImageUtils.cppm
            Utils/PipelineUtils.cppm
    )
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${BACKEND_TARGET})
endif()
