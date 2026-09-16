module;

export module Examples.InfiniteRunner.Leaderboard.Config;

import std;

import VulkanEngine.KeyExchange;

export namespace Examples::InfiniteRunner::Leaderboard {

// Names of the sealed entries this example reads. They live in
// examples/infinite_runner/secrets and are sealed into the generated module.
inline constexpr std::string_view kEndpointSecret = "leaderboard_endpoint.txt";
inline constexpr std::string_view kPskSecret = "leaderboard_psk.txt";
inline constexpr std::string_view kServerPublicKeySecret = "leaderboard_server_pubkey.txt";

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

// "host:port" from the sealed endpoint entry; nullopt if it is absent or
// malformed (for example on a fresh clone with no local secrets).
[[nodiscard]] std::optional<Endpoint> LoadEndpoint();

// Shared session secret. Falls back to a documented development key when no
// sealed PSK is present, so a local client and server can still talk.
[[nodiscard]] std::vector<std::byte> LoadSessionPsk();

// Pinned server X25519 public key (64 hex characters). Absent means the client
// falls back to the v1 PSK handshake.
[[nodiscard]] std::optional<VulkanEngine::Security::X25519Key> LoadServerPublicKey();

// Fingerprint of the ruleset this binary was built with. Scores are bucketed
// by this value on the server.
[[nodiscard]] std::uint64_t CurrentBalanceHash();

} // namespace Examples::InfiniteRunner::Leaderboard
