cmake_minimum_required(VERSION 3.15)

set(PROJECT_NAME "VulkanEngineV5")
set(LIBRARY_TARGET "VulkanEngine")
set(APP_TARGET "VulkanApp")

# if this is not set by the user cmake will decide
set(OUTPUT_PATH "${CMAKE_BINARY_DIR}/bin")

# Project name and version
project(${PROJECT_NAME} VERSION 1.0 LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Require compiler support for modules (will be applied per-target in src/)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "C++ modules required but the compiler (${CMAKE_CXX_COMPILER_ID}) is not supported. Use Clang or GCC.")
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

if(EXISTS "${LOGGING_INTERFACE_DIR}/CMakeLists.txt")
	add_subdirectory("${LOGGING_INTERFACE_DIR}" cppLoggingInterface)
else()
	message(FATAL_ERROR "Missing logging interface at ${LOGGING_INTERFACE_DIR}; provide the repo there or set LOGIFACE_PATH to another directory.")
endif()

# Make the logging interface path available to subprojects that declare
# `LOGIFACE_PATH` in the cache (e.g. external/cppFileLoader). This ensures
# they look up the same logging interface instead of expecting a nested copy.
set(LOGIFACE_PATH "${LOGGING_INTERFACE_DIR}" CACHE PATH "Optional path to the cppLoggingInterface repo" FORCE)

# -----------------
# VulkanRAIIWrapper
# -----------------
message("finding Vulkan RAII Wrapper")

option(ENABLE_LOGGING "Enable header-only logging interface" ON)

set(VULKANRAIIWRAPPER_PATH "${CMAKE_SOURCE_DIR}/external/VulkanRAIIWrapper" CACHE PATH "Optional path to the VulkanRAIIWrapper repo")

if(VULKANRAIIWRAPPER_PATH)
	add_subdirectory("${VULKANRAIIWRAPPER_PATH}" VulkanRAIIWrapper)
else()
	message(FATAL_ERROR "Missing VulkanRAIIWrapper at ${VULKANRAIIWRAPPER_PATH}; provide the repo there or set VULKANRAIIWRAPPER_PATH to another directory.")
endif()

# ----------
# FileLoader
# ----------
message("finding FileLoader")

set(FILELOADER_PATH "${CMAKE_SOURCE_DIR}/external/cppFileLoader" CACHE PATH "Optional path to the FileLoader repo")

if(FILELOADER_PATH)
	add_subdirectory("${FILELOADER_PATH}" FileLoader)
else()
	message(FATAL_ERROR "Missing FileLoader at ${FILELOADER_PATH}; provide the repo there or set FILELOADER_PATH to another directory.")
endif()

option(CLANG_TIDY_ENABLED "Enable Clang-Tidy static analysis" ON)
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