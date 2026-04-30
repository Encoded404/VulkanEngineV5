module;

#include <memory>
#include <string_view>

#include <logging/logging.hpp>
#include <logging/ConsoleLogger.hpp>

export module VulkanEngine.Startup;

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
    static std::shared_ptr<Logiface::ConsoleLogger> app_logger = std::make_shared<Logiface::ConsoleLogger>(); // NOLINT(misc-const-correctness)
    app_logger->SetLevel(level);
    Logiface::SetLogger(app_logger);
}

void InitializeLogger(std::string_view level) {
    InitializeLogger(ParseLogLevel(level));
}

} // namespace VulkanEngine::Startup

