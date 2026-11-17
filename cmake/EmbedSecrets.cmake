include_guard(GLOBAL)

#[=======================================================================
# embed_example_secrets
#
#   embed_example_secrets(<target>
#     SECRETS_DIR <dir>       # per-example secrets folder (may not exist)
#     MODULE      <name>      # generated C++ module name
#     NAMESPACE   <ns>        # generated C++ namespace
#     OUT_DIR     <dir>       # build-tree directory for the generated module
#   )
#
# Discovers every file in SECRETS_DIR at build time, seals each with the
# versioned DataCipher envelope, and emits one module exposing GetRaw/GetString/
# GetInt accessors. New files are picked up automatically: the glob below uses
# CONFIGURE_DEPENDS, so adding a secret re-runs CMake.
#
# A missing folder, missing keyring or empty folder produces a stub module so
# the build never depends on a developer having local secrets.
#=======================================================================]
function(embed_example_secrets TARGET)
    cmake_parse_arguments(ES "" "SECRETS_DIR;MODULE;NAMESPACE;OUT_DIR" "" ${ARGN})

    if(NOT ES_MODULE OR NOT ES_NAMESPACE OR NOT ES_OUT_DIR)
        message(FATAL_ERROR "embed_example_secrets: MODULE, NAMESPACE and OUT_DIR are required")
    endif()

    # An output directory may contain "$<CONFIG>", which file(MAKE_DIRECTORY) cannot expand: create one directory per configuration instead.
    # The directories have to exist before the generator writes into them.
    if(ES_OUT_DIR MATCHES "\\$<CONFIG>")
        if(CMAKE_CONFIGURATION_TYPES)
            set(_secrets_configs ${CMAKE_CONFIGURATION_TYPES})
        else()
            set(_secrets_configs "${CMAKE_BUILD_TYPE}")
        endif()
        foreach(_secrets_config IN LISTS _secrets_configs)
            string(REPLACE "$<CONFIG>" "${_secrets_config}" _secrets_dir "${ES_OUT_DIR}")
            file(MAKE_DIRECTORY "${_secrets_dir}")
        endforeach()
        unset(_secrets_dir)
        unset(_secrets_configs)
    else()
        file(MAKE_DIRECTORY "${ES_OUT_DIR}")
    endif()
    set(_out "${ES_OUT_DIR}/Secrets.cppm")

    # Dependency tracking. CONFIGURE_DEPENDS makes CMake re-run when the set of
    # files in the folder changes, which is what makes discovery automatic.
    set(_dep_files "")
    if(ES_SECRETS_DIR AND EXISTS "${ES_SECRETS_DIR}")
        file(GLOB _dep_files CONFIGURE_DEPENDS "${ES_SECRETS_DIR}/*")
        if(EXISTS "${ES_SECRETS_DIR}/.keyring")
            list(APPEND _dep_files "${ES_SECRETS_DIR}/.keyring")
        endif()
    endif()

    if(VKENGINE_SECRETS_GEN_COMMAND)
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${VKENGINE_SECRETS_GEN_COMMAND}"
                --in "${ES_SECRETS_DIR}"
                --out "${_out}"
                --keyring "${ES_SECRETS_DIR}/.keyring"
                --module "${ES_MODULE}"
                --namespace "${ES_NAMESPACE}"
            DEPENDS ${_dep_files} ${VKENGINE_SECRETS_GEN_DEPENDS}
            COMMENT "Sealing secrets for ${ES_NAMESPACE}"
            VERBATIM
        )
    else()
        # No generator available in this configuration: emit a stub so consumers still compile and the example runs without a leaderboard endpoint.
        # The content is assembled first because file(GENERATE) expands "$<CONFIG>" in the output path and writes one stub per configuration.
        set(_stub "// stub: secrets-gen unavailable in this configuration\nmodule;\n\n")
        string(APPEND _stub "export module ${ES_MODULE};\n\nimport std;\n\nimport VulkanEngine.DataCipher;\n\nexport namespace ${ES_NAMESPACE} {\n\n")
        string(APPEND _stub "struct Entry { std::string_view name; std::string_view aad; std::uint16_t cipher_variant; std::uint8_t key_id; std::span<const std::byte> sealed; };\n\n")
        string(APPEND _stub "inline constexpr std::array<Entry, 0> kEntries{};\n\n")
        string(APPEND _stub "[[nodiscard]] inline std::span<const Entry> Entries() noexcept { return kEntries; }\n")
        string(APPEND _stub "[[nodiscard]] inline const Entry* Find(std::string_view) noexcept { return nullptr; }\n")
        string(APPEND _stub "[[nodiscard]] inline std::optional<std::vector<std::byte>> GetRaw(std::string_view) { return std::nullopt; }\n")
        string(APPEND _stub "[[nodiscard]] inline std::optional<std::string> GetString(std::string_view) { return std::nullopt; }\n")
        string(APPEND _stub "[[nodiscard]] inline std::optional<std::int64_t> GetInt(std::string_view) { return std::nullopt; }\n\n")
        string(APPEND _stub "} // namespace ${ES_NAMESPACE}\n")
        file(GENERATE OUTPUT "${_out}" CONTENT "${_stub}")
        unset(_stub)
    endif()

    target_sources(${TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS "${ES_OUT_DIR}"
        FILES "${_out}"
    )
endfunction()
