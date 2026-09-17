#include <CLI/CLI.hpp>

#include "engine/core/EntryPoint.hpp"

import std;

import Runtime.Application;
import Runtime.ExampleCli;
import Examples.InfiniteRunner.Game;

int VulkanEngine::AppMain(int argc, char* const argv[]) {
    Runtime::Cli cli{"VulkanEngineV5 Infinite Runner", VKENGINE_ORG_ID, VKENGINE_APP_ID};

    std::string leaderboard_host;
    cli.App().add_option("--leaderboard-host", leaderboard_host,
                         "Override the sealed leaderboard host (IP or hostname)");

    int leaderboard_port = 0;
    cli.App().add_option("--leaderboard-port", leaderboard_port,
                         "Override the sealed leaderboard port (0 keeps the sealed value)")
       ->check(CLI::Range(0, 65535));

    if (!cli.Parse(argc, argv)) {
        return cli.ExitCode();
    }

    Examples::InfiniteRunner::Game::EndpointOverride endpoint{};
    endpoint.host = std::move(leaderboard_host);
    endpoint.port = static_cast<std::uint16_t>(leaderboard_port);

    auto game = std::make_unique<Examples::InfiniteRunner::Game::Game>(cli.ExecutablePath(),
                                                                       std::move(endpoint));

    return VulkanEngine::Application::RunApplication(cli.MakeConfig(), game->GetHooks());
}
