# layered targets
add_subdirectory(backend)
add_subdirectory(engine)
add_subdirectory(app)

# Sanitizers interface for Debug builds
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer in Debug mode" ON)
if(ENABLE_SANITIZERS)
  add_library(project_sanitizers INTERFACE)
  target_compile_options(project_sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  )
  target_link_options(project_sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  )
  target_link_libraries(${APP_TARGET} PRIVATE project_sanitizers)
  target_link_libraries(${ENGINE_TARGET} PRIVATE project_sanitizers)
  target_link_libraries(${BACKEND_TARGET} PRIVATE project_sanitizers)
endif()

# Sections optimization for dead-code stripping (gc-sections)
add_library(project_sections INTERFACE)
target_compile_options(project_sections INTERFACE
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffunction-sections>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fdata-sections>
)
target_link_options(project_sections INTERFACE
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wl,--gc-sections>
)
target_link_libraries(${APP_TARGET} PRIVATE project_sections)
target_link_libraries(${ENGINE_TARGET} PRIVATE project_sections)
target_link_libraries(${BACKEND_TARGET} PRIVATE project_sections)