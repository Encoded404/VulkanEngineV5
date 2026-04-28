add_library(${ENGINE_TARGET} STATIC)
target_compile_features(${ENGINE_TARGET} PUBLIC cxx_std_20)
target_link_libraries(${ENGINE_TARGET} PUBLIC logiface::logiface)

target_link_libraries(${ENGINE_TARGET} PRIVATE
    FileLoader
    ${BACKEND_TARGET}
)

find_package(Vulkan REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)

target_link_libraries(${ENGINE_TARGET} PRIVATE
    Vulkan::Vulkan
    KTX::ktx
    glm::glm
    fastgltf::fastgltf
)

target_include_directories(${ENGINE_TARGET} PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(${ENGINE_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${ENGINE_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

target_sources(${ENGINE_TARGET} PRIVATE
    Input/Input.cpp
    Mesh/MeshAssembler.cpp
    ResourceSystem/ResourceSystem.cpp
    ResourceSystem/TextureResource.cpp
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${ENGINE_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Component/Component.cppm
            Input/Input.cppm
            Mesh/MeshTypes.cppm
            Mesh/MeshAssembler.cppm
            ResourceSystem/ResourceSystem.cppm
            ResourceSystem/TextureResource.cppm
            Components/Transform.cppm
            FileLoaders/Mesh/BinMeshAssembler.cppm
            FileLoaders/Mesh/GltfMeshAssembler.cppm
    )
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${ENGINE_TARGET})
endif()

