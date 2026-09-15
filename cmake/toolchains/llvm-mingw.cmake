# =============================================================================
# Cross-compile toolchain: Linux (x86_64) -> Windows (x86_64) using the
# llvm-mingw distribution.
#
# llvm-mingw is a self-contained LLVM/Clang/LLD based mingw-w64 toolchain that
# ships clang, lld, compiler-rt, libunwind, libc++/libc++abi and a complete
# mingw-w64 sysroot (headers + CRT + import libraries), including the libc++
# standard-library module metadata (libc++.modules.json + std.cppm) required
# for `import std;`.
#
# Download: https://github.com/mstorsjo/llvm-mingw/releases
#   llvm-mingw-<version>-ucrt-ubuntu-22.04-x86_64.tar.xz
#
# Extract it into the project as <project>/toolchains/ (that directory is
# gitignored), then configure with `cmake --preset windows`. Full instructions,
# including the exact download commands, are in
# docs/cross-compiling-windows.md.
#
# This file is chainloaded in two places:
#   * from the `x64-mingw-libcxx` vcpkg triplet, which applies it to every
#     vcpkg port build; and
#   * from the `windows` configure preset via VCPKG_CHAINLOAD_TOOLCHAIN_FILE,
#     which applies it to the consuming project. vcpkg.cmake only honours that
#     variable when it is visible during the consumer configure — a value set
#     in the triplet alone is NOT propagated to the consumer.
#
# The compiler executables used are llvm-mingw's per-target *wrapper scripts*
# (e.g. bin/x86_64-w64-mingw32-clang++).  They inject `-target x86_64-w64-mingw32`
# and read the checked-in `.cfg` files, which add
#   -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++ -fuse-ld=lld
# Using the wrappers (rather than setting CMAKE_*_COMPILER_TARGET ourselves)
# also makes CMake derive the toolchain prefix `x86_64-w64-mingw32-`, so it
# automatically picks up `x86_64-w64-mingw32-clang-scan-deps`, `llvm-ar`, etc.
# =============================================================================

