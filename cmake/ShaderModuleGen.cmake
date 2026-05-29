# Generates C++20 module interface units (.cppm) that embed compiled SPIR-V
# via the #embed directive (supported by Clang 19+).
#
# For each .glsl shader source in CMAKE_CURRENT_SOURCE_DIR, produces a .cppm
# with module name Shaders.<Namespace>.<PascalCaseName> and a struct with a
# CreateModule() convenience method.
# Also generates a unified module Shaders.<Namespace> that re-exports all.
#
# Usage:
#   include(ShaderModuleGen)
#   generate_shader_modules(TARGET
#       NAMESPACE "Engine|App"
#       OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders/engine"
#   )
#
# The target gets properties:
#   GENERATED_SHADER_MODULES   — list of generated .cppm file paths
#   GENERATED_MODULE_NAMES     — corresponding module names

function(generate_shader_modules TARGET)
    cmake_parse_arguments(ARG "" "NAMESPACE;OUTPUT_DIR" "" ${ARGN})

    if(NOT ARG_NAMESPACE OR NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "generate_shader_modules: NAMESPACE and OUTPUT_DIR are required")
    endif()

    file(GLOB_RECURSE GLSL_SOURCE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/*.frag"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.vert"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.geom"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.tesc"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.tese"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.mesh"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.task"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.rgen"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.rchit"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.rmiss"
    )

    if(NOT GLSL_SOURCE_FILES)
        message(WARNING "No shader source files found in ${CMAKE_CURRENT_SOURCE_DIR}")
        add_custom_target(${TARGET})
        return()
    endif()

    set(GENERATED_FILES)
    set(MODULE_NAMES)

    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")

    foreach(GLSL ${GLSL_SOURCE_FILES})
        get_filename_component(FILE_NAME ${GLSL} NAME)

        # Convert filename to PascalCase for the class name.
        # e.g. "expand.comp"         → "ExpandComp"
        #       "depth_indir.vert"    → "DepthIndirVert"
        #       "collect_count_compact.comp" → "CollectCountCompactComp"
        string(REGEX REPLACE "[._-]" ";" NAME_PARTS "${FILE_NAME}")
        set(CLASS_NAME "")
        foreach(PART ${NAME_PARTS})
            string(SUBSTRING "${PART}" 0 1 FIRST)
            string(TOUPPER "${FIRST}" FIRST_UPPER)
            string(SUBSTRING "${PART}" 1 -1 REST)
            set(CLASS_NAME "${CLASS_NAME}${FIRST_UPPER}${REST}")
        endforeach()

        set(MODULE_NAME "Shaders.${ARG_NAMESPACE}.${CLASS_NAME}")
        set(CPPM_FILE "${ARG_OUTPUT_DIR}/${CLASS_NAME}.cppm")

        file(WRITE "${CPPM_FILE}"
"// Auto-generated. Do not edit.
// Source: ${GLSL}
// Module: ${MODULE_NAME}

module;

#include <cstdint>
#include <span>

#include <vulkan/vulkan_raii.hpp>

export module ${MODULE_NAME};

export namespace Shaders::${ARG_NAMESPACE} {
    struct ${CLASS_NAME} {
        [[nodiscard]] static std::span<const std::uint32_t> GetSpirvWords() noexcept {
            // NOLINTNEXTLINE(modernize-avoid-c-arrays)
            alignas(4) static constexpr unsigned char kBytes[] = {
                #embed \"${FILE_NAME}.spv\"
            };
            return { reinterpret_cast<const std::uint32_t*>(kBytes),
                     sizeof(kBytes) / sizeof(std::uint32_t) };
        }

        [[nodiscard]] static vk::raii::ShaderModule
        CreateModule(vk::raii::Device const& device) {
            auto const words = GetSpirvWords();
            vk::ShaderModuleCreateInfo const info({},
                words.size() * sizeof(std::uint32_t), words.data());
            return vk::raii::ShaderModule(device, info);
        }
    };
}
"
        )

        list(APPEND GENERATED_FILES "${CPPM_FILE}")
        list(APPEND MODULE_NAMES "${MODULE_NAME}")
    endforeach()

    # Generate unified parent module that re-exports all individual shader modules.
    # e.g.  import Shaders.Engine;  imports everything in one statement.
    set(UNIFIED_MODULE_NAME "Shaders.${ARG_NAMESPACE}")
    set(UNIFIED_MODULE_FILE "${ARG_OUTPUT_DIR}/__shaders_module__.cppm")
    set(UNIFIED_CONTENT "// Auto-generated. Do not edit.\n// Re-exports all shader modules under Shaders.${ARG_NAMESPACE}\n\n")
    set(UNIFIED_CONTENT "${UNIFIED_CONTENT}export module ${UNIFIED_MODULE_NAME};\n\n")
    foreach(MN ${MODULE_NAMES})
        set(UNIFIED_CONTENT "${UNIFIED_CONTENT}export import ${MN};\n")
    endforeach()
    file(WRITE "${UNIFIED_MODULE_FILE}" "${UNIFIED_CONTENT}")
    list(APPEND GENERATED_FILES "${UNIFIED_MODULE_FILE}")
    list(APPEND MODULE_NAMES "${UNIFIED_MODULE_NAME}")

    add_custom_target(${TARGET} DEPENDS ${GENERATED_FILES})

    # #embed is a Clang extension in C++20 mode; suppress for all generated files
    set_source_files_properties(${GENERATED_FILES} PROPERTIES
        COMPILE_OPTIONS "-Wno-c23-extensions"
    )

    set_target_properties(${TARGET} PROPERTIES
        GENERATED_SHADER_MODULES "${GENERATED_FILES}"
        GENERATED_MODULE_NAMES "${MODULE_NAMES}"
    )
endfunction()
