module;

#include <nlohmann/json.hpp>

export module Examples.InfiniteRunner.Account.ProfileStore;

import std;

import VulkanShared.Storage;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.Store;

export namespace Examples::InfiniteRunner::Account {

using Leaderboard::LocalSettings;
using Leaderboard::ScoreEntry;
using Leaderboard::SyncedSettings;

// A local player profile. The username is permanent; the display name is
// mutable and synced; the token is the account credential and is never written
// anywhere but the private per-profile save slot.
struct StoredProfile {
    std::string id;  // generated once, used as the save slot name
    std::string username;
    std::string display_name;
    std::string token;  // registration token (hex); empty until registered
    SyncedSettings synced{};
    LocalSettings local{};
};

// A score recorded locally but not yet accepted by the server.
struct PendingSubmission {
    std::uint64_t config_hash = 0;
    std::uint64_t run_id = 0;
    std::int32_t score = 0;
};

// Per-user profile persistence on top of VulkanShared::Storage.
//
// The profile index lives in settings.json (no secrets). Each profile's token,
// settings, local bests and pending submissions live in a private save slot
// named after the profile id, so one profile cannot read another's credential
// on a shared machine.
class ProfileStore {
public:
    // Resolves and creates the storage tree. Nullopt when storage is unusable.
    [[nodiscard]] static std::optional<ProfileStore> Create(
        const VulkanShared::Storage::Storage& storage);

    [[nodiscard]] std::span<const StoredProfile> Profiles() const noexcept { return profiles_; }
    [[nodiscard]] const StoredProfile* Find(std::string_view id) const noexcept;
    [[nodiscard]] const StoredProfile* Active() const noexcept;
    [[nodiscard]] std::string_view ActiveId() const noexcept { return active_id_; }

    // Creates a profile with a fresh id and makes it active.
    const StoredProfile& AddProfile(std::string username, std::string display_name);
    bool RemoveProfile(std::string_view id);
    bool SetActive(std::string_view id);

    bool SetToken(std::string_view id, std::string token);
    bool UpdateDisplayName(std::string_view id, std::string display_name);
    bool UpdateSynced(std::string_view id, SyncedSettings settings);
    bool UpdateLocal(std::string_view id, LocalSettings settings);

    // ── local scores (active profile) ──
    [[nodiscard]] std::vector<ScoreEntry> LocalTop(std::uint64_t config_hash, std::size_t count) const;
    [[nodiscard]] std::int32_t LocalBest(std::uint64_t config_hash) const;
    void RecordRun(std::uint64_t config_hash, std::uint64_t run_id, std::int32_t score,
                   std::string_view display_name);

    [[nodiscard]] std::span<const PendingSubmission> Pending() const noexcept { return pending_; }
    void ClearPending();

    // Flushes the index and the active profile. Called after mutations already;
    // exposed for callers that batch changes.
    bool Save();

private:
    struct LocalScore {
        std::int32_t score = 0;
        std::string display_name;
        std::uint64_t at = 0;
    };

    explicit ProfileStore(const VulkanShared::Storage::Storage& storage) : storage_(&storage) {}

    void LoadIndex();
    void LoadActiveData();
    [[nodiscard]] std::filesystem::path ProfileSlot(std::string_view id) const;
    [[nodiscard]] nlohmann::json ProfileDataToJson() const;

    const VulkanShared::Storage::Storage* storage_ = nullptr;
    std::string active_id_;
    std::vector<StoredProfile> profiles_;
    std::map<std::uint64_t, std::vector<LocalScore>> scores_;
    std::vector<PendingSubmission> pending_;
};

} // namespace Examples::InfiniteRunner::Account