if(NOT _VKENGINE_LLVM_MINGW_TOOLCHAIN)
    set(_VKENGINE_LLVM_MINGW_TOOLCHAIN 1)

    # Guard against a stale build tree. CMake only (re)generates
    # CMakeFiles/<ver>/CMakeSystem.cmake when the target system changes, so a
    # build directory first configured *before* this cross toolchain was wired
    # in keeps a CMakeSystem.cmake that reports a native system and
    # CMAKE_CROSSCOMPILING=FALSE. That silently disables all cross-specific
    # logic (e.g. host-building build-time tools) while the compiler is
    # nevertheless the cross compiler. Fail loudly instead.
    if(CMAKE_SYSTEM_LOADED AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        message(FATAL_ERROR
            "Stale CMake build tree: this build directory records target system "
            "'${CMAKE_SYSTEM_NAME}' (from CMakeFiles/*/CMakeSystem.cmake) instead of "
            "'Windows', so it predates the llvm-mingw cross toolchain and has "
            "CMAKE_CROSSCOMPILING=FALSE. Delete the build directory "
            "(e.g. `rm -rf build-windows`) and configure again.")
    endif()

    if(POLICY CMP0056)
        cmake_policy(SET CMP0056 NEW)
    endif()
    if(POLICY CMP0066)
        cmake_policy(SET CMP0066 NEW)
    endif()
    if(POLICY CMP0067)
        cmake_policy(SET CMP0067 NEW)
    endif()
    if(POLICY CMP0137)
        cmake_policy(SET CMP0137 NEW)
    endif()

    # vcpkg passes its triplet variables through try_compile; keep that working.
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
        VCPKG_CRT_LINKAGE VCPKG_TARGET_ARCHITECTURE
        VCPKG_C_FLAGS VCPKG_CXX_FLAGS
        VCPKG_C_FLAGS_DEBUG VCPKG_CXX_FLAGS_DEBUG
        VCPKG_C_FLAGS_RELEASE VCPKG_CXX_FLAGS_RELEASE
        VCPKG_LINKER_FLAGS VCPKG_LINKER_FLAGS_RELEASE VCPKG_LINKER_FLAGS_DEBUG
    )

    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(CMAKE_CROSSCOMPILING OFF CACHE BOOL "")
    endif()

    # VCPKG_CMAKE_SYSTEM_NAME is "MinGW" for this triplet, but CMake has no
    # MinGW system name; force the real one (same as vcpkg's mingw.cmake).
    set(CMAKE_SYSTEM_NAME Windows CACHE STRING "" FORCE)

    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
        set(CMAKE_SYSTEM_PROCESSOR i686 CACHE STRING "")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
        set(CMAKE_SYSTEM_PROCESSOR armv7 CACHE STRING "")
    elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(CMAKE_SYSTEM_PROCESSOR aarch64 CACHE STRING "")
    else()
        set(CMAKE_SYSTEM_PROCESSOR x86_64 CACHE STRING "")
    endif()

    # ---- Locate the llvm-mingw installation -----------------------------
    # Search order:
    #   1. -DVKENGINE_LLVM_MINGW_ROOT=<dir> (or the same environment variable)
    #   2. the LLVM_MINGW_ROOT environment variable
    #   3. <project>/toolchains/llvm-mingw*  (recommended: keep it in-tree)
    #   4. ~/opt/llvm-mingw*, ~/llvm-mingw*, /opt/llvm-mingw*
    #
    # Option 3 is what docs/cross-compiling-windows.md describes: download and
    # extract the release into <project>/toolchains/, which .gitignore excludes.

    # A cached root that no longer exists (e.g. the toolchain was moved into
    # toolchains/) must not stick; drop it and re-detect.
    if(VKENGINE_LLVM_MINGW_ROOT AND
       NOT IS_DIRECTORY "${VKENGINE_LLVM_MINGW_ROOT}/bin")
        message(STATUS
            "VKENGINE_LLVM_MINGW_ROOT='${VKENGINE_LLVM_MINGW_ROOT}' is no longer a "
            "valid llvm-mingw directory; searching again.")
        unset(VKENGINE_LLVM_MINGW_ROOT)
        unset(VKENGINE_LLVM_MINGW_ROOT CACHE)
    endif()

    if(NOT VKENGINE_LLVM_MINGW_ROOT)
        if(DEFINED ENV{VKENGINE_LLVM_MINGW_ROOT})
            set(VKENGINE_LLVM_MINGW_ROOT "$ENV{VKENGINE_LLVM_MINGW_ROOT}")
        elseif(DEFINED ENV{LLVM_MINGW_ROOT})
            set(VKENGINE_LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
        else()
            get_filename_component(_vkengine_project_root
                "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
            file(GLOB _vkengine_llvm_mingw_candidates LIST_DIRECTORIES true
                "${_vkengine_project_root}/toolchains/llvm-mingw*"
                "$ENV{HOME}/opt/llvm-mingw-*"
                "$ENV{HOME}/llvm-mingw-*"
                "/opt/llvm-mingw-*"
            )
            # GLOB also matches downloaded .tar.xz files; keep only directories.
            foreach(_vkengine_candidate IN LISTS _vkengine_llvm_mingw_candidates)
                if(IS_DIRECTORY "${_vkengine_candidate}")
                    list(APPEND _vkengine_llvm_mingw_dirs "${_vkengine_candidate}")
                endif()
            endforeach()
            # The project-local candidate is listed first, so it wins.
            if(_vkengine_llvm_mingw_dirs)
                list(GET _vkengine_llvm_mingw_dirs 0 VKENGINE_LLVM_MINGW_ROOT)
            endif()
            unset(_vkengine_llvm_mingw_candidates)
            unset(_vkengine_llvm_mingw_dirs)
            unset(_vkengine_project_root)
        endif()
    endif()
    set(VKENGINE_LLVM_MINGW_ROOT "${VKENGINE_LLVM_MINGW_ROOT}" CACHE PATH
        "Root of the llvm-mingw toolchain (containing bin/, share/, <triple>/)")

    if(NOT VKENGINE_LLVM_MINGW_ROOT OR
       NOT EXISTS "${VKENGINE_LLVM_MINGW_ROOT}/bin")
        message(FATAL_ERROR
            "llvm-mingw not found. Download a release from "
            "https://github.com/mstorsjo/llvm-mingw/releases and extract it into "
            "<project>/toolchains/ (see docs/cross-compiling-windows.md), or set "
            "-DVKENGINE_LLVM_MINGW_ROOT=<dir> / the VKENGINE_LLVM_MINGW_ROOT "
            "environment variable.")
    endif()

    set(_vkengine_mingw_prefix "${CMAKE_SYSTEM_PROCESSOR}-w64-mingw32")
    set(_vkengine_mingw_bin "${VKENGINE_LLVM_MINGW_ROOT}/bin")

    # The per-target wrapper scripts own the --target/-stdlib/-rtlib flags.
    # FORCE is deliberate: a cross toolchain is authoritative for the compiler
    # choice, and it must also win over a stale CMAKE_CXX_COMPILER already
    # recorded in the build directory's CMakeCache.
    set(CMAKE_C_COMPILER   "${_vkengine_mingw_bin}/${_vkengine_mingw_prefix}-clang"
        CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER "${_vkengine_mingw_bin}/${_vkengine_mingw_prefix}-clang++"
        CACHE FILEPATH "" FORCE)
    set(CMAKE_ASM_COMPILER "${_vkengine_mingw_bin}/${_vkengine_mingw_prefix}-clang"
        CACHE FILEPATH "" FORCE)
    set(CMAKE_RC_COMPILER  "${_vkengine_mingw_bin}/${_vkengine_mingw_prefix}-windres"
        CACHE FILEPATH "" FORCE)

    set(CMAKE_AR      "${_vkengine_mingw_bin}/llvm-ar"      CACHE FILEPATH "" FORCE)
    set(CMAKE_RANLIB  "${_vkengine_mingw_bin}/llvm-ranlib"  CACHE FILEPATH "" FORCE)
    set(CMAKE_STRIP   "${_vkengine_mingw_bin}/llvm-strip"   CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJCOPY "${_vkengine_mingw_bin}/llvm-objcopy" CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJDUMP "${_vkengine_mingw_bin}/llvm-objdump" CACHE FILEPATH "" FORCE)
    set(CMAKE_NM      "${_vkengine_mingw_bin}/llvm-nm"      CACHE FILEPATH "" FORCE)

    # Module dependency scanner (used for C++20/23 named modules and import std).
    set(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS
        "${_vkengine_mingw_bin}/${_vkengine_mingw_prefix}-clang-scan-deps"
        CACHE FILEPATH "" FORCE)

    # ---- import std: point CMake at the *target* libc++ metadata ----------
    # CMake >= 4.2 honours CMAKE_CXX_STDLIB_MODULES_JSON and skips its own
    # compiler probe (`clang++ -print-file-name=libc++.modules.json`), which
    # would otherwise resolve the host's libc++ metadata during a cross build.
    set(_vkengine_libcxx_modules_json
        "${VKENGINE_LLVM_MINGW_ROOT}/${_vkengine_mingw_prefix}/lib/libc++.modules.json")
    if(EXISTS "${_vkengine_libcxx_modules_json}")
        set(CMAKE_CXX_STDLIB_MODULES_JSON "${_vkengine_libcxx_modules_json}"
            CACHE FILEPATH "Standard-library module metadata for the target libc++")
    elseif(CMAKE_VERSION VERSION_GREATER_EQUAL 4.2)
        message(WARNING
            "libc++ module metadata not found at ${_vkengine_libcxx_modules_json}; "
            "`import std;` will not be available for the Windows target.")
    endif()

    if(CMAKE_VERSION VERSION_LESS 4.2)
        message(WARNING
            "Cross-compiling this project with `import std;` requires CMake >= 4.2 "
            "(CMAKE_CXX_STDLIB_MODULES_JSON). Detected CMake ${CMAKE_VERSION}; "
            "use the CMake bundled with your IDE if it is newer.")
    endif()

    # ---- vcpkg flag plumbing (mirrors scripts/toolchains/mingw.cmake) -----
    string(APPEND CMAKE_C_FLAGS_INIT " ${VCPKG_C_FLAGS} ")
    string(APPEND CMAKE_CXX_FLAGS_INIT " ${VCPKG_CXX_FLAGS} ")
    string(APPEND CMAKE_C_FLAGS_DEBUG_INIT " ${VCPKG_C_FLAGS_DEBUG} ")
    string(APPEND CMAKE_CXX_FLAGS_DEBUG_INIT " ${VCPKG_CXX_FLAGS_DEBUG} ")
    string(APPEND CMAKE_C_FLAGS_RELEASE_INIT " ${VCPKG_C_FLAGS_RELEASE} ")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE_INIT " ${VCPKG_CXX_FLAGS_RELEASE} ")

    string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS} ")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS} ")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${VCPKG_LINKER_FLAGS} ")
    if(VCPKG_CRT_LINKAGE STREQUAL "static")
        string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT "-static ")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT "-static ")
        string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT "-static ")
    endif()
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_DEBUG_INIT " ${VCPKG_LINKER_FLAGS_DEBUG} ")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_DEBUG_INIT " ${VCPKG_LINKER_FLAGS_DEBUG} ")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_DEBUG_INIT " ${VCPKG_LINKER_FLAGS_DEBUG} ")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_RELEASE_INIT " ${VCPKG_LINKER_FLAGS_RELEASE} ")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_RELEASE_INIT " ${VCPKG_LINKER_FLAGS_RELEASE} ")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT " ${VCPKG_LINKER_FLAGS_RELEASE} ")
endif()
