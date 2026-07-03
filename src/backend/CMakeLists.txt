add_library(${BACKEND_TARGET} STATIC)
target_compile_features(${BACKEND_TARGET} PUBLIC cxx_std_20)
target_link_libraries(${BACKEND_TARGET} PUBLIC logiface::logiface VulkanShared)

find_package(SDL3 CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(imgui CONFIG REQUIRED)

if(SYSTEM_VULKAN)
  find_library(SYSTEM_VULKAN_LIB NAMES vulkan PATHS /usr/lib64 /usr/lib NO_DEFAULT_PATH)
  find_path(SYSTEM_VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h PATHS /usr/include NO_DEFAULT_PATH)
  if(SYSTEM_VULKAN_LIB AND SYSTEM_VULKAN_INCLUDE_DIR)
    message(STATUS "Overriding vcpkg Vulkan with system: ${SYSTEM_VULKAN_LIB}")
    set_target_properties(Vulkan::Vulkan PROPERTIES
      IMPORTED_LOCATION "${SYSTEM_VULKAN_LIB}"
      INTERFACE_INCLUDE_DIRECTORIES "${SYSTEM_VULKAN_INCLUDE_DIR}"
    )
  else()
    message(WARNING "System Vulkan not found at /usr, using vcpkg Vulkan")
  endif()
endif()

target_link_libraries(${BACKEND_TARGET} PRIVATE
    SDL3::SDL3
    imgui::imgui
    Vulkan::HppModule
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
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

set_source_files_properties(
    vulkan/VulkanInstance.cpp
    vulkan/VulkanDevice.cpp
    vulkan/DispatchLoader.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

target_sources(${BACKEND_TARGET} PRIVATE
    event/Event.cpp
    platform/SdlWindow.cpp
    platform/SdlWindowBackend.cpp
    RenderGraph/RenderGraph.cpp
    vulkan/RenderGraphExecutor.cpp
    RenderGraph/RenderGraphCompile.cpp
    vulkan/FramePacer.cpp
    vulkan/VulkanBootstrap.cpp
    vulkan/DispatchLoader.cpp
    vulkan/VulkanBootstrapBackend.cpp
    vulkan/VulkanInstance.cpp
    vulkan/VulkanDevice.cpp
    vulkan/VulkanSwapchain.cpp
    vulkan/MemoryUtils.cpp
    vulkan/ImageUtils.cpp
    vulkan/PipelineBuilder.cpp
    vulkan/DebugUtils.cpp
    imgui/ImGuiVulkanBackend.cpp
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${BACKEND_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            event/Event.cppm
            platform/SdlWindow.cppm
            platform/SdlWindowBackend.cppm
            RenderGraph/RenderGraph.cppm
            vulkan/GraphExecutionContext.cppm
            vulkan/RenderGraphContext.cppm
            vulkan/FramePacer.cppm
            vulkan/CommonTypes.cppm
            vulkan/VulkanBootstrap.cppm
            vulkan/VulkanBootstrapBackend.cppm
            vulkan/VulkanInstance.cppm
            vulkan/VulkanDevice.cppm
            vulkan/VulkanSwapchain.cppm
            vulkan/MemoryUtils.cppm
            vulkan/ImageUtils.cppm
            vulkan/PipelineBuilder.cppm
            vulkan/DebugUtils.cppm
            imgui/ImGuiVulkanBackend.cppm
    )
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${BACKEND_TARGET})
endif()
