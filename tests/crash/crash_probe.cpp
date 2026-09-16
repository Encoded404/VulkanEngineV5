// Crash-handler probe.
//
// A tiny standalone executable used by crash_handler_tests. It links
// Crash.cpp directly (not the engine) so it can be built without sanitizers:
// the tests must exercise the real signal / structured-exception handlers,
// which AddressSanitizer would otherwise own in Debug builds.
//
// Usage: crash_probe <type> <report-dir>
//
// It always emits a known stage, breadcrumb and log marker before crashing, so
// the tests can assert that all of that context reached the crash report.

#include <csignal>

#include "engine/core/Crash.hpp"

import std;
import std.compat;

namespace {

constexpr const char* kMarker = "[probe] MARKER-LINE-12345";

void EmitContext() {
    VulkanEngine::Crash::Stage("probe stage");
    VulkanEngine::Crash::LogLine(kMarker);
    VulkanEngine::Crash::Breadcrumb("crumb", 7);
}

[[noreturn]] void TriggerCrash(const std::string& type) {
    if (type == "segv") {
        volatile int* null_pointer = nullptr;
        *null_pointer = 1;
    } else if (type == "fpe") {
        volatile int zero = 0;
        volatile int one = 1;
        volatile int quotient = one / zero;
        static_cast<void>(quotient);
    } else if (type == "ill") {
        __builtin_trap();
    }
#ifdef SIGBUS
    else if (type == "bus") {
        std::raise(SIGBUS);
    }
#endif
    else if (type == "abort") {
        std::abort();
    } else if (type == "terminate") {
        std::terminate();
    } else if (type == "throw") {
        throw std::runtime_error("probe thrown exception");
    }
    std::_Exit(90); // Unknown type: fail loudly rather than run on.
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: crash_probe <type> <report-dir>");
        return 2;
    }
    const std::string type = argv[1];

    VulkanEngine::Crash::Boot("crash_probe");
    VulkanEngine::Crash::SetReportDirectory(argv[2]);
    EmitContext();

    if (type == "none") {
        return 0;
    }
    if (type == "report") {
        VulkanEngine::Crash::Report("probe report", "probe detail");
        return 0;
    }
    if (type == "fatal") {
        VulkanEngine::Crash::Fatal("probe fatal", "probe detail");
    }

    TriggerCrash(type);
}
