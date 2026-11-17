include_guard(GLOBAL)

# Generates the explorer's downloadable-asset catalog header and the credits
# folder that ships next to the executable, both from one JSON file.
#
# The catalog JSON is the single source of truth: nothing about a downloadable
# asset is written down twice. Configure time validates every entry and fails
# loudly, so a malformed entry cannot reach a build; build time copies the
# staging folder into the per-configuration bin directory (see
# AddEngineExample.cmake, which owns that step because only it knows the target
# layout).
#
# Parameters, all one-value:
#   TARGET          target name, used as the message prefix
#   CATALOG         path to catalog.json
#   LICENSE_DIR     directory holding <SPDX>.txt license texts
#   PROJECT_LICENSE path to the project's own license, copied as credits/LICENSE
#   HEADER_OUT      path of the generated C++ header
#   CREDITS_OUT     path of the staging credits folder
#
# Generated text is assembled with string(APPEND) on purpose: a CMake list
# cannot hold blank lines or a literal ';', and both appear in the output.

# Reads `json` at the path in ARGN into `out`. Fails the configure when the path
# is missing, not a scalar, or the wrong type, naming the entry and the field.
function(_asset_catalog_json_get out json context)
    string(JSON _value ERROR_VARIABLE _error GET "${json}" ${ARGN})
    if(_error)
        string(JOIN "." _path ${ARGN})
        message(FATAL_ERROR "${context} has no readable '${_path}': ${_error}")
    endif()
    set(${out} "${_value}" PARENT_SCOPE)
endfunction()

# Rejects empty values and anything that would break line-oriented output: the
# generated header holds one single-line literal per field, and CREDITS.md is
# Markdown, so a newline or another control character must fail here instead of
# producing a mangled file.
function(_asset_catalog_require_printable prefix where value)
    if("${value}" STREQUAL "")
        message(FATAL_ERROR "${prefix}: ${where} is empty")
    endif()
    foreach(_code RANGE 1 31)
        string(ASCII ${_code} _character)
        string(FIND "${value}" "${_character}" _found)
        if(NOT _found EQUAL -1)
            message(FATAL_ERROR "${prefix}: ${where} contains a control character")
        endif()
    endforeach()
    string(ASCII 127 _delete)
    string(FIND "${value}" "${_delete}" _found)
    if(NOT _found EQUAL -1)
        message(FATAL_ERROR "${prefix}: ${where} contains a control character")
    endif()
endfunction()

