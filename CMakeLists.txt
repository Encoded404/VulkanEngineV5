cmake_minimum_required(VERSION 3.30)

if( NOT CMAKE_EXPERIMENTAL_CXX_IMPORT_STD )
    if( ${CMAKE_VERSION} VERSION_GREATER_EQUAL 4.3 )
		message("enabling import std with version greater or equal 4.3")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
    elseif( ${CMAKE_VERSION} VERSION_GREATER_EQUAL 3.30 )
		message("enabling import std with version greater or equal 3.30")
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    endif ()
endif ()

set(PROJECT_NAME "VulkanEngineV5")
set(BACKEND_TARGET "VulkanBackend")
set(ENGINE_TARGET "VulkanEngine")
set(APP_TARGET "VulkanApp")

set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "Use standard C++" FORCE)

set(USE_LIBCPP OFF CACHE BOOL "use libc++ instead of libstdc++, note: requires libc++ to have the std module supported.")

# ---- IMPORTANT: stdlib flags must be set BEFORE project() ----
#
# CMake's built-in std module detection (the __cmake_cxx23 target) runs
# during project(), where it discovers the compiler frontend variant and
# the <source>/<bits>/std.cc file to use for `import std;`.
#
# On Fedora with Clang, the `/etc/clang/x86_64-redhat-linux-gnu-clang++.cfg`
# file passes `--gcc-triple=x86_64-redhat-linux`.  This makes the compiler
# (and CMake's module scanner) find libstdc++'s bits/std.cc even when
# -stdlib=libc++ is specified, because --gcc-install-dir still points at
# the GCC installation.
#
# The fix: push -stdlib=libc++ AND --gcc-install-dir= into the *initial*
# compiler flags so CMake's probe and module detection see a Clang that
# uses libc++ everywhere and does not discover the GCC installation tree.
#
if(USE_LIBCPP)
	# Make the initial compiler probe use libc++ headers/module.
	# NOTE: Do NOT add --gcc-install-dir= here — that flag silences the
	# GCC installation entirely, which breaks Clang's ability to find
	# libc++.modules.json for module discovery.  The module scanner
	# also fails to detect `import std;` in source files (e.g. fastgltf).
	# We handle the std module source selection separately via
	# CMAKE_CXX_MODULE_STD below.
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

# if this is not set by the user cmake will decide
set(OUTPUT_PATH "${CMAKE_BINARY_DIR}/bin")

project(${PROJECT_NAME} VERSION 1.0 LANGUAGES CXX)

