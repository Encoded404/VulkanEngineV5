#ifndef VULKANENGINE_CORE_ENTRYPOINT_HPP
#define VULKANENGINE_CORE_ENTRYPOINT_HPP

// Engine-owned process entry point.
//
// The engine defines the real entry point (main, and WinMain on Windows) so
// that every application automatically gets crash handling, crash-safe log
// capture and (on Windows) a hidden console. An application only defines this
// hook; it must not define main itself.
//
// The engine provides a weak fallback so a missing hook reports a clear fatal
// error instead of failing to link. An application may still opt out and own
// main, in which case the engine entry point is simply not linked in.

namespace VulkanEngine {

// Implemented by the application. Called after crash handling is installed.
// Returns the process exit code.
int AppMain(int argc, char* const argv[]);

} // namespace VulkanEngine

#endif // VULKANENGINE_CORE_ENTRYPOINT_HPP
