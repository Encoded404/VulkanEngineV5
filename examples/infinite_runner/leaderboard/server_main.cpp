#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

import std;

import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.Config;
import Examples.InfiniteRunner.Leaderboard.Log;
import Examples.InfiniteRunner.Leaderboard.Server;

namespace {

// Directory the running executable lives in. argv[0] is the portable way to
// find it; weakly_canonical resolves a relative invocation ("build/bin/...")
// without requiring the file to already exist in canonical form.
[[nodiscard]] std::filesystem::path ExecutableDirectory(const char* argv0) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(argv0, ec);
    if (ec) {
        return {};
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
    return (ec ? absolute : canonical).parent_path();
}

// Resolves a committed default without assuming where the server was launched
// from. Each anchor (the working directory, then the executable's directory and
// each ancestor) is tried with the full repo-relative path first and then with
// the bare filename, so both a source/build tree and a deployed directory with
// the file copied next to the binary work. Returns `relative` unchanged when
// nothing matches, and the caller reports that.
[[nodiscard]] std::filesystem::path ResolveDefault(std::string_view relative,
                                                   const std::filesystem::path& executable_dir) {
    const std::filesystem::path rel{relative};
    if (rel.is_absolute()) {
        return rel;
    }
    const std::filesystem::path basename = rel.filename();

    std::vector<std::filesystem::path> anchors;
    anchors.emplace_back(); // the working directory, as an empty prefix
    for (std::filesystem::path dir = executable_dir; !dir.empty();) {
        anchors.push_back(dir);
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir || parent.empty()) {
            break;
        }
        dir = parent;
    }

    std::error_code ec;
    for (const std::filesystem::path& anchor : anchors) {
        const std::filesystem::path full = anchor / rel;
        if (std::filesystem::exists(full, ec)) {
            return full;
        }
        if (!basename.empty() && basename != rel) {
            const std::filesystem::path flat = anchor / basename;
            if (std::filesystem::exists(flat, ec)) {
                return flat;
            }
        }
    }
    return rel;
}

} // namespace