# Escapes a value so it can sit inside a C++ string literal.
function(_asset_catalog_escape_cpp out value)
    string(REPLACE "\\" "\\\\" _escaped "${value}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    set(${out} "${_escaped}" PARENT_SCOPE)
endfunction()

# Escapes a value so it cannot break out of a Markdown table cell.
function(_asset_catalog_escape_markdown out value)
    string(REPLACE "|" "\\|" _escaped "${value}")
    set(${out} "${_escaped}" PARENT_SCOPE)
endfunction()

# Renders a byte count the way the explorer's UI does, plus the exact number so
# the credits table and `--list-downloads` can be compared literally. The tenth
# is rounded, not truncated, because the UI's std::format("{:.1f}") rounds: a
# credits row that says 5.1 MiB where the panel says 5.2 MiB would look like a
# bug in the catalog.
function(_asset_catalog_format_size out bytes)
    if(bytes LESS 1024)
        set(_text "${bytes} B (${bytes} bytes)")
    elseif(bytes LESS 1048576)
        math(EXPR _whole "${bytes} / 1024")
        math(EXPR _remainder "${bytes} % 1024")
        math(EXPR _tenth "(${_remainder} * 10 + 512) / 1024")
        if(_tenth GREATER 9)
            # Rounding the tenth can carry into the whole unit, which the tenth
            # digit alone cannot express.
            math(EXPR _whole "${_whole} + 1")
            set(_tenth 0)
        endif()
        set(_text "${_whole}.${_tenth} KiB (${bytes} bytes)")
    else()
        math(EXPR _whole "${bytes} / 1048576")
        math(EXPR _remainder "${bytes} % 1048576")
        math(EXPR _tenth "(${_remainder} * 10 + 524288) / 1048576")
        if(_tenth GREATER 9)
            math(EXPR _whole "${_whole} + 1")
            set(_tenth 0)
        endif()
        set(_text "${_whole}.${_tenth} MiB (${bytes} bytes)")
    endif()
    set(${out} "${_text}" PARENT_SCOPE)
endfunction()

function(generate_asset_credits)
    cmake_parse_arguments(GEN ""
        "TARGET;CATALOG;LICENSE_DIR;PROJECT_LICENSE;HEADER_OUT;CREDITS_OUT" "" ${ARGN})

    if(NOT GEN_TARGET)
        set(GEN_TARGET "asset catalog")
    endif()
    set(_prefix "${GEN_TARGET}")

    if(NOT GEN_CATALOG OR NOT GEN_LICENSE_DIR OR NOT GEN_PROJECT_LICENSE
       OR NOT GEN_HEADER_OUT OR NOT GEN_CREDITS_OUT)
        message(FATAL_ERROR
            "generate_asset_credits: CATALOG, LICENSE_DIR, PROJECT_LICENSE, HEADER_OUT "
            "and CREDITS_OUT are all required")
    endif()
    if(NOT EXISTS "${GEN_CATALOG}")
        message(FATAL_ERROR "${_prefix}: catalog '${GEN_CATALOG}' does not exist")
    endif()
    if(NOT EXISTS "${GEN_PROJECT_LICENSE}")
        message(FATAL_ERROR
            "${_prefix}: project license '${GEN_PROJECT_LICENSE}' does not exist")
    endif()

    file(READ "${GEN_CATALOG}" _json)

    _asset_catalog_json_get(_version "${_json}" "${_prefix}: catalog" version)
    if(NOT _version STREQUAL "1")
        message(FATAL_ERROR
            "${_prefix}: catalog 'version' is '${_version}', but this generator only "
            "understands version 1")
    endif()

    string(JSON _count ERROR_VARIABLE _error LENGTH "${_json}" assets)
    if(_error)
        message(FATAL_ERROR "${_prefix}: catalog has no 'assets' array: ${_error}")
    endif()
    if(_count EQUAL 0)
        message(FATAL_ERROR
            "${_prefix}: catalog '${GEN_CATALOG}' has no assets; the explorer needs at "
            "least one entry")
    endif()

    # The license policy the downloader enforces. Keep this list in step with
    # IsPermissiveLicense in examples/explorer/Download.cpp.
    set(_permissive_licenses CC0-1.0 CC-BY-4.0 CC-BY-3.0 PUBLIC-DOMAIN)

    # Recreate the staging folder from scratch: a credits file whose catalog
    # entry was removed must disappear, not linger next to a build that no
    # longer mentions it.
    file(REMOVE_RECURSE "${GEN_CREDITS_OUT}")
    file(MAKE_DIRECTORY "${GEN_CREDITS_OUT}" "${GEN_CREDITS_OUT}/assets"
         "${GEN_CREDITS_OUT}/licenses")

    set(_header_entries "")
    set(_credits_rows "")
    set(_seen_ids "")
    set(_seen_files "")
    set(_required_licenses "")

    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 0 ${_last})
        set(_context "${_prefix}: catalog entry ${_index}")
        _asset_catalog_json_get(_id "${_json}" "${_context}" assets ${_index} id)
        _asset_catalog_require_printable("${_prefix}" "${_context} 'id'" "${_id}")
        set(_context "${_prefix}: catalog entry ${_index} ('${_id}')")

        _asset_catalog_json_get(_name "${_json}" "${_context}" assets ${_index} name)
        _asset_catalog_json_get(_category "${_json}" "${_context}" assets ${_index} category)
        _asset_catalog_json_get(_summary "${_json}" "${_context}" assets ${_index} summary)
        _asset_catalog_json_get(_license "${_json}" "${_context}" assets ${_index} license)
        _asset_catalog_json_get(_attribution "${_json}" "${_context}" assets ${_index} attribution)
        _asset_catalog_json_get(_source_page "${_json}" "${_context}" assets ${_index} source_page)
        _asset_catalog_json_get(_url "${_json}" "${_context}" assets ${_index} url)
        _asset_catalog_json_get(_file_name "${_json}" "${_context}" assets ${_index} file_name)
        _asset_catalog_json_get(_size "${_json}" "${_context}" assets ${_index} size)
        _asset_catalog_json_get(_sha256 "${_json}" "${_context}" assets ${_index} sha256)

        foreach(_field id name category summary license attribution source_page url file_name sha256)
            set(_field_value "${_${_field}}")
            _asset_catalog_require_printable("${_prefix}" "${_context} '${_field}'"
                                             "${_field_value}")
        endforeach()

        if(_id MATCHES "[/\\\\]")
            message(FATAL_ERROR "${_context} 'id' contains a path separator")
        endif()
        if(_id STREQUAL "." OR _id STREQUAL "..")
            message(FATAL_ERROR "${_context} 'id' is not a usable file name stem")
        endif()
        if(NOT _license IN_LIST _permissive_licenses)
            message(FATAL_ERROR
                "${_context} license '${_license}' is not permissive; allowed values: "
                "${_permissive_licenses}")
        endif()
        if(NOT EXISTS "${GEN_LICENSE_DIR}/${_license}.txt")
            message(FATAL_ERROR
                "${_context} has no license text at '${GEN_LICENSE_DIR}/${_license}.txt'")
        endif()
        if(NOT _url MATCHES "^https://")
            message(FATAL_ERROR "${_context} 'url' is not an HTTPS address")
        endif()
        if(_url MATCHES "/(main|master)/")
            message(FATAL_ERROR
                "${_context} 'url' points at a branch; pin a commit so the checksum keeps "
                "its meaning")
        endif()
        if(_file_name MATCHES "[/\\\\]")
            message(FATAL_ERROR "${_context} 'file_name' contains a path separator")
        endif()
        if(_file_name STREQUAL "." OR _file_name STREQUAL "..")
            message(FATAL_ERROR "${_context} 'file_name' is not a usable file name")
        endif()
        if(NOT _size MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "${_context} 'size' is not a positive integer: '${_size}'")
        endif()
        if(NOT _sha256 MATCHES "^[0-9a-f]+$")
            message(FATAL_ERROR "${_context} 'sha256' is not lowercase hex")
        endif()
        string(LENGTH "${_sha256}" _sha_length)
        if(NOT _sha_length EQUAL 64)
            message(FATAL_ERROR
                "${_context} 'sha256' has ${_sha_length} characters, expected 64")
        endif()

        if(_id IN_LIST _seen_ids)
            message(FATAL_ERROR "${_context} repeats the id '${_id}'")
        endif()
        if(_file_name IN_LIST _seen_files)
            message(FATAL_ERROR "${_context} repeats the file name '${_file_name}'")
        endif()
        list(APPEND _seen_ids "${_id}")
        list(APPEND _seen_files "${_file_name}")
        if(NOT _license IN_LIST _required_licenses)
            list(APPEND _required_licenses "${_license}")
        endif()

        _asset_catalog_escape_cpp(_id_cpp "${_id}")
        _asset_catalog_escape_cpp(_name_cpp "${_name}")
        _asset_catalog_escape_cpp(_category_cpp "${_category}")
        _asset_catalog_escape_cpp(_summary_cpp "${_summary}")
        _asset_catalog_escape_cpp(_license_cpp "${_license}")
        _asset_catalog_escape_cpp(_attribution_cpp "${_attribution}")
        _asset_catalog_escape_cpp(_source_cpp "${_source_page}")
        _asset_catalog_escape_cpp(_url_cpp "${_url}")
        _asset_catalog_escape_cpp(_file_cpp "${_file_name}")
        _asset_catalog_escape_cpp(_sha_cpp "${_sha256}")
        string(APPEND _header_entries
            "    {\"${_id_cpp}\", \"${_name_cpp}\", \"${_category_cpp}\", \"${_summary_cpp}\", "
            "\"${_license_cpp}\", \"${_attribution_cpp}\", \"${_source_cpp}\", \"${_url_cpp}\", "
            "\"${_file_cpp}\", ${_size}ULL, \"${_sha_cpp}\"},\n")

        _asset_catalog_escape_markdown(_id_md "${_id}")
        _asset_catalog_escape_markdown(_name_md "${_name}")
        _asset_catalog_escape_markdown(_summary_md "${_summary}")
        _asset_catalog_escape_markdown(_category_md "${_category}")
        _asset_catalog_escape_markdown(_license_md "${_license}")
        _asset_catalog_escape_markdown(_attribution_md "${_attribution}")
        _asset_catalog_escape_markdown(_source_md "${_source_page}")
        _asset_catalog_escape_markdown(_url_md "${_url}")
        _asset_catalog_format_size(_size_md "${_size}")
        string(APPEND _credits_rows
            "| `${_id_md}` | ${_name_md} | ${_summary_md} | ${_category_md} | ${_license_md} | "
            "${_size_md} | ${_attribution_md} | [source page](${_source_md}) | "
            "[download](${_url_md}) |\n")

        set(_asset_file "${_name}\n")
        string(APPEND _asset_file "License: ${_license}\n")
        string(APPEND _asset_file "Attribution: ${_attribution}\n")
        string(APPEND _asset_file "Source: ${_source_page}\n")
        string(APPEND _asset_file "Downloaded from: ${_url}\n")
        string(APPEND _asset_file "SHA-256: ${_sha256}\n")
        string(APPEND _asset_file "Not bundled; downloaded on demand.\n")
        file(WRITE "${GEN_CREDITS_OUT}/assets/${_id}.txt" "${_asset_file}")
    endforeach()

    set(_header "")
    string(APPEND _header
        "// Generated from the ${GEN_TARGET} catalog JSON by cmake/GenerateAssetCredits.cmake.\n"
        "// Do not edit: edit the catalog and rebuild instead.\n"
        "//\n"
        "// This header is plain data on purpose. It is included in the global module\n"
        "// fragment of Scenes.cpp, so it must not include anything or name a standard\n"
        "// library type.\n"
        "namespace SceneExplorerCatalog {\n"
        "\n"
        "struct Entry {\n"
        "    const char* id;\n"
        "    const char* name;\n"
        "    const char* category;\n"
        "    const char* summary;\n"
        "    const char* license;\n"
        "    const char* attribution;\n"
        "    const char* source_page;\n"
        "    const char* url;\n"
        "    const char* file_name;\n"
        "    unsigned long long size;\n"
        "    const char* sha256;\n"
        "};\n"
        "\n"
        "inline constexpr Entry kEntries[] = {\n")
    string(APPEND _header "${_header_entries}")
    string(APPEND _header
        "};\n"
        "\n"
        "inline constexpr unsigned long long kEntryCount =\n"
        "    sizeof(kEntries) / sizeof(kEntries[0]);\n"
        "\n"
        "} // namespace SceneExplorerCatalog\n")

    # Only rewrite the header when it changed: every recompile of the example
    # costs a module rebuild, and content equality is the honest test (a
    # timestamp would change on every configure).
    set(_existing_header "")
    if(EXISTS "${GEN_HEADER_OUT}")
        file(READ "${GEN_HEADER_OUT}" _existing_header)
    endif()
    if(NOT _existing_header STREQUAL _header)
        file(WRITE "${GEN_HEADER_OUT}" "${_header}")
    endif()

    set(_credits_md "# Asset credits\n")
    string(APPEND _credits_md
        "\n"
        "This folder belongs to the Scene Explorer example. It lists every sample asset the\n"
        "explorer can download, and it is generated on every build from the catalog JSON at\n"
        "`examples/scene_explorer/catalog.json`. Do not edit it by hand.\n"
        "\n"
        "No asset bytes ship with the executable. Each file is fetched from its pinned URL\n"
        "when the user asks for it, checked against the exact byte size and the SHA-256 in\n"
        "the table below, and stored in the per-user cache. A downloaded file also gets a\n"
        "`.license.txt` sidecar in that cache, so the copy the user actually has keeps its\n"
        "attribution.\n"
        "\n"
        "## Assets\n"
        "\n"
        "| id | name | summary | category | license | size | attribution | source page | "
        "download |\n"
        "|---|---|---|---|---|---|---|---|---|\n")
    string(APPEND _credits_md "${_credits_rows}")
    string(APPEND _credits_md
        "\n"
        "## License texts\n"
        "\n"
        "Every license named above has its verbatim text in `licenses/`.\n"
        "\n"
        "- `LICENSE` is the license of the example itself.\n"
        "- `licenses/<SPDX>.txt` is the verbatim text of one asset license.\n")
    file(WRITE "${GEN_CREDITS_OUT}/CREDITS.md" "${_credits_md}")

    foreach(_license IN LISTS _required_licenses)
        file(COPY_FILE "${GEN_LICENSE_DIR}/${_license}.txt"
             "${GEN_CREDITS_OUT}/licenses/${_license}.txt")
    endforeach()
    file(COPY_FILE "${GEN_PROJECT_LICENSE}" "${GEN_CREDITS_OUT}/LICENSE")
endfunction()
