// Weak-symbol replacement that is portable to MSVC.
//
// The fallback AppMain lives in its own translation unit, inside the
// EngineEntryPoint static library. A linker only extracts an archive member
// when one of its symbols is still unresolved, so an application that defines
// VulkanEngine::AppMain keeps this object out of the link; an application that
// forgets the hook gets this clear diagnostic instead of an undefined-symbol
// error. This is the portable equivalent of the old [[gnu::weak]] attribute,
// which MSVC does not support.
#include "engine/core/bootstrap/EntryPoint.hpp"

#include "engine/core/bootstrap/Crash.hpp"

int VulkanEngine::AppMain(int, char* const[]) {
    VulkanEngine::Crash::Report("no VulkanEngine::AppMain defined",
                                "the application did not define VulkanEngine::AppMain");
    return 70;
}
