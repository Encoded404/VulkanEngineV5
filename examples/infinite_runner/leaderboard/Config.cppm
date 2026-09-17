module;

export module Examples.InfiniteRunner.Leaderboard.Config;

import std;

import VulkanEngine.KeyExchange;
import Examples.InfiniteRunner.Leaderboard.Account;

export namespace Examples::InfiniteRunner::Leaderboard {

// Names of the sealed entries this example reads. They live in
// examples/infinite_runner/secrets and are sealed into the generated module.
inline constexpr std::string_view kEndpointSecret = "leaderboard_endpoint.txt";
inline constexpr std::string_view kServerPublicKeySecret = "leaderboard_server_pubkey.txt";

// Hand-maintained list of ruleset fingerprints the server accepts, one 64-bit
// hex hash per line, `#` comments allowed. The current build's hash must be in
// it; the server refuses to start otherwise and rejects any config that is not
// listed.
inline constexpr std::string_view kAcceptedConfigsDefault =
    "examples/infinite_runner/leaderboard/accepted_configs.txt";

// Hand-maintained username/display-name content policy. `block <token>` entries
// are blocked substrings; `allow <token>` entries exempt the span they cover,
// so one allow prefix covers every name built on it.
inline constexpr std::string_view kNamesPolicyDefault =
    "examples/infinite_runner/leaderboard/names_policy.txt";

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

// "host:port" from the sealed endpoint entry; nullopt if it is absent or
// malformed (for example on a fresh clone with no local secrets).
[[nodiscard]] std::optional<Endpoint> LoadEndpoint();

// Pinned server X25519 public key (64 hex characters). Required: without it the
// client cannot perform the authenticated handshake.
[[nodiscard]] std::optional<VulkanEngine::Security::X25519Key> LoadServerPublicKey();

// Fingerprint of the ruleset this binary was built with. Scores are bucketed
// by this value on the server.
[[nodiscard]] std::uint64_t CurrentBalanceHash();

// Parses an accepted-configs file. Returns an empty vector when the file is
// absent or unreadable.
[[nodiscard]] std::vector<std::uint64_t> LoadAcceptedConfigs(const std::filesystem::path& path);

// Parses a names-policy file. Returns an empty policy when the file is absent
// or unreadable. Entries are single whitespace-free tokens; multi-word entries
// cannot match the tokenized name and are warned about and skipped.
[[nodiscard]] NamePolicy LoadNamePolicy(const std::filesystem::path& path);

} // namespace Examples::InfiniteRunner::Leaderboard
