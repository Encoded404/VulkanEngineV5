#include <string>
#include <vector>
#include <memory>

#include <logging/logging.hpp>
#include <logging/ConsoleLogger.hpp>

namespace {
    void InitializeLogger() {
        static std::shared_ptr<Logiface::ConsoleLogger> app_logger = std::make_shared<Logiface::ConsoleLogger>(); // NOLINT(misc-const-correctness)
        Logiface::SetLogger(app_logger);
    }
} // anonymous namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    InitializeLogger();

    LOGIFACE_LOG(info, "Modern CMake Template CLI started");

    if (!args.empty()) {
        // convert args to a vector of strings and log them
        std::string args_list = "Command-line arguments:";
        for (const auto& arg : args) {
            args_list += "\n->\t" + arg;
        }
        LOGIFACE_LOG(info, args_list);
    }

    LOGIFACE_LOG(info, "App completed");

    return 0;
}