module;

export module VulkanEngine.KeyExchange;

import std;

export namespace VulkanEngine::Security {

// ─────────────────────────────────────────────────────────────────────────────
// X25519 key agreement for the session handshake.
//
// The client pins the server's long-term public key; both sides generate an
// ephemeral keypair per connection and derive a shared secret from the
// exchange. The raw X25519 output is not uniformly random, so it is always run
// through a KDF before use (see DeriveKey in DataCipher).
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::size_t kX25519KeyBytes = 32;

using X25519Key = std::array<std::byte, kX25519KeyBytes>;

struct X25519KeyPair {
    X25519Key secret{};
    X25519Key public_key{};
};

// Generates a fresh random secret and its public key.
[[nodiscard]] X25519KeyPair GenerateX25519KeyPair();

// Deterministic derivation of the public key from a stored secret (used by the
// client to turn a pinned server public key check into a no-op, and by the
// server to load its long-term identity).
[[nodiscard]] X25519Key X25519PublicFromSecret(const X25519Key& secret);

// Shared secret for (our secret, their public). Nullopt for a degenerate result
// (an all-zero secret, which a low-order peer public key produces).
[[nodiscard]] std::optional<X25519Key> X25519SharedSecret(const X25519Key& secret,
                                                          const X25519Key& peer_public);

} // namespace VulkanEngine::Security