int main(int argc, char** argv) {
    using namespace Examples::InfiniteRunner::Leaderboard;

    std::uint16_t port = 7777;
    std::string store_path;
    bool store_explicit = false;
    bool ephemeral = false;
    std::string account_path;
    std::string identity_path;
    // The committed policy files default to a repo-relative path, but the
    // server is usually started from its build output directory. Resolve them
    // against the working directory and then the executable's directory and
    // ancestors, so no --configs/--names is needed in the common case.
    const std::filesystem::path executable_dir = ExecutableDirectory(argv[0]);
    std::string configs_path = ResolveDefault(kAcceptedConfigsDefault, executable_dir).string();
    std::string names_path = ResolveDefault(kNamesPolicyDefault, executable_dir).string();
    std::string legacy_path;
    std::string log_level = "info";
    std::size_t per_account = 32;
    std::chrono::seconds idle_timeout{90};

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
        } else if (argument == "--configs") {
            if (!next(configs_path)) {
                std::cerr << "--configs needs a value\n";
                return 2;
            }
        } else if (argument == "--check-configs") {
            // Build-time gate: fail the build when BalanceConfig changed but the
            // new fingerprint was not added to the accepted-configs list.
            if (!next(value)) {
                std::cerr << "--check-configs needs a value\n";
                return 2;
            }
            const std::vector<std::uint64_t> configs = LoadAcceptedConfigs(value);
            const std::uint64_t current = CurrentBalanceHash();
            if (std::find(configs.begin(), configs.end(), current) == configs.end()) {
                std::cerr << std::format(
                    "leaderboard_server: current ruleset {:#x} is not listed in {}\n"
                    "(add it before building so clients on this balance are accepted)\n",
                    current, value);
                return 1;
            }
            std::cout << std::format("leaderboard_server: ruleset {:#x} is accepted\n", current);
            return 0;
        } else if (argument == "--names") {
            if (!next(names_path)) {
                std::cerr << "--names needs a value\n";
                return 2;
            }
        } else if (argument == "--legacy-store") {
            if (!next(legacy_path)) {
                std::cerr << "--legacy-store needs a value\n";
                return 2;
            }
        } else if (argument == "--per-account") {
            if (!next(value)) {
                std::cerr << "--per-account needs a value\n";
                return 2;
            }
            per_account = static_cast<std::size_t>(std::atoi(value.c_str()));
            if (per_account == 0) {
                std::cerr << "--per-account must be positive\n";
                return 2;
            }
        } else if (argument == "--idle-timeout") {
            if (!next(value)) {
                std::cerr << "--idle-timeout needs a value\n";
                return 2;
            }
            const int seconds = std::atoi(value.c_str());
            if (seconds <= 0) {
                std::cerr << "--idle-timeout must be positive\n";
                return 2;
            }
            idle_timeout = std::chrono::seconds{seconds};
        } else if (argument == "--log-level") {
            if (!next(log_level)) {
                std::cerr << "--log-level needs a value\n";
                return 2;
            }
        } else if (argument == "--help" || argument == "-h") {
            std::cout
                << "usage: leaderboard_server [options]\n"
                   "\n"
                   "  --port <n>        TCP port to bind (default 7777; 0 picks a free port).\n"
                   "  --store <dir>     directory holding one JSON board per ruleset, with\n"
                   "                    <dir>.accounts.json and <dir>.identity beside it. Without\n"
                   "                    --store these default to leaderboard_server.scores/,\n"
                   "                    leaderboard_server.accounts.json and\n"
                   "                    leaderboard_server.identity in the working directory.\n"
                   "  --ephemeral       keep scores in memory only; nothing is written.\n"
                   "  --accounts <file> account database (default: beside --store, else the\n"
                   "                    working directory).\n"
                   "  --identity <file> server X25519 private key, created when absent\n"
                   "                    (default: beside --store, else the working directory).\n"
                   "  --configs <file>  accepted ruleset fingerprints, one hex hash per line.\n"
                   "                    Default: examples/infinite_runner/leaderboard/\n"
                   "                    accepted_configs.txt.\n"
                   "  --names <file>    username/display-name policy; 'block <token>' and\n"
                   "                    'allow <token>' lines. Default:\n"
                   "                    examples/infinite_runner/leaderboard/names_policy.txt.\n"
                   "  --legacy-store <file>  old line-based score file to import once.\n"
                   "  --per-account <n> scores retained per account (default 32).\n"
                   "  --idle-timeout <s> close a connection that sends nothing for this many\n"
                   "                    seconds; the client keepalive keeps it warm (default 90).\n"
                   "  --log-level <lvl> trace|debug|info|warn|error|off (default info).\n"
                   "  --check-configs <file>  verify the current ruleset is listed, then exit.\n"
                   "  -h, --help        this help.\n"
                   "\n"
                   "The --configs and --names defaults are looked up in the working directory,\n"
                   "then in the executable's directory and each of its ancestors, under the full\n"
                   "relative path and then by filename, so a deployed server finds a copy placed\n"
                   "next to the binary.\n";
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
    // With --store given, the sidecars are named after the store path so one
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

    // Scores persist by default; --ephemeral opts out. The store is a directory
    // holding one JSON file per ruleset.
    const std::string score_path =
        ephemeral ? std::string{}
                  : sidecar(store_prefix, ".scores", "leaderboard_server.scores");

    if (account_path.empty()) {
        account_path = sidecar(store_prefix, ".accounts.json", "leaderboard_server.accounts.json");
    }
    if (identity_path.empty()) {
        // Pick the identity that matches whichever pin the client was built
        // with. An explicit leaderboard_server_pubkey.txt is the client's
        // authoritative pin, and this server cannot know which identity that
        // key belongs to, so it must not guess .server_identity in that case.
        // Set VKENGINE_SERVER_IDENTITY (or pass --identity) to be explicit.
        const std::filesystem::path example_identity =
            ResolveDefault("examples/infinite_runner/secrets/.server_identity", executable_dir);
        const std::filesystem::path explicit_pin =
            ResolveDefault("examples/infinite_runner/secrets/leaderboard_server_pubkey.txt",
                           executable_dir);
        if (const char* from_env = std::getenv("VKENGINE_SERVER_IDENTITY");
            from_env != nullptr && from_env[0] != '\0') {
            identity_path = from_env;
        } else if (!store_prefix.empty()) {
            identity_path = sidecar(store_prefix, ".identity", "leaderboard_server.identity");
        } else if (!std::filesystem::exists(explicit_pin) &&
                   std::filesystem::exists(example_identity)) {
            // No explicit pin: the client's key was derived from this identity,
            // so using it guarantees the two agree.
            identity_path = example_identity.string();
        } else {
            identity_path = "leaderboard_server.identity";
        }
    }

    SetLogLevel(ParseLogLevel(log_level).value_or(LogLevel::Info));
    // Make the resolved file locations visible; a silent default miss was the
    // reason the policy files appeared not to load from the build directory.
    LogMessage(LogLevel::Info, std::format("server: configs {}", configs_path));
    LogMessage(LogLevel::Info, std::format("server: names   {}", names_path));

    ServerOptions options;
    options.port = port;
    options.store_path = score_path;
    options.account_path = account_path;
    options.identity_path = identity_path;
    options.max_scores_per_account = per_account;
    options.idle_timeout = idle_timeout;
    // Username/display-name content policy. Refusals carry a reason back to the
    // client, so the list stays focused on clear abuse.
    options.name_policy = LoadNamePolicy(names_path);
    if (options.name_policy.blocked.empty() && options.name_policy.allowed.empty()) {
        std::cerr << "leaderboard_server: warning: no names policy at " << names_path << "\n";
    }

    if (!legacy_path.empty()) {
        options.legacy_store_path = legacy_path;
    } else if (!ephemeral) {
        const std::filesystem::path legacy_default =
            ResolveDefault("leaderboard_server.scores.txt", executable_dir);
        if (std::filesystem::exists(legacy_default)) {
            options.legacy_store_path = legacy_default;
        }
    }

    options.accepted_configs = LoadAcceptedConfigs(configs_path);
    if (options.accepted_configs.empty()) {
        std::cerr << "leaderboard_server: warning: no accepted-configs list at " << configs_path
                  << "; serving the current ruleset only\n";
        options.accepted_configs.push_back(CurrentBalanceHash());
    }

    Server server(std::move(options));
    if (!server.Start()) {
        std::cerr << "leaderboard_server: failed to start (see log); port " << port << "\n";
        return 1;
    }
    std::cout << "leaderboard_server: listening on port " << server.Port() << "\n";
    std::cout << "leaderboard_server: server public key "
              << TokenToHex(server.PublicKey()) << "\n";

    server.Run();
    std::cout << "leaderboard_server: stopped\n";
    return 0;
}
