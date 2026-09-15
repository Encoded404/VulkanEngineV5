# Overlay port (see vcpkg-overlays/ports/shader-slang in VulkanEngineV5).
#
# This is a copy of the upstream vcpkg shader-slang port with ONE addition at
# the bottom of the file: after the CMake config fixup we generate a
# slangTargets-debug.cmake that exports the release libraries (mirrored into
# debug/lib by the upstream portfile) as the Debug configuration of the slang
# targets. Without it, multi-config consumers link slang from lib/ in every
# configuration while other libraries are linked from debug/lib in Debug
# builds, which creates a cycle in CMake's runtime-search-path ordering and
# triggers "Cannot generate a safe runtime search path" warnings.
#
# To refresh this overlay after an upstream port update: re-copy
# ports/shader-slang/{portfile.cmake,vcpkg.json} from vcpkg and re-apply the
# "generate debug targets export" block below.

vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

set(key NOTFOUND)
if(VCPKG_TARGET_IS_WINDOWS)
	set(key "windows-${VCPKG_TARGET_ARCHITECTURE}")
elseif(VCPKG_TARGET_IS_OSX)
	set(key "macosx-${VCPKG_TARGET_ARCHITECTURE}")
elseif(VCPKG_TARGET_IS_LINUX)
	set(key "linux-${VCPKG_TARGET_ARCHITECTURE}")
endif()

set(ARCHIVE NOTFOUND)
set(DEBUG_INFO_ARCHIVE NOTFOUND)
# For convenient updates, use 
# vcpkg install shader-slang --cmake-args=-DVCPKG_SHADER_SLANG_UPDATE=1
if(key STREQUAL "windows-x64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-windows-x86_64.zip"
		FILENAME "slang-${VERSION}-windows-x86_64.zip"
		SHA512 6ca46a6e920596b2818d870663ea1c08c1ba0c40e600a6e6faf38784e3750f1a97d550f1c2b36fa0e2458166b9d128783cf0c413ecb6cd9cf98a675f3f92e33b
	)
	vcpkg_download_distfile(
		DEBUG_INFO_ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-windows-x86_64-debug-info.zip"
		FILENAME "slang-${VERSION}-windows-x86_64-debug-info.zip"
		SHA512 4e095c38a8ff054741f2e2857c4b989e0854100cb754af1b29d11203b23818863b4a34e85b2962df158e94b3de84d9534c982a995f8891b9888c532de469d4a5
	)
endif()
if(key STREQUAL "windows-arm64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-windows-aarch64.zip"
		FILENAME "slang-${VERSION}-windows-aarch64.zip"
		SHA512 37e9528c0d3ae2ae36ef94c5570523bec80f428a6953f3fa41ef13dabbecefd450017acb0e5d80e7ca98bdd973c79fb5ce87870a57c616e7b186b8c32e0c43ab
	)
	vcpkg_download_distfile(
		DEBUG_INFO_ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-windows-aarch64-debug-info.zip"
		FILENAME "slang-${VERSION}-windows-aarch64-debug-info.zip"
		SHA512 f3b9e09a9792b9a25ace3377df4c401ac5db46ef1fac0830bc511758457c803bc8ca72282383090ac80c97da1726f96ae48cedeee3909d168037260f730ce3ce
	)
endif()
if(key STREQUAL "macosx-x64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-macos-x86_64.zip"
		FILENAME "slang-${VERSION}-macos-x86_64.zip"
		SHA512 da86dd57f9ec98060f26b1ad87d3b9b7cfd127bd44534a629f6eb3a429065e09b1c41f9b4b0b5fee0e9748e7fa1ed180539c208b9b9a3d4f2737447f59accc9a
	)
	vcpkg_download_distfile(
		DEBUG_INFO_ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-macos-x86_64-debug-info.zip"
		FILENAME "slang-${VERSION}-macos-x86_64-debug-info.zip"
		SHA512 8d5c688405b9023d4985432eb44922d85fecc2b539c9bff198888219d89e6a606ada09659aab5d1f824b6efbbadd529d3d5de101e87b92114d32a478a45af3fb
	)
endif()
if(key STREQUAL "macosx-arm64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-macos-aarch64.zip"
		FILENAME "slang-${VERSION}-macos-aarch64.zip"
		SHA512 91c2c38fdb9dc387d2014bfb015bc3f2c09aaa7b9be734d852a4f41b82b42c98fcd8ee5241057a6000f708baa9fa91af691c4c963bd0fda7faf68032e877945a
	)
	vcpkg_download_distfile(
		DEBUG_INFO_ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-macos-aarch64-debug-info.zip"
		FILENAME "slang-${VERSION}-macos-aarch64-debug-info.zip"
		SHA512 0c9125cd80e7f89403f72af4f0077d9c9d436421cc4256b50763180680ebfdfda127b7f8aeeb6659dd2a26a17cadce720a4a97f60ca756b5a98d4ee6ca5cd2b6
	)
