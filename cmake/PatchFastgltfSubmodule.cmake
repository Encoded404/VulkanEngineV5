# PatchFastgltfSubmodule.cmake
#
# Copies the fastgltf git submodule source to the build directory and applies
# the necessary patches at configure time, so the original submodule tree is
# never modified.
#
# Usage:
#   include(PatchFastgltfSubmodule)
#   patch_fastgltf_submodule(
#       SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/fastgltf"
#       PATCHED_DIR_VAR PATCHED_FASTGLTF_DIR
#   )
#   add_subdirectory(${PATCHED_FASTGLTF_DIR} ...)
#

function(patch_fastgltf_submodule)
    set(oneValueArgs SOURCE_DIR PATCHED_DIR_VAR)
    cmake_parse_arguments(PFS "" "${oneValueArgs}" "" ${ARGN})

    if(NOT PFS_SOURCE_DIR)
        message(FATAL_ERROR "patch_fastgltf_submodule: SOURCE_DIR is required")
    endif()
    if(NOT PFS_PATCHED_DIR_VAR)
        message(FATAL_ERROR "patch_fastgltf_submodule: PATCHED_DIR_VAR is required")
    endif()

    set(_patched_dir "${CMAKE_BINARY_DIR}/_deps/fastgltf_patched")

    # Only copy + patch on the very first configure for this build tree.
    if(NOT EXISTS "${_patched_dir}/CMakeLists.txt")
        message(STATUS "patch_fastgltf_submodule: copying fastgltf to build tree for patching ...")

        # ---- copy ----
        file(COPY "${PFS_SOURCE_DIR}" DESTINATION "${CMAKE_BINARY_DIR}/_deps")
        file(RENAME "${CMAKE_BINARY_DIR}/_deps/fastgltf" "${_patched_dir}")

        # ---- patch CMakeLists.txt: 'LANGUAGES C CXX' -> 'LANGUAGES CXX' ----
        file(READ "${_patched_dir}/CMakeLists.txt" _fg_cmake)
        string(REPLACE "LANGUAGES C CXX" "LANGUAGES CXX" _fg_cmake "${_fg_cmake}")
        file(WRITE "${_patched_dir}/CMakeLists.txt" "${_fg_cmake}")

        # ---- patch headers: fix TU-local entity errors for Clang C++20 modules ----
        foreach(_file util.hpp types.hpp core.hpp)
            file(READ "${_patched_dir}/include/fastgltf/${_file}" _content)
            # Turn 'static constexpr' into 'inline constexpr' at namespace scope.
            string(REGEX REPLACE "\n(    )?static constexpr" "\n\\1inline constexpr" _content "${_content}")
            # Turn free-standing 'constexpr std::string_view mimeType* =' into 'inline constexpr …'.
            string(REGEX REPLACE "\n(constexpr std::string_view mimeType[A-Za-z]+ = )" "\ninline \\1" _content "${_content}")
            file(WRITE "${_patched_dir}/include/fastgltf/${_file}" "${_content}")
        endforeach()

        message(STATUS "patch_fastgltf_submodule: done → ${_patched_dir}")
    endif()

    set(${PFS_PATCHED_DIR_VAR} "${_patched_dir}" PARENT_SCOPE)
endfunction()
