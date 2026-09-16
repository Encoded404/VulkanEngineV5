#include <gtest/gtest.h>

import std;
import std.compat;

import VulkanEngine.PasswordHash;

namespace {

using VulkanEngine::Security::Argon2Params;
using VulkanEngine::Security::ConstantTimeEquals;
using VulkanEngine::Security::HashPassword;
using VulkanEngine::Security::VerifyPassword;

// Small work factors keep the suite fast; the format, not the cost, is under
// test here.
Argon2Params TestParams() {
    Argon2Params params{};
    params.blocks = 64;
    params.passes = 2;
    params.lanes = 1;
    params.hash_bytes = 32;
    params.salt_bytes = 16;
    return params;
}

std::span<const std::byte> AsBytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

TEST(PasswordHashTest, VerifiesTheRightPassword) {
    const std::string verifier = HashPassword(AsBytes("correct horse"), TestParams());
    ASSERT_FALSE(verifier.empty());
    EXPECT_TRUE(VerifyPassword(AsBytes("correct horse"), verifier));
}

TEST(PasswordHashTest, RejectsTheWrongPassword) {
    const std::string verifier = HashPassword(AsBytes("correct horse"), TestParams());
    ASSERT_FALSE(verifier.empty());
    EXPECT_FALSE(VerifyPassword(AsBytes("correct hors"), verifier));
    EXPECT_FALSE(VerifyPassword(AsBytes(""), verifier));
}

TEST(PasswordHashTest, SaltsAreUniquePerHash) {
    const std::string a = HashPassword(AsBytes("same"), TestParams());
    const std::string b = HashPassword(AsBytes("same"), TestParams());
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    EXPECT_NE(a, b);
}

TEST(PasswordHashTest, RejectsMalformedVerifiers) {
    EXPECT_FALSE(VerifyPassword(AsBytes("x"), ""));
    EXPECT_FALSE(VerifyPassword(AsBytes("x"), "argon2id$64$2$1$00$00"));
    EXPECT_FALSE(VerifyPassword(AsBytes("x"), "bcrypt$64$2$1$00$00"));
    EXPECT_FALSE(VerifyPassword(AsBytes("x"), "argon2id$0$0$0$zz$zz"));
}

TEST(PasswordHashTest, ConstantTimeEqualsBehaviour) {
    const std::array<std::byte, 4> a{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<std::byte, 4> b{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::array<std::byte, 4> c{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{5}};
    const std::array<std::byte, 3> d{std::byte{1}, std::byte{2}, std::byte{3}};
    EXPECT_TRUE(ConstantTimeEquals(a, b));
    EXPECT_FALSE(ConstantTimeEquals(a, c));
    EXPECT_FALSE(ConstantTimeEquals(a, d));
}

} // namespace
