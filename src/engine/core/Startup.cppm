module;

// logging_macros.hpp has no <memory> include, safe in GMF.
#include <logging/logging_macros.hpp>

export module VulkanEngine.Startup;

import std;
import logiface;
import logiface.ConsoleLogger;

export namespace VulkanEngine::Startup {

[[nodiscard]] Logiface::Level ParseLogLevel(std::string_view level) {
    if (level == "trace") return Logiface::Level::trace;
    if (level == "debug") return Logiface::Level::debug;
    if (level == "info") return Logiface::Level::info;
    if (level == "warn") return Logiface::Level::warn;
    if (level == "error") return Logiface::Level::error;
    if (level == "critical") return Logiface::Level::critical;
    return Logiface::Level::info;
}

void InitializeLogger(Logiface::Level level) {
    // Logger is a singleton with static lifetime — raw pointer is fine.
    static Logiface::ConsoleLogger app_logger; // NOLINT(misc-const-correctness)
    app_logger.SetLevel(level);
    Logiface::SetLogger(&app_logger);
}

void InitializeLogger(std::string_view level) {
    InitializeLogger(ParseLogLevel(level));
}

} // namespace VulkanEngine::Startup

