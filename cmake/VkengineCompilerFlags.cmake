include_guard(GLOBAL)

# Applies the project's per-configuration optimization/debug flags in a
# compiler-correct spelling. MSVC does not understand -g/-O0/-O2/-O3, and the
# multi-config generator does not apply CMAKE_CXX_FLAGS_<CONFIG> for us here.
function(vkengine_target_config_flags TARGET)
    if(NOT CMAKE_CONFIGURATION_TYPES)
        return()
    endif()
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE
            "$<$<CONFIG:Debug>:/Od>"
            "$<$<CONFIG:RelWithDebInfo>:/O2>"
            "$<$<CONFIG:Release>:/O2>"
        )
    else()
        target_compile_options(${TARGET} PRIVATE
            "$<$<CONFIG:Debug>:-g>"
            "$<$<CONFIG:Debug>:-O0>"
            "$<$<CONFIG:RelWithDebInfo>:-g>"
            "$<$<CONFIG:RelWithDebInfo>:-O2>"
            "$<$<CONFIG:Release>:-O3>"
        )
    endif()
endfunction()
