#include <gtest/gtest.h>

import std;
import std.compat;

import VulkanEngine.PasswordHash;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.AccountStore;

namespace {

using namespace Examples::InfiniteRunner::Leaderboard;

VulkanEngine::Security::Argon2Params TestArgon2() {
    VulkanEngine::Security::Argon2Params params{};
    params.blocks = 64;
    params.passes = 2;
    params.lanes = 1;
    params.hash_bytes = 32;
    params.salt_bytes = 16;
    return params;
}

AccountStoreOptions MemoryStore() {
    AccountStoreOptions options{};
    options.argon2 = TestArgon2();
    return options;
}

TEST(UsernameTest, CanonicalizesAndValidates) {
    EXPECT_EQ(CanonicalizeUsername("  Alice  "), std::optional<std::string>{"alice"});
    EXPECT_FALSE(CanonicalizeUsername("ab").has_value());          // too short
    EXPECT_FALSE(CanonicalizeUsername("1alice").has_value());      // must start with a letter
    EXPECT_FALSE(CanonicalizeUsername("al ice").has_value());      // space
    EXPECT_FALSE(CanonicalizeUsername("alice!").has_value());      // punctuation
}

TEST(DisplayNameTest, SanitizesDisplayNames) {
    EXPECT_EQ(SanitizeDisplayName("  Alice  "), std::optional<std::string>{"Alice"});
    EXPECT_FALSE(SanitizeDisplayName("").has_value());
    EXPECT_FALSE(SanitizeDisplayName("bad\nname").has_value());
}

TEST(AccountStoreTest, RegisterLoginRoundTrip) {
    AccountStore store(MemoryStore());
    const auto registered = store.Register("Alice", "Alice");
    ASSERT_EQ(registered.status, SyncStatus::Ok);
    EXPECT_EQ(registered.display_name, "Alice");
    EXPECT_EQ(registered.token.size(), kTokenHexLength);

    const auto login = store.Login("alice", registered.token);
    EXPECT_EQ(login.status, SyncStatus::Ok);
    EXPECT_EQ(login.account.id, registered.user_id);
    EXPECT_EQ(login.account.username, "alice");
}

TEST(AccountStoreTest, RejectsDuplicateUsernameIgnoringCase) {
    AccountStore store(MemoryStore());
    ASSERT_EQ(store.Register("alice", "Alice").status, SyncStatus::Ok);
    EXPECT_EQ(store.Register("ALICE", "Other").status, SyncStatus::UsernameTaken);
}

TEST(AccountStoreTest, RejectsWrongToken) {
    AccountStore store(MemoryStore());
    const auto registered = store.Register("alice", "Alice");
    ASSERT_EQ(registered.status, SyncStatus::Ok);

    std::string wrong = registered.token;
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    EXPECT_EQ(store.Login("alice", wrong).status, SyncStatus::InvalidToken);
    EXPECT_EQ(store.Login("alice", "not-hex").status, SyncStatus::InvalidToken);
    EXPECT_EQ(store.Login("nobody", registered.token).status, SyncStatus::UnknownAccount);
}

TEST(AccountStoreTest, RunIdsAreIdempotent) {
    AccountStore store(MemoryStore());
    const auto registered = store.Register("alice", "Alice");
    ASSERT_EQ(registered.status, SyncStatus::Ok);

    EXPECT_EQ(store.RecordRun(registered.user_id, 1234), AccountStore::ScoreResult::Accepted);
    EXPECT_EQ(store.RecordRun(registered.user_id, 1234), AccountStore::ScoreResult::Duplicate);
    EXPECT_EQ(store.RecordRun(registered.user_id, 5678), AccountStore::ScoreResult::Accepted);
    EXPECT_EQ(store.RecordRun(999999, 1), AccountStore::ScoreResult::UnknownAccount);
}

TEST(AccountStoreTest, SettingsUpdate) {
    AccountStore store(MemoryStore());
    const auto registered = store.Register("alice", "Alice");
    ASSERT_EQ(registered.status, SyncStatus::Ok);

    SyncedSettings hidden{};
    hidden.show_on_leaderboard = false;
    const auto updated = store.UpdateSettings(registered.user_id, "Alice Cooper", hidden);
    EXPECT_EQ(updated.status, SyncStatus::Ok);
    EXPECT_EQ(updated.account.display_name, "Alice Cooper");
    EXPECT_FALSE(updated.settings.show_on_leaderboard);

    EXPECT_EQ(store.UpdateSettings(registered.user_id, "bad\nname", hidden).status,
              SyncStatus::InvalidDisplayName);
}

TEST(AccountStoreTest, PersistsAcrossReload) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "vkengine_account_store_test.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    std::string token;
    UserId user_id = 0;
    {
        AccountStoreOptions options = MemoryStore();
        options.path = path;
        AccountStore store(options);
        const auto registered = store.Register("alice", "Alice");
        ASSERT_EQ(registered.status, SyncStatus::Ok);
        token = registered.token;
        user_id = registered.user_id;
    }
    {
        AccountStoreOptions options = MemoryStore();
        options.path = path;
        AccountStore store(options);
        EXPECT_EQ(store.AccountCount(), 1U);
        const auto login = store.Login("alice", token);
        EXPECT_EQ(login.status, SyncStatus::Ok);
        EXPECT_EQ(login.account.id, user_id);
    }
    std::filesystem::remove(path, ec);
}

TEST(SessionStoreTest, IssuesResolvesAndRevokes) {
    SessionStoreOptions options{};
    options.ttl = std::chrono::hours(1);
    SessionStore sessions(options);

    const auto issued = sessions.Issue(42);
    ASSERT_FALSE(issued.token.empty());
    EXPECT_EQ(sessions.Resolve(issued.token), std::optional<UserId>{42});
    EXPECT_FALSE(sessions.Resolve("deadbeef").has_value());

    sessions.Revoke(issued.token);
    EXPECT_FALSE(sessions.Resolve(issued.token).has_value());
}

} // namespace
