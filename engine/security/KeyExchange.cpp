module;

#include "monocypher.h"

module VulkanEngine.KeyExchange;

import std;

import VulkanEngine.DataCipher;

namespace VulkanEngine::Security {

namespace {

void ToU8(const X25519Key& key, std::uint8_t out[kX25519KeyBytes]) {
    std::memcpy(out, key.data(), kX25519KeyBytes);
}

[[nodiscard]] X25519Key FromU8(const std::uint8_t in[kX25519KeyBytes]) {
    X25519Key key{};
    std::memcpy(key.data(), in, kX25519KeyBytes);
    return key;
}

} // namespace

X25519KeyPair GenerateX25519KeyPair() {
    X25519KeyPair pair{};
    const std::vector<std::byte> secret = RandomBytes(kX25519KeyBytes);
    if (secret.size() != kX25519KeyBytes) {
        return pair;
    }
    std::memcpy(pair.secret.data(), secret.data(), kX25519KeyBytes);
    pair.public_key = X25519PublicFromSecret(pair.secret);
    return pair;
}

X25519Key X25519PublicFromSecret(const X25519Key& secret) {
    std::uint8_t secret_bytes[kX25519KeyBytes];
    ToU8(secret, secret_bytes);

    std::uint8_t public_bytes[kX25519KeyBytes];
    crypto_x25519_public_key(public_bytes, secret_bytes);
    return FromU8(public_bytes);
}

std::optional<X25519Key> X25519SharedSecret(const X25519Key& secret, const X25519Key& peer_public) {
    std::uint8_t secret_bytes[kX25519KeyBytes];
    std::uint8_t peer_bytes[kX25519KeyBytes];
    ToU8(secret, secret_bytes);
    ToU8(peer_public, peer_bytes);

    std::uint8_t shared_bytes[kX25519KeyBytes];
    crypto_x25519(shared_bytes, secret_bytes, peer_bytes);

    // A low-order peer public key yields an all-zero secret. Reject it rather
    // than deriving a predictable session key from it.
    std::uint8_t aggregate = 0;
    for (const std::uint8_t byte : shared_bytes) {
        aggregate |= byte;
    }
    crypto_wipe(secret_bytes, sizeof(secret_bytes));
    if (aggregate == 0) {
        crypto_wipe(shared_bytes, sizeof(shared_bytes));
        return std::nullopt;
    }
    return FromU8(shared_bytes);
}

} // namespace VulkanEngine::Security
