#include <gtest/gtest.h>

import std;
import std.compat;

import VulkanEngine.KeyExchange;

namespace {

using VulkanEngine::Security::GenerateX25519KeyPair;
using VulkanEngine::Security::kX25519KeyBytes;
using VulkanEngine::Security::X25519Key;
using VulkanEngine::Security::X25519PublicFromSecret;
using VulkanEngine::Security::X25519SharedSecret;

TEST(KeyExchangeTest, BothSidesDeriveTheSameSecret) {
    const auto alice = GenerateX25519KeyPair();
    const auto bob = GenerateX25519KeyPair();
    ASSERT_NE(alice.secret, bob.secret);

    const auto alice_view = X25519SharedSecret(alice.secret, bob.public_key);
    const auto bob_view = X25519SharedSecret(bob.secret, alice.public_key);
    ASSERT_TRUE(alice_view.has_value());
    ASSERT_TRUE(bob_view.has_value());
    EXPECT_EQ(*alice_view, *bob_view);
}

TEST(KeyExchangeTest, PublicKeyIsDeterministic) {
    const auto pair = GenerateX25519KeyPair();
    EXPECT_EQ(X25519PublicFromSecret(pair.secret), pair.public_key);
}

TEST(KeyExchangeTest, RejectsDegeneratePeerPublicKey) {
    const auto pair = GenerateX25519KeyPair();
    const X25519Key zero{};
    EXPECT_FALSE(X25519SharedSecret(pair.secret, zero).has_value());
}

TEST(KeyExchangeTest, GeneratedKeysAreWellFormed) {
    const auto pair = GenerateX25519KeyPair();
    bool secret_nonzero = false;
    bool public_nonzero = false;
    for (std::size_t i = 0; i < kX25519KeyBytes; ++i) {
        secret_nonzero = secret_nonzero || pair.secret[i] != std::byte{0};
        public_nonzero = public_nonzero || pair.public_key[i] != std::byte{0};
    }
    EXPECT_TRUE(secret_nonzero);
    EXPECT_TRUE(public_nonzero);
}

} // namespace
