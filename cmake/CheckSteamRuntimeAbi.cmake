include_guard(GLOBAL)

# Adds a post-link ABI compatibility check for Steam Linux Runtime targets.
#
# Valve's steamrt4 SDK defaults to linking libstdc++ and libgcc dynamically,
# and the runtime's libstdc++ can be older than the build host's. A newer
# GLIBCXX_* symbol version is a hard failure at process start, not a link
# error, so this check catches it at build time.
#
# The check is generic: a libc++ build has no GLIBCXX_/CXXABI_ symbols, so only
# its GLIBC_* ceiling is enforced (the Clang + libc++ configuration statically
# embeds libc++ in Release and never loads the runtime's libstdc++). A
# libstdc++ build is checked for all three, plus the static-libstdc++ mistake.
function(vkengine_check_steam_runtime_abi TARGET)
    if(VKENGINE_STEAM_RUNTIME STREQUAL "off")
        return()
    endif()
    if(NOT UNIX)
        return()
    endif()

    find_program(VKENGINE_READELF_TOOL
        NAMES llvm-readelf readelf
        HINTS "${CMAKE_CXX_COMPILER}/../../bin")
    if(NOT VKENGINE_READELF_TOOL)
        message(WARNING
            "Steam Runtime ABI check disabled for ${TARGET}: readelf/llvm-readelf not found.")
        return()
    endif()

    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:${TARGET}>"
            "-DRUNTIME=${VKENGINE_STEAM_RUNTIME}"
            "-DREADELF=${VKENGINE_READELF_TOOL}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckSteamRuntimeAbiScript.cmake"
        COMMENT "Checking ${TARGET} against Steam Linux Runtime ${VKENGINE_STEAM_RUNTIME} C++ ABI"
        VERBATIM)
endfunction()