# CMAKE_CXX_MODULE_STD was already set before project() to point at libc++'s
# module files.  We still set it to ON here: the ON flag tells CMake to
# propagate the __cmake_cxx23 module target reference to all consumer
# targets via their .modmap files (-fmodule-file=std=.../std.pcm).
# Without the ON flag, consumer targets won't know where to find the
# std module and will fail with "module 'std' not found".
#
# When USE_LIBCPP is OFF, we simply set ON and let CMake auto-detect
# libstdc++'s bits/std.cc — which is the correct default.
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Require C++23 for import std; and vulkan module support
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Verify libc++ availability (only valid after project() — we now know the compiler)
if(USE_LIBCPP)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		include(CheckCXXSourceCompiles)

		set(CMAKE_REQUIRED_FLAGS "-stdlib=libc++")

		check_cxx_source_compiles("
			#include <string>
			int main() {
				std::string s = \"hello\";
				return s.size();
			}
		" HAS_LIBCXX)

		unset(CMAKE_REQUIRED_FLAGS)

		if (HAS_LIBCXX)
			message(STATUS "libc++ detected and working — using it")

			# Force -stdlib=libc++ onto the auto-generated std-module targets so
			# the module BMI is compiled against the same libc++ headers that
			# the rest of the project uses.
			if(TARGET __cmake_cxx23)
				set_property(TARGET __cmake_cxx23 APPEND PROPERTY COMPILE_OPTIONS "-stdlib=libc++")
			endif()
			if(TARGET __cmake_cxx26)
				set_property(TARGET __cmake_cxx26 APPEND PROPERTY COMPILE_OPTIONS "-stdlib=libc++")
			endif()

			# Linker flag is not automatically propagated from CMAKE_CXX_FLAGS.
			add_link_options(-stdlib=libc++)
		else()
			message(STATUS "libc++ NOT available — falling back to libstdc++")
			# Undo the pre-project() changes so the default libstdc++ is used
			string(REPLACE "-stdlib=libc++" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
			unset(CMAKE_CXX_MODULE_STD)
		endif()
	else()
		message(WARNING "USE_LIBCPP=ON but compiler is not Clang — ignoring")
		string(REPLACE "-stdlib=libc++" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
		unset(CMAKE_CXX_MODULE_STD)
	endif()
endif()

# Require compiler support for modules
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "C++ modules required but the compiler (${CMAKE_CXX_COMPILER_ID}) is not supported. Use Clang or GCC.")
endif()

# find vulkan
find_package( Vulkan 1.4.350 QUIET )

# manually create module target if it was not previously provided
if( NOT TARGET Vulkan::HppModule )
	message("could not find vulkan target with find, adding manually")
	add_library( Vulkan-HppModule OBJECT )
	add_library( Vulkan::HppModule ALIAS Vulkan-HppModule )
	target_sources( Vulkan-HppModule PUBLIC
			FILE_SET CXX_MODULES
			BASE_DIRS ${Vulkan_INCLUDE_DIR}
			FILES
			${Vulkan_INCLUDE_DIR}/vulkan/vulkan.cppm
			# ${Vulkan_INCLUDE_DIR}/vulkan/vulkan_video.cppm # no vulkan_video.cppm used and it sometimes gives errors so begone
	)
	target_compile_features( Vulkan-HppModule PUBLIC cxx_std_23 )
	set_target_properties( Vulkan-HppModule PROPERTIES CXX_MODULE_STD ON )
	target_include_directories( Vulkan-HppModule PUBLIC ${Vulkan_INCLUDE_DIR} )
endif()

target_compile_definitions( Vulkan-HppModule PUBLIC VULKAN_HPP_DISPATCH_LOADER_DYNAMIC )

find_program(CCACHE_PROGRAM ccache)

if(CCACHE_PROGRAM)
	#set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM}) # we dont use the c compiler rn
	set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

# Enable CTest at the top level so `ctest` discovers tests
include(CTest)

# Include custom CMake modules path (for other helpers)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# -----------------
# Logging Interface
# -----------------
message("finding logging interface")

set(DEFAULT_LOGGING_INTERFACE_DIR "${CMAKE_SOURCE_DIR}/external/cppLoggingInterface")
set(LOGIFACE_PATH "" CACHE PATH "Optional path to the cppLoggingInterface repo")

if(LOGIFACE_PATH)
	set(LOGGING_INTERFACE_DIR "${LOGIFACE_PATH}")
else()
	set(LOGGING_INTERFACE_DIR "${DEFAULT_LOGGING_INTERFACE_DIR}")
endif()

# Build the logging module (provides import logiface;), but use
# #include headers instead of import std; (avoids Clang bug LLVM #138558).
set(LOGIFACE_BUILD_MODULE ON CACHE BOOL "" FORCE)
set(LOGIFACE_USE_STD_MODULE OFF CACHE BOOL "" FORCE)

if(EXISTS "${LOGGING_INTERFACE_DIR}/CMakeLists.txt")
	add_subdirectory("${LOGGING_INTERFACE_DIR}" cppLoggingInterface)
else()
	message(FATAL_ERROR "Missing logging interface at ${LOGGING_INTERFACE_DIR}; provide the repo there or set LOGIFACE_PATH to another directory.")
endif()

# Make the logging interface path available to subprojects that declare
# `LOGIFACE_PATH` in the cache (e.g. external/cppFileLoader). This ensures
# they look up the same logging interface instead of expecting a nested copy.
set(LOGIFACE_PATH "${LOGGING_INTERFACE_DIR}" CACHE PATH "Optional path to the cppLoggingInterface repo" FORCE)

# ----------
# FileLoader
# ----------
message("finding FileLoader")

set(FILELOADER_PATH "${CMAKE_SOURCE_DIR}/external/cppFileLoader" CACHE PATH "Optional path to the FileLoader repo")

# Enable module support for FileLoader (import std; + module files).
set(CPPFILELOADER_USE_STD_MODULE OFF CACHE BOOL "" FORCE)
set(CPPFILELOADER_BUILD_AS_MODULE ON CACHE BOOL "" FORCE)

if(FILELOADER_PATH)
	add_subdirectory("${FILELOADER_PATH}" FileLoader)
else()
	message(FATAL_ERROR "Missing FileLoader at ${FILELOADER_PATH}; provide the repo there or set FILELOADER_PATH to another directory.")
endif()

# ----------------------------
# Slang/SPIR-V Compiler Helper
# ----------------------------
message("finding SlangSpriVCompilerHelper")

add_subdirectory("${CMAKE_SOURCE_DIR}/external/SlangSpriVCompilerHelper" SlangSpriVCompilerHelper)

option(CLANG_TIDY_ENABLED "Enable Clang-Tidy static analysis" ON)
option(SYSTEM_VULKAN "Use system Vulkan loader instead of vcpkg's" ON)
# disable Clang-Tidy if not using Clang or GCC
if(CLANG_TIDY_ENABLED AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set(CLANG_TIDY_ENABLED OFF)
endif()
if(CLANG_TIDY_ENABLED)
	# Enable Clang-Tidy if available
	include(ConfigureClangTidy)
endif()

# Add subdirectories for source and tests
add_subdirectory(src)

if(BUILD_TESTING)
	add_subdirectory(tests)
endif()