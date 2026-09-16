# layered targets
add_subdirectory(shared)
add_subdirectory(backend)
add_subdirectory(security)
add_subdirectory(engine)
add_subdirectory(runtime)

if(ENABLE_SANITIZERS)
    target_link_libraries(${RUNTIME_TARGET} PRIVATE project_sanitizers)
    target_link_libraries(${ENGINE_TARGET} PRIVATE project_sanitizers)
    target_link_libraries(${BACKEND_TARGET} PRIVATE project_sanitizers)
    target_link_libraries(VulkanShared PRIVATE project_sanitizers)
endif()

target_link_libraries(${RUNTIME_TARGET} PRIVATE project_sections)
target_link_libraries(${ENGINE_TARGET} PRIVATE project_sections)
target_link_libraries(${BACKEND_TARGET} PRIVATE project_sections)
target_link_libraries(VulkanShared PRIVATE project_sections)

# Debug-info compression / Release identical-code folding. Linked PRIVATE to
# static libraries on purpose: the interface link options propagate through
# $<LINK_ONLY:...> to whatever executable finally links these, which is how
# --gc-sections already reaches every example and test binary.
target_link_libraries(${RUNTIME_TARGET} PRIVATE project_size)
target_link_libraries(${ENGINE_TARGET} PRIVATE project_size)
target_link_libraries(${BACKEND_TARGET} PRIVATE project_size)
target_link_libraries(VulkanShared PRIVATE project_size)
