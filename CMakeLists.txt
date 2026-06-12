cmake_minimum_required(VERSION 3.30)

if( NOT CMAKE_EXPERIMENTAL_CXX_IMPORT_STD )
    if( ${CMAKE_VERSION} VERSION_GREATER_EQUAL 4.3 )
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)
    elseif( ${CMAKE_VERSION} VERSION_GREATER_EQUAL 3.30 )
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
    endif ()
endif ()

set(PROJECT_NAME "VulkanEngineV5")
set(BACKEND_TARGET "VulkanBackend")
set(ENGINE_TARGET "VulkanEngine")
set(APP_TARGET "VulkanApp")

# if this is not set by the user cmake will decide
set(OUTPUT_PATH "${CMAKE_BINARY_DIR}/bin")

project(${PROJECT_NAME} VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Require C++23 for import std; and vulkan module support
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Require compiler support for modules
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "C++ modules required but the compiler (${CMAKE_CXX_COMPILER_ID}) is not supported. Use Clang or GCC.")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-fmodules-reduced-bmi)
endif()

# find vulkan
find_package( Vulkan 1.4.350 QUIET )

# manually create module target if it was not previously provided
if( NOT TARGET Vulkan::HppModule )
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

# Enable std module support for the logging interface.
set(LOGIFACE_USE_STD_MODULE ON CACHE BOOL "" FORCE)

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
set(CPPFILELOADER_USE_STD_MODULE ON CACHE BOOL "" FORCE)
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