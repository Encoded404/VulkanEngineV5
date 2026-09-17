module;

module Examples.InfiniteRunner.Leaderboard.SessionCrypto;

import std;

import VulkanEngine.DataCipher;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

// Nonce encoded big-endian in the trailing 8 bytes; the rest stays zero. Safe
// because the sequence number is unique within a session.
[[nodiscard]] std::vector<std::byte> NonceFromSequence(std::size_t nonce_bytes, std::uint64_t seq) {
    std::vector<std::byte> nonce(nonce_bytes, std::byte{0});
    for (std::size_t i = 0; i < 8 && i < nonce_bytes; ++i) {
        nonce[nonce_bytes - 1 - i] = static_cast<std::byte>((seq >> (8 * i)) & 0xFFU);
    }
    return nonce;
}

[[nodiscard]] VulkanEngine::Security::KeyRing SessionKeyRing(std::span<const std::byte> session_key) {
    VulkanEngine::Security::KeyRing ring;
    ring.Add(kSessionKeyId, session_key);
    return ring;
}

} // namespace

namespace {

[[nodiscard]] std::vector<std::byte> MakeSessionSalt(std::span<const std::byte> client_nonce,
                                                      std::span<const std::byte> server_nonce,
                                                      std::uint64_t config_hash) {
    std::vector<std::byte> salt;
    salt.reserve(client_nonce.size() + server_nonce.size() + 8);
    salt.insert(salt.end(), client_nonce.begin(), client_nonce.end());
    salt.insert(salt.end(), server_nonce.begin(), server_nonce.end());
    for (int i = 0; i < 8; ++i) {
        salt.push_back(static_cast<std::byte>((config_hash >> (8 * i)) & 0xFFU));
    }
    return salt;
}

} // namespace

std::vector<std::byte> DeriveX25519SessionKey(std::span<const std::byte> shared_secret,
                                              std::span<const std::byte> client_nonce,
                                              std::span<const std::byte> server_nonce,
                                              std::uint64_t config_hash) {
    const std::vector<std::byte> salt = MakeSessionSalt(client_nonce, server_nonce, config_hash);
    return VulkanEngine::Security::DeriveKey(shared_secret, salt, "IRLB-session-v2", 32);
}

std::vector<std::byte> SealMessage(std::span<const std::byte> session_key,
                                   VulkanEngine::Security::CipherVariant variant,
                                   std::uint64_t seq,
                                   std::span<const std::byte> aad,
                                   std::span<const std::byte> plaintext) {
    const std::optional<std::size_t> nonce_bytes = VulkanEngine::Security::NonceBytes(variant);
    if (!nonce_bytes.has_value() || *nonce_bytes == 0) {
        return {};
    }
    const std::vector<std::byte> nonce = NonceFromSequence(*nonce_bytes, seq);
    const VulkanEngine::Security::KeyRing ring = SessionKeyRing(session_key);
    return VulkanEngine::Security::Seal(ring, variant, kSessionKeyId, nonce, plaintext, aad);
}

std::optional<std::vector<std::byte>> OpenMessage(std::span<const std::byte> session_key,
                                                  VulkanEngine::Security::CipherVariant /*variant*/,
                                                  std::uint64_t /*seq*/,
                                                  std::span<const std::byte> aad,
                                                  std::span<const std::byte> sealed) {
    // The nonce and variant travel in the envelope header, so opening only
    // needs the key and the AAD the sender bound.
    const VulkanEngine::Security::KeyRing ring = SessionKeyRing(session_key);
    return VulkanEngine::Security::Open(ring, sealed, aad);
}

} // namespace Examples::InfiniteRunner::Leaderboard
