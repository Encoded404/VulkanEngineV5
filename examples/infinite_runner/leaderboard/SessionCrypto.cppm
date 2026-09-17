module;

export module Examples.InfiniteRunner.Leaderboard.SessionCrypto;

import std;

import VulkanEngine.DataCipher;

export namespace Examples::InfiniteRunner::Leaderboard {

// Session-key derivation and per-message AEAD.
//
// The session key is derived from the X25519 shared secret plus both handshake
// nonces and the ruleset fingerprint, so a session is bound to one ruleset and
// cannot be replayed between them. Message nonces are derived from the frame
// sequence number, which is unique per session.

inline constexpr std::uint8_t kSessionKeyId = 1;

// Session key from the X25519 shared secret. The raw DH output is not uniform,
// so it is always run through the KDF.
[[nodiscard]] std::vector<std::byte> DeriveX25519SessionKey(
    std::span<const std::byte> shared_secret,
    std::span<const std::byte> client_nonce,
    std::span<const std::byte> server_nonce,
    std::uint64_t config_hash);

[[nodiscard]] std::vector<std::byte> SealMessage(std::span<const std::byte> session_key,
                                                 VulkanEngine::Security::CipherVariant variant,
                                                 std::uint64_t seq,
                                                 std::span<const std::byte> aad,
                                                 std::span<const std::byte> plaintext);

[[nodiscard]] std::optional<std::vector<std::byte>> OpenMessage(
    std::span<const std::byte> session_key,
    VulkanEngine::Security::CipherVariant variant,
    std::uint64_t seq,
    std::span<const std::byte> aad,
    std::span<const std::byte> sealed);

} // namespace Examples::InfiniteRunner::Leaderboard
