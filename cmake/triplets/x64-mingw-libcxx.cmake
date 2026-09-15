# Windows x86_64 target built from Linux with llvm-mingw (Clang + libc++).
#
# - VCPKG_CMAKE_SYSTEM_NAME=MinGW makes vcpkg treat the target as Windows
#   (VCPKG_TARGET_IS_WINDOWS / VCPKG_TARGET_IS_MINGW), which is what port
#   logic and `supports` platform expressions expect for a MinGW toolchain.
# - The chainloaded toolchain (cmake/toolchains/llvm-mingw.cmake) replaces
#   vcpkg's default scripts/toolchains/mingw.cmake and points the compilers at
#   llvm-mingw's per-target Clang wrappers.
#
# Dynamic linkage is required because the shader-slang port only offers a
# shared library (vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)). Linking every
# dependency dynamically is also the norm on Windows.
#
# Select with: -DVCPKG_TARGET_TRIPLET=x64-mingw-libcxx
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME MinGW)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/llvm-mingw.cmake")

# Newer Clang (>= 21) diagnoses `-ffp-contract=off` after `-ffp-model=precise`
# with -Woverriding-option. ktx's bundled astcenc passes both and builds with
# -Werror, which turns that diagnostic into a hard error. Disable it for all
# ports (the same flag is already used by the native clang-libcxx toolchain).
# A fully-disabling -Wno-<diag> is not resurrected by a later -Werror.
set(VCPKG_CXX_FLAGS "-Wno-overriding-option")
set(VCPKG_C_FLAGS "-Wno-overriding-option")

# The Vulkan-Loader (pulled in for the Windows target by imgui's vulkan-binding
# feature) defaults to its MASM path when USE_GAS is unset. MASM needs ml64.exe,
# which does not exist on a Linux host, so the port would fail during
# enable_language(ASM_MASM). Force the GAS path, which clang's integrated
# assembler handles fine for the mingw target.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DUSE_GAS=ON")
