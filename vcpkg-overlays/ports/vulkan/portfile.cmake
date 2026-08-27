# =============================================================================
# Overlay port: vulkan (headers-only variant)
#
# This overlay overrides the upstream vcpkg "vulkan" meta-port from
# /home/aronhoy/vcpkg/ports/vulkan (see vcpkg-configuration.json ->
# "overlay-ports").
#
# WHY THIS OVERLAY EXISTS:
#   The upstream meta-port depends on both `vulkan-headers` AND
#   `vulkan-loader`, so every port that uses the `vulkan` meta-port (notably
#   imgui's `vulkan-binding` feature) force-built a vcpkg-compiled copy of the
#   Vulkan loader. That is unnecessary: the loader is a runtime dependency
#   resolved at runtime (SDL loads libvulkan.so.1 dynamically), and the app
#   links the system loader from the distro (/usr/lib64/libvulkan.so.1).
#
#   Bundling a second, in-tree loader (vcpkg 1.4.309) alongside the system
#   loader (1.4.341) caused a dual-loader crash when RenderDoc armed its
#   capture layer on launch. This overlay prunes the `vulkan-loader`
#   dependency edge so only headers are installed, and the detection step
#   below validates that a Vulkan implementation (the system one) is present.
#
#   Any project relying on the upstream behaviour can still find a loader at
#   configure/link time through CMake's built-in FindVulkan module, which
#   resolves the system library (see ../usage).
# =============================================================================

set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

set(vulkan_result_file "${CURRENT_BUILDTREES_DIR}/vulkan-${TARGET_TRIPLET}.cmake.log")
vcpkg_cmake_configure(
    SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}"
    OPTIONS_RELEASE
        "-DOUTFILE=${vulkan_result_file}"
)

# Detection step: run find_package(Vulkan) in a throwaway CMake project and
# fail the port if no implementation (loader + headers) is found. This is the
# upstream port's behaviour and is what guarantees the system loader exists,
# since we no longer pull vulkan-loader in ourselves.
include("${vulkan_result_file}")
if(DETECTED_Vulkan_FOUND)
    message(STATUS "Found Vulkan ${DETECTED_Vulkan_VERSION} (${DETECTED_Vulkan_LIBRARIES})")
else()
    set(message "Vulkan wasn't found.")
    if(VCPKG_TARGET_IS_ANDROID AND DETECTED_ANDROID_NATIVE_API_LEVEL AND DETECTED_ANDROID_NATIVE_API_LEVEL LESS "24")
        string(APPEND message " Vulkan support from the Android NDK requires API level 24 (found: ${DETECTED_ANDROID_NATIVE_API_LEVEL})")
    endif()
    message(FATAL_ERROR "${message}")
endif()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
             "${CMAKE_CURRENT_LIST_DIR}/vulkan-result.cmake.in"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/detect-vulkan"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" [[
This is a stub package. Copyright and license information
is provided with Vulkan headers and loader.
For Android, the loader is provided by the NDK.
]])