endif()
if(key STREQUAL "linux-x64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-linux-x86_64.zip"
		FILENAME "slang-${VERSION}-linux-x86_64.zip"
		SHA512 d50ff36b1d94b07be38be0d16e8c6336b8dd254cb078e8582d715496e4d7c4bb5e3e53792bce9af1666f3ced0725a46ba89d123e4b6c5990237519a2b990d26a
	)
	vcpkg_download_distfile(
		DEBUG_INFO_ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-linux-x86_64-debug-info.zip"
		FILENAME "slang-${VERSION}-linux-x86_64-debug-info.zip"
		SHA512 339f0069db46d4c6912bacaf9966684fe3d6c38ece969c2565227b3d8a170ba7152005d029e0caf7b20e8f614ffe7d58108e41811db6840df4708e01edf936ec
	)
endif()
if(key STREQUAL "linux-arm64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-linux-aarch64.zip"
		FILENAME "slang-${VERSION}-linux-aarch64.zip"
		SHA512 d7cb2767342509248e50ad95fa4e01537abd6d63a10c4bc6005f049b0aa2fd315efd57d585c4e0343fa6fb893758d0e6715d2c2e41cc144d676c359ca758920b
	)
	vcpkg_download_distfile(
		DEBUG_INFO_ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-linux-aarch64-debug-info.zip"
		FILENAME "slang-${VERSION}-linux-aarch64-debug-info.zip"
		SHA512 f9764a98b200c045bf2b8a3a32ad2cff677c4a33ce17729d4ea444e49663e8c1b988b95bd6427d3f726e32c1d910a3d4298b7f2b94d74f3460ce49817ce76422
	)
endif()
if(NOT ARCHIVE)
	message(FATAL_ERROR "Unsupported platform. Please implement me!")
endif()

vcpkg_extract_source_archive(
	BINDIST_PATH
	ARCHIVE "${ARCHIVE}"
	NO_REMOVE_ONE_LEVEL
)

if(DEBUG_INFO_ARCHIVE)
	vcpkg_extract_source_archive(
		DEBUG_INFO_PATH
		ARCHIVE "${DEBUG_INFO_ARCHIVE}"
		NO_REMOVE_ONE_LEVEL
	)
endif()

if(VCPKG_SHADER_SLANG_UPDATE)
	message(STATUS "All downloads are up-to-date.")
	message(FATAL_ERROR "Stopping due to VCPKG_SHADER_SLANG_UPDATE being enabled.")
endif()

file(GLOB libs
	"${BINDIST_PATH}/lib/*.lib"
	"${BINDIST_PATH}/lib/*.dylib"
	"${BINDIST_PATH}/lib/*.so"
	"${BINDIST_PATH}/lib/*.so.0.${VERSION}" # On linux, some of the .so files are postfixed by the version.
)
file(INSTALL ${libs} DESTINATION "${CURRENT_PACKAGES_DIR}/lib")

file(GLOB dyn_libs
	"${BINDIST_PATH}/lib/*.dylib"
	"${BINDIST_PATH}/lib/*.so"
	"${BINDIST_PATH}/lib/*.so.0.${VERSION}" # On linux, some of the .so files are postfixed by the version.
)

