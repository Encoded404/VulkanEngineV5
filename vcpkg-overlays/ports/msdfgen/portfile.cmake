# Overlay port (see vcpkg-overlays/ports/msdfgen in VulkanEngineV5).
#
# This is the upstream vcpkg msdfgen port with ONE addition: the linkage check
# below. The rest is verbatim.
#
# Why: upstream does not pin the linkage, so on a dynamic triplet (x64-mingw-
# libcxx) msdfgen is built as a DLL. That DLL exports nothing -- its import
# library carries only the import descriptor and no msdfgen symbol at all,
# because msdfgen's CMake build never defines the dllexport half of
# MSDFGEN_PUBLIC that its installed config tells consumers to treat as
# dllimport. Every consumer then fails to link with undefined symbols for the
# whole msdfgen:: namespace, which is exactly what the Windows cross build does.
#
# Forcing a static library is the smallest correct fix and matches how this
# engine consumes msdfgen: it uses the C++ core API directly and wants the code
# linked in, not a DLL to deploy. Verified: the static archive defines the
# symbols (libmsdfgen-core.a) while the shared import library defined none.
#
# To retire this overlay: move the registry baseline past a msdfgen port that
# pins the linkage itself (or that fixes the exports), then delete this
# directory.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Chlumsky/msdfgen
    REF "v${VERSION}"
    SHA512 ad02b8b18b4c3329d1c3e9468dedb6ea45b1691817a970f84c4c6dbfd47e2b3483710810256a2714dece6f000931125fdef9fc6bad12f40667a623f7a5f06c9a
    HEAD_REF master
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        openmp MSDFGEN_USE_OPENMP
        geometry-preprocessing MSDFGEN_USE_SKIA
        tools MSDFGEN_BUILD_STANDALONE
    INVERTED_FEATURES
        extensions MSDFGEN_CORE_ONLY
)

if (VCPKG_CRT_LINKAGE STREQUAL dynamic)
    set(MSDFGEN_DYNAMIC_RUNTIME ON)
else()
    set(MSDFGEN_DYNAMIC_RUNTIME OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMSDFGEN_USE_VCPKG=ON
        -DMSDFGEN_VCPKG_FEATURES_SET=ON
        -DMSDFGEN_INSTALL=ON
        -DMSDFGEN_DYNAMIC_RUNTIME="${MSDFGEN_DYNAMIC_RUNTIME}"
        ${FEATURE_OPTIONS}
    MAYBE_UNUSED_VARIABLES
        MSDFGEN_VCPKG_FEATURES_SET
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/msdfgen)

# move exe to tools
if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES msdfgen AUTO_CLEAN)
endif()

# cleanup
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# license
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
