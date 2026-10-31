cmake_minimum_required(VERSION 3.30)

# Post-link Steam Linux Runtime ABI check. Invoked as a script by
# vkengine_check_steam_runtime_abi() in CheckSteamRuntimeAbi.cmake:
#
#   cmake -DEXE=<exe> -DRUNTIME=steamrt4 -DREADELF=<tool>
#         -P CheckSteamRuntimeAbiScript.cmake

if(NOT DEFINED EXE OR EXE STREQUAL "" OR NOT EXISTS "${EXE}")
    message(FATAL_ERROR "CheckSteamRuntimeAbi: EXE is missing or does not exist: '${EXE}'")
endif()
if(NOT DEFINED RUNTIME OR RUNTIME STREQUAL "")
    message(FATAL_ERROR "CheckSteamRuntimeAbi: RUNTIME is required")
endif()
if(NOT DEFINED READELF OR READELF STREQUAL "")
    message(FATAL_ERROR "CheckSteamRuntimeAbi: READELF is required")
endif()

# Oldest libstdc++/libc ABI each runtime guarantees. The runtime can use a
# newer host library when present, but it cannot supply symbols that do not
# exist in its own copy.
if(RUNTIME STREQUAL "steamrt4")
    set(MAX_GLIBCXX "3.4.33")   # Debian 13: GCC 14
    set(MAX_CXXABI  "1.3.15")
    set(MAX_GLIBC   "2.41")
elseif(RUNTIME STREQUAL "sniper")
    set(MAX_GLIBCXX "3.4.28")   # Debian 11: GCC 10
    set(MAX_CXXABI  "1.3.13")
    set(MAX_GLIBC   "2.31")
elseif(RUNTIME STREQUAL "scout")
    set(MAX_GLIBCXX "3.4.16")   # Ubuntu 12.04: GCC 4.6
    set(MAX_CXXABI  "1.3.6")
    set(MAX_GLIBC   "2.15")
else()
    message(FATAL_ERROR "CheckSteamRuntimeAbi: unknown RUNTIME '${RUNTIME}'")
endif()

execute_process(
    COMMAND "${READELF}" --dyn-syms --wide "${EXE}"
    OUTPUT_VARIABLE _syms
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "CheckSteamRuntimeAbi: '${READELF}' failed on ${EXE}: ${_err}")
endif()

execute_process(
    COMMAND "${READELF}" -d "${EXE}"
    OUTPUT_VARIABLE _dynamic
    ERROR_QUIET
    RESULT_VARIABLE _dyn_rc)
if(NOT _dyn_rc EQUAL 0)
    set(_dynamic "")
endif()

function(_vkengine_version_greater LHS RHS OUT)
    string(REPLACE "." ";" _lhs "${LHS}")
    string(REPLACE "." ";" _rhs "${RHS}")
    set(_result FALSE)
    list(LENGTH _lhs _lhsl)
    list(LENGTH _rhs _rhsl)
    math(EXPR _last "${_lhsl} - 1")
    foreach(_i RANGE 0 ${_last})
        list(GET _lhs ${_i} _lv)
        if(_i LESS _rhsl)
            list(GET _rhs ${_i} _rv)
        else()
            set(_rv 0)
        endif()
        if(_lv GREATER _rv)
            set(_result TRUE)
            break()
        elseif(_lv LESS _rv)
            break()
        endif()
    endforeach()
    set(${OUT} ${_result} PARENT_SCOPE)
endfunction()

function(_vkengine_check_versions KIND REGEX MAX)
    string(REGEX MATCHALL "${REGEX}" _matches "${_syms}")
    if(NOT _matches)
        return()
    endif()
    list(REMOVE_DUPLICATES _matches)
    set(_found "0.0.0")
    foreach(_v IN LISTS _matches)
        string(REGEX REPLACE "^${KIND}_" "" _v "${_v}")
        _vkengine_version_greater("${_v}" "${_found}" _gt)
        if(_gt)
            set(_found "${_v}")
        endif()
    endforeach()
    _vkengine_version_greater("${_found}" "${MAX}" _too_new)
    if(_too_new)
        message(FATAL_ERROR
            "${EXE}\n"
            "  requires ${KIND}_${_found}, but Steam Linux Runtime ${RUNTIME} provides "
            "at most ${KIND}_${MAX}.\n"
            "  This binary would fail at startup with:\n"
            "    version '${KIND}_${_found}' not found (required by ...)\n"
            "  Build with the runtime's SDK compiler (steamrt4: GCC 14), or "
            "recompile the offending code against older library headers. "
            "Do not bundle libstdc++ to work around it; the runtime is meant to "
            "supply libstdc++ dynamically.")
    endif()
endfunction()

_vkengine_check_versions("GLIBCXX" "GLIBCXX_[0-9]+(\\.[0-9]+)+" "${MAX_GLIBCXX}")
_vkengine_check_versions("CXXABI"  "CXXABI_[0-9]+(\\.[0-9]+)+"  "${MAX_CXXABI}")
_vkengine_check_versions("GLIBC"   "GLIBC_[0-9]+(\\.[0-9]+)+"   "${MAX_GLIBC}")

# If the executable uses libstdc++ symbols but has no DT_NEEDED entry for the
# shared library, the C++ runtime was linked statically. steamrt4 expects the
# runtime's shared libstdc++ to be used, and a static copy inside a game can
# conflict with the graphics-driver stack.
if(_syms MATCHES "GLIBCXX_" AND NOT _dynamic MATCHES "libstdc\\+\\+\\.so\\.6")
    message(FATAL_ERROR
        "${EXE}\n"
        "  appears to link libstdc++ statically (GLIBCXX symbols present, but no "
        "DT_NEEDED entry for libstdc++.so.6).\n"
        "  Steam Linux Runtime expects the game to link libstdc++ dynamically. "
        "Remove -static-libstdc++ (and -static-libgcc) for Steam builds.")
endif()

get_filename_component(_exe_name "${EXE}" NAME)
if(_syms MATCHES "GLIBCXX_")
    message(STATUS
        "Steam Runtime ABI check passed: ${_exe_name} (${RUNTIME}: "
        "GLIBCXX <= ${MAX_GLIBCXX}, CXXABI <= ${MAX_CXXABI}, GLIBC <= ${MAX_GLIBC})")
else()
    message(STATUS
        "Steam Runtime ABI check passed: ${_exe_name} (${RUNTIME}: "
        "GLIBC <= ${MAX_GLIBC}; no libstdc++ dependency)")
endif()
