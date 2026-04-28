# Set the C++ standard and enable compiler features
add_library(${LIBRARY_TARGET} STATIC)
target_compile_features(${LIBRARY_TARGET} PUBLIC cxx_std_20)
target_link_libraries(${LIBRARY_TARGET} PUBLIC logiface::logiface)

target_link_libraries(${LIBRARY_TARGET} PRIVATE FileLoader)

find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
target_link_libraries(${LIBRARY_TARGET} PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    glm::glm
)
target_link_libraries(${LIBRARY_TARGET} PUBLIC KTX::ktx)

target_include_directories(${LIBRARY_TARGET} PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

# Compiler-specific warning options for the library target
target_compile_options(${LIBRARY_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${LIBRARY_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()


# Source files
target_sources(${LIBRARY_TARGET} PRIVATE
    Mesh/MeshAssembler.cpp
    Utils/MemoryUtils.cpp
    Utils/ImageUtils.cpp
    Utils/PipelineUtils.cpp
    ResourceSystem/ResourceSystem.cpp
    ResourceSystem/TextureResource.cpp
    RenderGraph/RenderGraph.cpp
    Platform/SdlPlatformShell.cpp
    Platform/SdlPlatformBackend.cpp
    Runtime/RuntimeShell.cpp
    Runtime/VulkanBootstrap.cpp
    Runtime/VulkanDispatchLoader.cpp
    Runtime/VulkanBootstrapBackend.cpp
    Runtime/VulkanInstance.cpp
    Runtime/VulkanDevice.cpp
    Runtime/VulkanSwapchain.cpp
)

# C++ module interface units: use CMake 3.28 FILE_SET to declare module
# interface/source units to the target. CMake will build these as module
# translation units in the proper phase for compilers that support C++ modules.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${LIBRARY_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_SOURCE_DIR}/src
        FILES
            Component/Component.cppm
            Utils/MemoryUtils.cppm
            Utils/ImageUtils.cppm
            Utils/PipelineUtils.cppm
            Mesh/MeshTypes.cppm
            Mesh/MeshAssembler.cppm
            ResourceSystem/ResourceSystem.cppm
            ResourceSystem/TextureResource.cppm
            RenderGraph/RenderGraph.cppm
            RenderGraph/GraphExecutionContext.cppm
            RenderGraph/GraphExecutionBridge.cppm
            Platform/SdlPlatformShell.cppm
            Platform/SdlPlatformBackend.cppm
            Runtime/RuntimeShell.cppm
            Runtime/CommonTypes.cppm
            Runtime/VulkanBootstrap.cppm
            Runtime/VulkanBootstrapBackend.cppm
            Runtime/VulkanInstance.cppm
            Runtime/VulkanDevice.cppm
            Runtime/VulkanSwapchain.cppm
    )
endif()

# Optionally enable per-target clang-tidy if the helper function is available.
if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${LIBRARY_TARGET})
endif()
