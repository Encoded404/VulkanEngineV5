# Copies the MinGW/Windows runtime DLLs a built executable depends on into its
# output directory.
#
# Invoked as a script (cmake -P) once per source directory from the POST_BUILD
# step in add_engine_example:
#
#   cmake -DDLL_DIR=<dir> -DDEST=<exe dir> -P CopyRuntimeDlls.cmake
#
# DLL_DIR is typically one of:
#   * <build>/vcpkg_installed/<triplet>/bin            (release dependencies)
#   * <build>/vcpkg_installed/<triplet>/debug/bin      (debug dependencies)
#   * <llvm-mingw>/<triple>/bin                        (libc++, libunwind, ...)
#
# Files are only copied when missing or newer than the destination, so
# incremental builds do not re-copy unchanged DLLs.

if(NOT DEFINED DEST OR DEST STREQUAL "")
    message(FATAL_ERROR "CopyRuntimeDlls.cmake: DEST is required")
endif()
if(NOT DEFINED DLL_DIR OR DLL_DIR STREQUAL "")
    message(FATAL_ERROR "CopyRuntimeDlls.cmake: DLL_DIR is required")
endif()

file(MAKE_DIRECTORY "${DEST}")

set(_copied 0)
if(EXISTS "${DLL_DIR}")
    file(GLOB _dlls "${DLL_DIR}/*.dll")
    foreach(_dll IN LISTS _dlls)
        get_filename_component(_name "${_dll}" NAME)
        set(_dest_file "${DEST}/${_name}")
        # IS_NEWER_THAN is true when the destination does not exist.
        if(NOT "${_dll}" IS_NEWER_THAN "${_dest_file}")
            continue()
        endif()
        file(COPY_FILE "${_dll}" "${_dest_file}")
        math(EXPR _copied "${_copied} + 1")
    endforeach()
endif()

if(_copied GREATER 0)
    message(STATUS "CopyRuntimeDlls: deployed ${_copied} DLL(s) from ${DLL_DIR} to ${DEST}")
endif()
