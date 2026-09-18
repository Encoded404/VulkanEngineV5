#include "engine/core/bootstrap/EntryPoint.hpp"

#include "engine/core/bootstrap/Crash.hpp"

#include <cstdio>
#include <exception>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace {

// Installed in the same object as the entry point, so the linker keeps it for
// as long as it keeps `main`. This gives the crash handlers coverage during
// dynamic initialization of later translation units (best effort: initializers
// that run before this one are, by construction, not covered).
struct EarlyCrashBoot {
    EarlyCrashBoot() { VulkanEngine::Crash::Boot(nullptr); }
};
EarlyCrashBoot g_early_crash_boot;

int RunApplicationEntry(int argc, char* const argv[]) {
    int exit_code = 70; // EX_SOFTWARE
    try {
        exit_code = VulkanEngine::AppMain(argc, argv);
    } catch (const std::exception& ex) {
        VulkanEngine::Crash::Report("unhandled exception", ex.what());
        exit_code = 1;
    } catch (...) {
        VulkanEngine::Crash::Report("unhandled exception", "non-std exception");
        exit_code = 1;
    }
    VulkanEngine::Crash::Shutdown();
    return exit_code;
}

#if defined(_WIN32)
// A GUI-subsystem executable has no console of its own. When it is launched
// from an existing console (a terminal), attach to it so logging stays
// visible; when it is launched by a double-click there is no parent console
// and the window stays hidden, which is the point.
void AttachParentConsoleIfPresent() {
    if (::AttachConsole(ATTACH_PARENT_PROCESS) == 0) {
        return;
    }
    std::FILE* reopened = std::freopen("CONOUT$", "w", stdout);
    static_cast<void>(reopened);
    reopened = std::freopen("CONOUT$", "w", stderr);
    static_cast<void>(reopened);
}
#endif

} // namespace

[[gnu::weak]] int VulkanEngine::AppMain(int, char* const[]) {
    VulkanEngine::Crash::Report("no VulkanEngine::AppMain defined",
                                "the application did not define VulkanEngine::AppMain");
    return 70;
}

int main(int argc, char** argv) {
    VulkanEngine::Crash::Boot(nullptr);
    return RunApplicationEntry(argc, argv);
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    VulkanEngine::Crash::Boot(nullptr);
    AttachParentConsoleIfPresent();
    return RunApplicationEntry(__argc, __argv);
}
#endif
