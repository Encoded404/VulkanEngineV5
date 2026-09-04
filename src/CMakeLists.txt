# layered targets
add_subdirectory(shared)
add_subdirectory(backend)
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