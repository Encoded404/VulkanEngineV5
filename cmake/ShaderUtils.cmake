# Shader compilation utilities for Vulkan
find_program(GLSLC_EXECUTABLE glslc HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE})
find_program(GLSLANG_VALIDATOR glslangValidator HINTS ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE})

if(NOT GLSLC_EXECUTABLE AND NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslc or glslangValidator not found! Please install Vulkan SDK.")
endif()

# Extracts shader variant data from a companion variants file.
# Expected line format inside <shader>.variants:
#   variant <name> [DEFINE_0 DEFINE_1 ...]
# Lines beginning with '#' or empty lines are ignored.
# This keeps the metadata readable without relying on a JSON parser in CMake.
function(_ve_parse_shader_variants VARIANT_FILE OUT_NAMES OUT_DEFINES)
    if(NOT EXISTS ${VARIANT_FILE})
        set(${OUT_NAMES} "" PARENT_SCOPE)
        set(${OUT_DEFINES} "" PARENT_SCOPE)
        return()
    endif()

    file(STRINGS ${VARIANT_FILE} _variant_lines)

    set(_variant_names)
    set(_variant_defines)

    foreach(_raw_line ${_variant_lines})
        string(STRIP "${_raw_line}" _line)
        # Skip empty and comment-only lines before doing any heavy lifting.
        if(_line STREQUAL "")
            continue()
        endif()
        if(_line MATCHES "^#")
            continue()
        endif()

        # Ensure the line starts with the keyword "variant" (case-insensitive).
        string(REGEX REPLACE "^[Vv][Aa][Rr][Ii][Aa][Nn][Tt][ \t]+" "" _rest "${_line}")
        if(_rest STREQUAL "${_line}")
            message(WARNING "Ignoring malformed variant entry in ${VARIANT_FILE}: ${_line}")
            continue()
        endif()

        # Collapse repeated whitespace so names/defines split predictably.
        string(REGEX REPLACE "[ \t]+" " " _rest "${_rest}")
        string(STRIP "${_rest}" _rest)
        if(_rest STREQUAL "")
            message(WARNING "Variant entry missing name in ${VARIANT_FILE}")
            continue()
        endif()

    # Token layout: first entry is the variant name, any remaining tokens are defines.
    string(REPLACE " " ";" _tokens "${_rest}")
        list(GET _tokens 0 _variant_name)
        list(LENGTH _tokens _token_count)

        if(_token_count GREATER 1)
            list(SUBLIST _tokens 1 -1 _define_list)
        else()
            set(_define_list)
        endif()

        if(_define_list)
            list(JOIN _define_list "|" _define_joined)
        else()
            set(_define_joined "")
        endif()

        # Cache the variant and its defines for the caller.
        list(APPEND _variant_names "${_variant_name}")
        list(APPEND _variant_defines "${_define_joined}")
    endforeach()

    set(${OUT_NAMES} "${_variant_names}" PARENT_SCOPE)
    set(${OUT_DEFINES} "${_variant_defines}" PARENT_SCOPE)
endfunction()

# Function to compile shaders automatically
function(compile_shaders TARGET_NAME)
    set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    set(SHADER_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})
    
    file(GLOB_RECURSE GLSL_SOURCE_FILES
        "${SHADER_SOURCE_DIR}/*.frag"
        "${SHADER_SOURCE_DIR}/*.vert"
        "${SHADER_SOURCE_DIR}/*.comp"
        "${SHADER_SOURCE_DIR}/*.geom"
        "${SHADER_SOURCE_DIR}/*.tesc"
        "${SHADER_SOURCE_DIR}/*.tese"
        "${SHADER_SOURCE_DIR}/*.mesh"
        "${SHADER_SOURCE_DIR}/*.task"
        "${SHADER_SOURCE_DIR}/*.rgen"
        "${SHADER_SOURCE_DIR}/*.rchit"
        "${SHADER_SOURCE_DIR}/*.rmiss"
    )

    set(SPIRV_BINARY_FILES)
    
    foreach(GLSL ${GLSL_SOURCE_FILES})
        get_filename_component(FILE_NAME ${GLSL} NAME)
        get_filename_component(FILE_STEM ${GLSL} NAME_WE)
        get_filename_component(FILE_DIR ${GLSL} DIRECTORY)
        get_filename_component(FILE_EXT ${GLSL} EXT)

    # Optional metadata file describing named variants and their preprocessor defines.
    set(VARIANT_FILE "${FILE_DIR}/${FILE_STEM}.variants")
        set(SHADER_VARIANTS_HANDLED FALSE)
        set(PARSED_VARIANT_NAMES)
        set(PARSED_VARIANT_DEFINES)

        if(EXISTS ${VARIANT_FILE})
            _ve_parse_shader_variants(${VARIANT_FILE} PARSED_VARIANT_NAMES PARSED_VARIANT_DEFINES)
            if(PARSED_VARIANT_NAMES)
                set(SHADER_VARIANTS_HANDLED TRUE)
                set(_variant_index 0)
                foreach(VARIANT_NAME ${PARSED_VARIANT_NAMES})
                    list(GET PARSED_VARIANT_DEFINES ${_variant_index} VARIANT_DEFINE_STRING)
                    set(VARIANT_DEFINE_ARGS)
                    if(VARIANT_DEFINE_STRING)
                        # Turn each recorded define into a -D flag for the toolchain.
                        string(REPLACE "|" ";" VARIANT_DEFINE_LIST "${VARIANT_DEFINE_STRING}")
                        foreach(VARIANT_DEFINE ${VARIANT_DEFINE_LIST})
                            if(VARIANT_DEFINE)
                                list(APPEND VARIANT_DEFINE_ARGS "-D${VARIANT_DEFINE}")
                            endif()
                        endforeach()
                    endif()

                    # Sanitize the variant name so it is safe inside the output filename.
                    string(REGEX REPLACE "[^A-Za-z0-9_\-]" "_" VARIANT_SUFFIX "${VARIANT_NAME}")
                    if(VARIANT_SUFFIX STREQUAL "")
                        set(VARIANT_SUFFIX "variant")
                    endif()

                    # Encode the variant suffix before the GLSL extension to keep lookups simple.
                    set(SPIRV "${SHADER_BINARY_DIR}/${FILE_STEM}_${VARIANT_SUFFIX}${FILE_EXT}.spv")

                    if(GLSLANG_VALIDATOR)
                        add_custom_command(
                            OUTPUT ${SPIRV}
                            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_BINARY_DIR}"
                            COMMAND ${GLSLANG_VALIDATOR} -V --quiet ${VARIANT_DEFINE_ARGS} -o ${SPIRV} ${GLSL}
                            DEPENDS ${GLSL} ${VARIANT_FILE}
                            COMMENT "Compiling shader variant: ${FILE_NAME} [${VARIANT_NAME}]"
                        )
                    else()
                        add_custom_command(
                            OUTPUT ${SPIRV}
                            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_BINARY_DIR}"
                            COMMAND ${GLSLC_EXECUTABLE} ${VARIANT_DEFINE_ARGS} ${GLSL} -o ${SPIRV}
                            DEPENDS ${GLSL} ${VARIANT_FILE}
                            COMMENT "Compiling shader variant: ${FILE_NAME} [${VARIANT_NAME}]"
                        )
                    endif()

                    list(APPEND SPIRV_BINARY_FILES ${SPIRV})
                    math(EXPR _variant_index "${_variant_index} + 1")
                endforeach()
            else()
                message(WARNING "Shader variant metadata in ${VARIANT_FILE} did not yield any variants; falling back to default compilation.")
            endif()
        endif()

        if(NOT SHADER_VARIANTS_HANDLED)
            # No variants -> produce a single SPIR-V artefact matching the GLSL filename.
            set(SPIRV "${SHADER_BINARY_DIR}/${FILE_NAME}.spv")

            if(GLSLANG_VALIDATOR)
                add_custom_command(
                    OUTPUT ${SPIRV}
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_BINARY_DIR}"
                    COMMAND ${GLSLANG_VALIDATOR} -V --quiet -o ${SPIRV} ${GLSL}
                    DEPENDS ${GLSL}
                    COMMENT "Compiling shader: ${FILE_NAME}"
                )
            else()
                add_custom_command(
                    OUTPUT ${SPIRV}
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_BINARY_DIR}"
                    COMMAND ${GLSLC_EXECUTABLE} ${GLSL} -o ${SPIRV}
                    DEPENDS ${GLSL}
                    COMMENT "Compiling shader: ${FILE_NAME}"
                )
            endif()

            list(APPEND SPIRV_BINARY_FILES ${SPIRV})
        endif()
    endforeach(GLSL)
    
    add_custom_target(${TARGET_NAME}
        DEPENDS ${SPIRV_BINARY_FILES}
        COMMENT "Compiling shaders"
    )
    
    # Set output directory for runtime access
    set_target_properties(${TARGET_NAME} PROPERTIES
        SHADER_OUTPUT_DIR ${SHADER_BINARY_DIR}
    )
endfunction()

# Function to add shader dependency to a target
function(target_link_shaders TARGET_NAME SHADER_TARGET)
    add_dependencies(${TARGET_NAME} ${SHADER_TARGET})
    
    # Get shader output directory
    get_target_property(SHADER_DIR ${SHADER_TARGET} SHADER_OUTPUT_DIR)
    
    # Define shader directory for the target
    target_compile_definitions(${TARGET_NAME} PRIVATE 
        SHADER_DIR="$<$<CONFIG:Debug>:${SHADER_DIR}>$<$<CONFIG:Release>:${SHADER_DIR}>$<$<CONFIG:RelWithDebInfo>:${SHADER_DIR}>$<$<CONFIG:MinSizeRel>:${SHADER_DIR}>"
    )
endfunction()
