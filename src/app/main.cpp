#include <string>
#include <vector>
#include <memory>

// #include <VulkanEngine/core.hpp> // unused in this file

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

    // Use the public library API (class with two example functions)
    //VulkanEngine::Example api;
    LOGIFACE_LOG(debug, "Calling exampleFunction");
    //int result1 = api.exampleFunction(21);
    //LOGIFACE_LOG(debug, "exampleFunction(21) = " + std::to_string(result1));

    LOGIFACE_LOG(debug, "Calling anotherExampleFunction");
    //int result2 = api.anotherExampleFunction(5);
    //LOGIFACE_LOG(debug, "anotherExampleFunction(5) = " + std::to_string(result2));

    if (!args.empty()) {
        std::string args_list = "Args:";
        for (const auto& a : args) args_list += " \"" + a + '"';
        LOGIFACE_LOG(debug, args_list);
    }

    LOGIFACE_LOG(info, "App completed");

    return 0;
}