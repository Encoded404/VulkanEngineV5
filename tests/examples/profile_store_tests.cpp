#include <gtest/gtest.h>

import std;
import std.compat;

import VulkanShared.Storage;
import VulkanShared.UserPaths;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Account.ProfileStore;

namespace {

using Examples::InfiniteRunner::Account::ProfileStore;

std::filesystem::path ScratchBase() {
    static std::atomic<unsigned> counter{0};
    const auto nonce = static_cast<unsigned>(std::random_device{}());
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() /
        std::format("vkengine_profile_store_{}_{}", nonce, counter.fetch_add(1));
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

VulkanShared::Storage::Storage MakeStorage(const std::filesystem::path& base) {
    VulkanShared::UserPaths::Roots roots{};
    roots.persistent = base;
    roots.cache = base / "cache";
    roots.log = base / "logs";
    VulkanShared::Storage::Options options{};
    options.durable_writes = false;
    auto storage = VulkanShared::Storage::Storage::CreateWithRoots(std::move(roots), options);
    EXPECT_TRUE(storage.has_value());
    return std::move(*storage);
}

TEST(ProfileStoreTest, MultipleProfilesAreIsolated) {
    const std::filesystem::path base = ScratchBase();
    VulkanShared::Storage::Storage storage = MakeStorage(base);

    auto store = ProfileStore::Create(storage);
    ASSERT_TRUE(store.has_value());

    const auto& alice = store->AddProfile("alice", "Alice");
    const std::string alice_id = alice.id;
    ASSERT_FALSE(alice_id.empty());
    store->SetToken(alice_id, std::string(64, 'a'));
    store->RecordRun(0x1234, 1, 100, "Alice");
    EXPECT_EQ(store->LocalBest(0x1234), 100);
    ASSERT_EQ(store->LocalTop(0x1234, 5).size(), 1U);
    EXPECT_EQ(store->LocalTop(0x1234, 5).front().score, 100);

    const auto& bob = store->AddProfile("bob", "Bob");
    EXPECT_EQ(store->ActiveId(), bob.id);
    // Ids are the ImGui row identity, so identical display names must still get
    // distinct ids.
    EXPECT_NE(alice_id, bob.id);
    // Bob's board is separate from Alice's.
    EXPECT_EQ(store->LocalBest(0x1234), 0);

    ASSERT_TRUE(store->SetActive(alice_id));
    EXPECT_EQ(store->Active()->username, "alice");
    EXPECT_EQ(store->LocalBest(0x1234), 100);
    EXPECT_EQ(store->Active()->token, std::string(64, 'a'));

    ASSERT_TRUE(store->RemoveProfile(bob.id));
    EXPECT_EQ(store->Profiles().size(), 1U);

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST(ProfileStoreTest, PersistsAcrossReload) {
    const std::filesystem::path base = ScratchBase();
    VulkanShared::Storage::Storage storage = MakeStorage(base);

    std::string alice_id;
    {
        auto store = ProfileStore::Create(storage);
        ASSERT_TRUE(store.has_value());
        alice_id = store->AddProfile("alice", "Alice").id;
        store->SetToken(alice_id, std::string(64, 'b'));
        store->RecordRun(0x99, 7, 55, "Alice");
        ASSERT_TRUE(store->Save());
    }

    {
        auto store = ProfileStore::Create(storage);
        ASSERT_TRUE(store.has_value());
        ASSERT_EQ(store->Profiles().size(), 1U);
        EXPECT_EQ(store->ActiveId(), alice_id);
        ASSERT_NE(store->Active(), nullptr);
        EXPECT_EQ(store->Active()->display_name, "Alice");
        EXPECT_EQ(store->Active()->token, std::string(64, 'b'));
        EXPECT_EQ(store->LocalBest(0x99), 55);
    }

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST(ProfileStoreTest, IdsAreUniqueEvenWithIdenticalNames) {
    const std::filesystem::path base = ScratchBase();
    VulkanShared::Storage::Storage storage = MakeStorage(base);

    auto store = ProfileStore::Create(storage);
    ASSERT_TRUE(store.has_value());

    const std::string first = store->AddProfile("alice", "Alice").id;
    const std::string second = store->AddProfile("alice", "Alice").id;
    const std::string third = store->AddProfile("alice", "Alice").id;
    EXPECT_NE(first, second);
    EXPECT_NE(second, third);
    EXPECT_NE(first, third);

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST(ProfileStoreTest, DeleteRemovesProfileAndItsCredential) {
    const std::filesystem::path base = ScratchBase();
    VulkanShared::Storage::Storage storage = MakeStorage(base);

    std::string id;
    {
        auto store = ProfileStore::Create(storage);
        ASSERT_TRUE(store.has_value());
        id = store->AddProfile("alice", "Alice").id;
        store->SetToken(id, std::string(64, 'c'));
        store->RecordRun(0x1, 5, 10, "Alice");
        ASSERT_TRUE(std::filesystem::exists(storage.SavePath("profile-" + id)));

        ASSERT_TRUE(store->RemoveProfile(id));
        EXPECT_TRUE(store->Profiles().empty());
        EXPECT_TRUE(store->ActiveId().empty());
        // The private slot holding the token must not survive the profile.
        EXPECT_FALSE(std::filesystem::exists(storage.SavePath("profile-" + id)));
    }
    {
        auto store = ProfileStore::Create(storage);
        ASSERT_TRUE(store.has_value());
        EXPECT_TRUE(store->Profiles().empty());
    }

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST(ProfileStoreTest, DisplayNameAndSyncedSettingsUpdate) {
    const std::filesystem::path base = ScratchBase();
    VulkanShared::Storage::Storage storage = MakeStorage(base);

    auto store = ProfileStore::Create(storage);
    ASSERT_TRUE(store.has_value());
    const std::string id = store->AddProfile("carol", "Carol").id;

    ASSERT_TRUE(store->UpdateDisplayName(id, "Carol Two"));
    Examples::InfiniteRunner::Leaderboard::SyncedSettings hidden{};
    hidden.show_on_leaderboard = false;
    ASSERT_TRUE(store->UpdateSynced(id, hidden));

    ASSERT_NE(store->Active(), nullptr);
    EXPECT_EQ(store->Active()->display_name, "Carol Two");
    EXPECT_FALSE(store->Active()->synced.show_on_leaderboard);

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

} // namespace