if(VCPKG_TARGET_IS_WINDOWS)
  file(GLOB dlls "${BINDIST_PATH}/bin/*.dll")
  list(APPEND dyn_libs ${dlls})
  file(INSTALL ${dlls} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")

  # In windows, the debug symbols are on the root directory of the debug archive
  if(DEBUG_INFO_PATH)
    file(GLOB pdb_files "${DEBUG_INFO_PATH}/*.pdb")
    if(pdb_files)
      file(INSTALL ${pdb_files} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
    endif()
  endif()
endif()

# In other platfroms, the debug symbols are structured under lib.
# There are also debug symbols for the tools under bin but we ignore these
if(NOT VCPKG_TARGET_IS_WINDOWS AND DEBUG_INFO_PATH)
  file(GLOB debug_sym_libs "${DEBUG_INFO_PATH}/lib/*")
  if(debug_sym_libs)
    file(INSTALL ${debug_sym_libs} DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
  endif()
endif()

if(NOT VCPKG_BUILD_TYPE)
  file(INSTALL "${CURRENT_PACKAGES_DIR}/lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug")
  if(VCPKG_TARGET_IS_WINDOWS)
    file(INSTALL "${CURRENT_PACKAGES_DIR}/bin" DESTINATION "${CURRENT_PACKAGES_DIR}/debug")
  endif()
endif()

# On macos, slang has signed their binaries
# vcpkg wants to be helpful and update the rpath as it moves binaries around but this 
# breaks the code signature and makes the binaries useless
# Removing the signature is rude so instead we will disable rpath fixup
if(VCPKG_TARGET_IS_OSX OR VCPKG_TARGET_IS_IOS)
  set(VCPKG_FIXUP_MACHO_RPATH OFF)
endif()

# Must manually copy some tool dependencies since vcpkg can't copy them automagically for us
file(INSTALL ${dyn_libs} DESTINATION "${CURRENT_PACKAGES_DIR}/tools/shader-slang")
vcpkg_copy_tools(TOOL_NAMES slangc slangd slangi slang SEARCH_DIR "${BINDIST_PATH}/bin")

file(GLOB headers "${BINDIST_PATH}/include/*.h")
file(INSTALL ${headers} DESTINATION "${CURRENT_PACKAGES_DIR}/include")

block(SCOPE_FOR VARIABLES)
	set(VCPKG_BUILD_TYPE Release) # no debug binaries anyways

	if (VCPKG_TARGET_IS_WINDOWS)
		file(COPY "${BINDIST_PATH}/cmake" DESTINATION "${CURRENT_PACKAGES_DIR}")
		vcpkg_cmake_config_fixup(CONFIG_PATH cmake PACKAGE_NAME slang)
	else()
		file(COPY "${BINDIST_PATH}/lib/cmake/slang" DESTINATION "${CURRENT_PACKAGES_DIR}")
		vcpkg_cmake_config_fixup(CONFIG_PATH slang PACKAGE_NAME slang)
	endif()

	vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/slang/slangConfig.cmake"
		[[HINTS "${PACKAGE_PREFIX_DIR}/bin" ENV PATH]]
		[[PATHS "${PACKAGE_PREFIX_DIR}/tools/shader-slang" NO_DEFAULT_PATH REQUIRED]]
	)
	vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/slang/slangConfigVersion.cmake"
		[[if("${CMAKE_SIZEOF_VOID_P}" STREQUAL ""]]
		[[if(#[=[ host tool ]=] "TRUE"]] 
	)
endblock()

# ---- Generate a Debug export for the installed (release) libraries ----
#
# shader-slang builds no debug binaries (VCPKG_BUILD_TYPE Release above), but
# the release libraries were mirrored into debug/lib earlier in this file so
# that Debug consumers have something to link. Export those copies as the
# Debug configuration of the same imported targets, so that multi-config
# builds (e.g. Ninja Multi-Config) link all shared libraries from the same
# directory per configuration (debug/lib in Debug, lib in Release). Mixing
# directories otherwise makes CMake's runtime-search-path ordering cyclic
# ("Cannot generate a safe runtime search path ... cycle in the constraint
# graph") because each library then exists in both directories while being
# linked from only one of them.
#
# The upstream-generated slangTargets.cmake globs every slangTargets-*.cmake
# file in the same directory, so dropping a -debug file next to the -release
# one registers the Debug configuration automatically. We derive it from the
# release file: rename the RELEASE properties to DEBUG and point them at the
# debug/lib copies (tool locations under tools/ stay unchanged — only the
# release binaries exist there, and they are configuration-independent).
if(EXISTS "${CURRENT_PACKAGES_DIR}/share/slang/slangTargets-release.cmake")
	file(READ "${CURRENT_PACKAGES_DIR}/share/slang/slangTargets-release.cmake" _slang_debug_targets)
	string(REPLACE "_RELEASE" "_DEBUG" _slang_debug_targets "${_slang_debug_targets}")
	string(REPLACE "IMPORTED_CONFIGURATIONS RELEASE" "IMPORTED_CONFIGURATIONS DEBUG" _slang_debug_targets "${_slang_debug_targets}")
	string(REPLACE [[for configuration "Release"]] [[for configuration "Debug"]] _slang_debug_targets "${_slang_debug_targets}")
	string(REPLACE "\${_IMPORT_PREFIX}/lib/" "\${_IMPORT_PREFIX}/debug/lib/" _slang_debug_targets "${_slang_debug_targets}")
	file(WRITE "${CURRENT_PACKAGES_DIR}/share/slang/slangTargets-debug.cmake" "${_slang_debug_targets}")
endif()

vcpkg_install_copyright(
	FILE_LIST "${BINDIST_PATH}/LICENSE"
	COMMENT #[[ from README ]] [[
The Slang code itself is under the Apache 2.0 with LLVM Exception license.

Builds of the core Slang tools depend on the following projects, either automatically or optionally, which may have their own licenses:

* [`glslang`](https://github.com/KhronosGroup/glslang) (BSD)
* [`lz4`](https://github.com/lz4/lz4) (BSD)
* [`miniz`](https://github.com/richgel999/miniz) (MIT)
* [`spirv-headers`](https://github.com/KhronosGroup/SPIRV-Headers) (Modified MIT)
* [`spirv-tools`](https://github.com/KhronosGroup/SPIRV-Tools) (Apache 2.0)
* [`ankerl::unordered_dense::{map, set}`](https://github.com/martinus/unordered_dense) (MIT)

Slang releases may include [slang-llvm](https://github.com/shader-slang/slang-llvm) which includes [LLVM](https://github.com/llvm/llvm-project) under the license:

* [`llvm`](https://llvm.org/docs/DeveloperPolicy.html#new-llvm-project-license-framework) (Apache 2.0 License with LLVM exceptions)
]])
