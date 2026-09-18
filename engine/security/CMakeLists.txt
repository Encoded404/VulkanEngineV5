set(SECURITY_TARGET "VulkanSecurity")

# Monocypher is a single C translation unit. Compile it as C++ (the headers
# carry extern "C" guards, exactly like MikkTSpace) so the project's CXX-only
# language setup is preserved and no new toolchain requirement is introduced.
set_source_files_properties(
    "${CMAKE_SOURCE_DIR}/external/Monocypher/src/monocypher.c"
    PROPERTIES LANGUAGE CXX
)

add_library(${SECURITY_TARGET} STATIC
    DataCipher.cpp
    PasswordHash.cpp
    KeyExchange.cpp
    "${CMAKE_SOURCE_DIR}/external/Monocypher/src/monocypher.c"
)
target_compile_features(${SECURITY_TARGET} PUBLIC cxx_std_23)

# Private: Monocypher headers. Public: the project root so
# engine/security/cipher_format.hpp is reachable from the module implementations.
target_include_directories(${SECURITY_TARGET} PRIVATE
    "${CMAKE_SOURCE_DIR}/external/Monocypher/src"
)
target_include_directories(${SECURITY_TARGET} PUBLIC
    ${CMAKE_SOURCE_DIR}
)

target_compile_options(${SECURITY_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${SECURITY_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${SECURITY_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            DataCipher.cppm
            PasswordHash.cppm
            KeyExchange.cppm
    )
endif()

if(TARGET project_sanitizers)
    target_link_libraries(${SECURITY_TARGET} PRIVATE project_sanitizers)
endif()
if(TARGET project_sections)
    target_link_libraries(${SECURITY_TARGET} PRIVATE project_sections)
endif()
if(TARGET project_size)
    target_link_libraries(${SECURITY_TARGET} PRIVATE project_size)
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${SECURITY_TARGET})
endif()
