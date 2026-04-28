function(configure_app_asset_copy target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "configure_app_asset_copy: target '${target}' does not exist")
    endif()

    set(oneValueArgs SOURCE_DIR DEST_SUBDIR)
    cmake_parse_arguments(CAAC "" "${oneValueArgs}" "" ${ARGN})

    if(NOT CAAC_SOURCE_DIR)
        message(FATAL_ERROR "configure_app_asset_copy: SOURCE_DIR is required")
    endif()

    if(NOT EXISTS "${CAAC_SOURCE_DIR}")
        message(STATUS "configure_app_asset_copy: skipping missing folder ${CAAC_SOURCE_DIR}")
        return()
    endif()

    if(NOT CAAC_DEST_SUBDIR)
        get_filename_component(CAAC_DEST_SUBDIR "${CAAC_SOURCE_DIR}" NAME)
    endif()

    file(GLOB_RECURSE APP_ASSET_FILES CONFIGURE_DEPENDS
        "${CAAC_SOURCE_DIR}/*"
    )

    if(APP_ASSET_FILES)
        # add_custom_command(TARGET ...) does not support DEPENDS.
        # We'll use a stamp file to track changes if we want incremental copies,
        # but for simplicity and fixing the warning, we'll remove DEPENDS from the TARGET command.
        # POST_BUILD commands run whenever the target is built (linked).
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CAAC_SOURCE_DIR}"
                "$<TARGET_FILE_DIR:${target}>/${CAAC_DEST_SUBDIR}"
            COMMENT "Copying app assets: ${CAAC_SOURCE_DIR} -> $<TARGET_FILE_DIR:${target}>/${CAAC_DEST_SUBDIR}"
            VERBATIM
        )
    endif()
endfunction()
