#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

import std;

import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.Config;
import Examples.InfiniteRunner.Leaderboard.Log;
import Examples.InfiniteRunner.Leaderboard.Server;

int main(int argc, char** argv) {
    using namespace Examples::InfiniteRunner::Leaderboard;

    std::uint16_t port = 7777;
    std::string store_path;
    bool store_explicit = false;
    bool ephemeral = false;
    std::string account_path;
    std::string identity_path;
    std::string log_level = "info";

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string value;
        if (argument == "--port") {
            if (!next(value)) {
                std::cerr << "--port needs a value\n";
                return 2;
            }
            port = static_cast<std::uint16_t>(std::atoi(value.c_str()));
        } else if (argument == "--store") {
            if (!next(store_path)) {
                std::cerr << "--store needs a value\n";
                return 2;
            }
            store_explicit = true;
        } else if (argument == "--ephemeral") {
            ephemeral = true;
        } else if (argument == "--accounts") {
            if (!next(account_path)) {
                std::cerr << "--accounts needs a value\n";
                return 2;
            }
        } else if (argument == "--identity") {
            if (!next(identity_path)) {
                std::cerr << "--identity needs a value\n";
                return 2;
            }
        } else if (argument == "--log-level") {
            if (!next(log_level)) {
                std::cerr << "--log-level needs a value\n";
                return 2;
            }
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "usage: leaderboard_server [--port <n>] [--store <file>] [--ephemeral]\n"
                         "                         [--accounts <file>] [--identity <file>]\n"
                         "                         [--log-level trace|debug|info|warn|error|off]\n"
                         "\n"
                         "  --store <file>   score file. Accounts and the server identity default\n"
                         "                   to <file>.accounts.json and <file>.identity beside it.\n"
                         "                   With a directory (--store .) the standalone names are\n"
                         "                   used inside that directory instead.\n"
                         "  --ephemeral      keep scores in memory only; nothing is written.\n"
                         "                   Without a store, scores persist to\n"
                         "                   leaderboard_server.scores.txt.\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            return 2;
        }
    }

    // Accounts and the server identity are stable files. The identity must
    // outlive restarts or the public key pinned in the client stops matching,
    // so neither may default to an in-memory-only value.
    //
    // With --store given, the sidecars are named after the store file so one
    // prefix owns all the state. This composes paths rather than appending to
    // the string: "--store ." used to become "..accounts.json".
    const auto sidecar = [](const std::string& store, std::string_view suffix,
                            std::string_view fallback) -> std::string {
        if (store.empty()) {
            return std::string{fallback};
        }
        const std::filesystem::path path{store};
        const std::string stem = path.filename().string();
        if (stem.empty() || stem == "." || stem == "..") {
            // The store names a directory (".", "..", "data/"), so the sidecar
            // goes inside it under the standalone name.
            return (path / std::string{fallback}).string();
        }
        return (path.parent_path() / (stem + std::string{suffix})).string();
    };
    // Only an explicit --store groups the sidecars under one prefix; otherwise
    // they keep their standalone default names.
    const std::string store_prefix = store_explicit ? store_path : std::string{};

    // Scores persist by default; --ephemeral opts out. When the store names a
    // directory ("." or "data/") the score file goes inside it, instead of
    // trying to append to the directory itself (which silently dropped scores).
    const std::string score_path =
        ephemeral ? std::string{}
                  : sidecar(store_prefix, /*suffix=*/"", "leaderboard_server.scores.txt");

    if (account_path.empty()) {
        account_path = sidecar(store_prefix, ".accounts.json", "leaderboard_server.accounts.json");
    }
    if (identity_path.empty()) {
        // Pick the identity that matches whichever pin the client was built
        // with. An explicit leaderboard_server_pubkey.txt is the client's
        // authoritative pin, and this server cannot know which identity that
        // key belongs to, so it must not guess .server_identity in that case.
        // Set VKENGINE_SERVER_IDENTITY (or pass --identity) to be explicit.
        constexpr const char* kExampleIdentity = "examples/infinite_runner/secrets/.server_identity";
        constexpr const char* kExplicitPin = "examples/infinite_runner/secrets/leaderboard_server_pubkey.txt";
        if (const char* from_env = std::getenv("VKENGINE_SERVER_IDENTITY");
            from_env != nullptr && from_env[0] != '\0') {
            identity_path = from_env;
        } else if (!store_prefix.empty()) {
            identity_path = sidecar(store_prefix, ".identity", "leaderboard_server.identity");
        } else if (!std::filesystem::exists(kExplicitPin) &&
                   std::filesystem::exists(kExampleIdentity)) {
            // No explicit pin: the client's key was derived from this identity,
            // so using it guarantees the two agree.
            identity_path = kExampleIdentity;
        } else {
            identity_path = "leaderboard_server.identity";
        }
    }

    SetLogLevel(ParseLogLevel(log_level).value_or(LogLevel::Info));

    ServerOptions options;
    options.port = port;
    options.psk = LoadSessionPsk();
    options.store_path = score_path;
    options.account_path = account_path;
    options.identity_path = identity_path;
    options.accept_unknown_configs = true;

    Server server(std::move(options));
    if (!server.Start()) {
        std::cerr << "leaderboard_server: failed to bind port " << port << "\n";
        return 1;
    }
    std::cout << "leaderboard_server: listening on port " << server.Port() << "\n";
    std::cout << "leaderboard_server: server public key "
              << TokenToHex(server.PublicKey()) << "\n";

    server.Run();
    std::cout << "leaderboard_server: stopped\n";
    return 0;
}
