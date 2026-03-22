# Set the C++ standard and enable compiler features
add_library(${LIBRARY_TARGET} STATIC)
target_compile_features(${LIBRARY_TARGET} PUBLIC cxx_std_20)
target_link_libraries(${LIBRARY_TARGET} PUBLIC logiface::logiface)

target_link_libraries(${LIBRARY_TARGET} PRIVATE VulkanRAIIWrapper)
target_link_libraries(${LIBRARY_TARGET} PRIVATE FileLoader)

# Compiler-specific warning options for the library target
target_compile_options(${LIBRARY_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# Module flags applied per-target using modern target_* APIs
## Apply module flags only for Clang (explicit to avoid passing unsupported flags)
target_compile_options(${LIBRARY_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:Clang>:-fmodules;-fimplicit-modules;-fimplicit-module-maps;-fcxx-modules>
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${LIBRARY_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Include directories
target_include_directories(${LIBRARY_TARGET}
PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/include>
)

# Source files
target_sources(${LIBRARY_TARGET} PRIVATE
    Mesh/MeshAssembler.cpp
    Utils/MemoryUtils.cpp
    Utils/ImageUtils.cpp
    Utils/PipelineUtils.cpp
)

# Build module interface units as OBJECT libraries so they are compiled
# before consumers that `import` them. Then link their objects into the
# main library target.
add_library(${LIBRARY_TARGET}_modules OBJECT
    Utils/MemoryUtils.cppm
    Utils/ImageUtils.cppm
    Utils/PipelineUtils.cppm
    Mesh/MeshTypes.cppm
    Mesh/MeshAssembler.cppm
)

target_link_libraries(${LIBRARY_TARGET} PRIVATE $<TARGET_OBJECTS:${LIBRARY_TARGET}_modules>)
