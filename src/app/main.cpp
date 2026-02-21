#include <iostream>
#include <string>
#include <vector>

#include <lib1/lib_example.hpp>

#include <logging/logging.hpp>
#include <logging/ConsoleLogger.hpp>

// forward declarations
void initializeLogger();

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    initializeLogger();

    LOGIFACE_LOG(info, "Modern CMake Template CLI started");

    // Use the public library API (class with two example functions)
    modern_cmake_template::example api;
    const int v1 = 21;
    const int v2 = 5;
    LOGIFACE_LOG(debug, "Calling exampleFunction");
    int result1 = api.exampleFunction(v1);
    LOGIFACE_LOG(debug, "exampleFunction(" + std::to_string(v1) + ") = " + std::to_string(result1));

    LOGIFACE_LOG(debug, "Calling anotherExampleFunction");
    int result2 = api.anotherExampleFunction(v2);
    LOGIFACE_LOG(debug, "anotherExampleFunction(" + std::to_string(v2) + ") = " + std::to_string(result2));

    if (!args.empty()) {
        std::string args_list;
        args_list = "Args:";
        for (auto a : args) args_list += " \"" + a + '"';
        LOGIFACE_LOG(debug, args_list);
    }

    LOGIFACE_LOG(info, "App completed");

    return 0;
}

void initializeLogger() {
    static std::shared_ptr<Logiface::ConsoleLogger> app_logger = std::make_shared<Logiface::ConsoleLogger>();
    Logiface::SetLogger(app_logger);
}
