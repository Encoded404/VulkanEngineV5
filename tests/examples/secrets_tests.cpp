#include <gtest/gtest.h>

import std;
import std.compat;

import Examples.InfiniteRunner.Secrets;

namespace {

using namespace Examples::InfiniteRunner::Secrets;

// These tests are guarded on the presence of locally sealed secrets: a fresh
// clone has only the committed templates, so the generated module is a stub.
// They validate the generator/runtime contract rather than specific values.

TEST(SecretsTest, EverySealedEntryOpens) {
    if (Entries().empty()) {
        GTEST_SKIP() << "no local secrets in examples/infinite_runner/secrets";
    }
    for (const Entry& entry : Entries()) {
        const std::optional<std::vector<std::byte>> raw = GetRaw(entry.name);
        EXPECT_TRUE(raw.has_value()) << entry.name;
        EXPECT_GT(entry.sealed.size(), 0U) << entry.name;
    }
}

TEST(SecretsTest, UnknownNameReturnsNullopt) {
    EXPECT_FALSE(GetRaw("definitely-not-a-secret").has_value());
    EXPECT_FALSE(GetString("definitely-not-a-secret").has_value());
    EXPECT_FALSE(GetInt("definitely-not-a-secret").has_value());
}

TEST(SecretsTest, AccessorsAgreeForTextEntries) {
    if (Find("leaderboard_endpoint.txt") == nullptr) {
        GTEST_SKIP() << "leaderboard_endpoint.txt not present locally";
    }
    const std::optional<std::string> text = GetString("leaderboard_endpoint.txt");
    ASSERT_TRUE(text.has_value());
    const std::optional<std::vector<std::byte>> raw = GetRaw("leaderboard_endpoint.txt");
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(text->size(), raw->size());
}

} // namespace
