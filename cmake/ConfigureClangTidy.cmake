find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)

if(NOT CLANG_TIDY_EXECUTABLE)
    message(WARNING "Clang-Tidy not configured: 'clang-tidy' executable not found.")
endif()

# Function to enable clang-tidy for a specific target.
# Usage: enable_target_clang_tidy(<target> [HEADER_FILTER <regex>])
# If no HEADER_FILTER is supplied the default is ^${CMAKE_SOURCE_DIR}/src/.
function(enable_target_clang_tidy target)
    cmake_parse_arguments(CT "" "HEADER_FILTER" "" ${ARGN})

    if(NOT CLANG_TIDY_EXECUTABLE)
        message(WARNING "Cannot enable clang-tidy for '${target}': 'clang-tidy' not found.")
        return()
    endif()

    if(NOT TARGET ${target})
        message(WARNING "Cannot enable clang-tidy: target '${target}' does not exist.")
        return()
    endif()

    set(CLANG_TIDY_CMD "${CLANG_TIDY_EXECUTABLE};-p=${CMAKE_BINARY_DIR}")
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_CMD}")
    message(STATUS "Clang-Tidy enabled for target '${target}'.")
endfunction()