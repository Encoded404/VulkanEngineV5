#include <CLI/CLI.hpp>

import std;

import Runtime.Application;
import Runtime.ExampleCli;
import Examples.Minimal.Game;

int main(int argc, char* const argv[]) {
    Runtime::Cli cli{"VulkanEngineV5 Minimal"};

    if (!cli.Parse(argc, argv)) {
        return cli.ExitCode();
    }

    auto game = std::make_unique<Examples::Minimal::Game::Game>(cli.ExecutablePath());

    return VulkanEngine::Application::RunApplication(cli.MakeConfig(), game->GetHooks());
}
