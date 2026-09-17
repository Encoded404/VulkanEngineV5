module;

#include <nlohmann/json.hpp>

module Examples.InfiniteRunner.Account.ProfileStore;

import std;

import VulkanEngine.DataCipher;
import VulkanShared.Storage;
import Examples.InfiniteRunner.Leaderboard.Account;

namespace Examples::InfiniteRunner::Account {

namespace {

[[nodiscard]] std::uint64_t NowSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] std::string NewProfileId() {
    return Leaderboard::TokenToHex(VulkanEngine::Security::RandomBytes(8));
}

template <typename T>
[[nodiscard]] T ValueOr(const nlohmann::json& object, const char* key, T fallback) {
    if (!object.contains(key)) {
        return fallback;
    }
    try {
        return object.at(key).get<T>();
    } catch (const std::exception&) {
        return fallback;
    }
}

} // namespace

std::optional<ProfileStore> ProfileStore::Create(const VulkanShared::Storage::Storage& storage) {
    ProfileStore store{storage};
    store.LoadIndex();
    if (store.active_id_.empty() && !store.profiles_.empty()) {
        store.active_id_ = store.profiles_.front().id;
    } else if (!store.active_id_.empty() &&
               store.Find(store.active_id_) == nullptr) {
        store.active_id_ = store.profiles_.empty() ? std::string{} : store.profiles_.front().id;
    }
    store.LoadActiveData();
    return store;
}

void ProfileStore::LoadIndex() {
    profiles_.clear();
    active_id_.clear();

    const std::expected<std::string, VulkanShared::Storage::Error> config = storage_->ReadConfig();
    if (!config.has_value()) {
        return; // NotFound (or unreadable) means "no profiles yet"
    }
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(*config);
    } catch (const std::exception&) {
        return;
    }
    active_id_ = ValueOr<std::string>(document, "active", {});
    if (!document.contains("profiles") || !document["profiles"].is_array()) {
        return;
    }
    for (const nlohmann::json& entry : document["profiles"]) {
        StoredProfile profile;
        profile.id = ValueOr<std::string>(entry, "id", {});
        profile.username = ValueOr<std::string>(entry, "username", {});
        profile.display_name = ValueOr<std::string>(entry, "display_name", {});
        if (profile.id.empty()) {
            continue;
        }
        profiles_.push_back(std::move(profile));
    }
}

void ProfileStore::LoadActiveData() {
    scores_.clear();
    pending_.clear();
    if (active_id_.empty()) {
        return;
    }
    const std::string slot = "profile-" + active_id_;
    const std::expected<std::vector<std::byte>, VulkanShared::Storage::Error> data =
        storage_->ReadSave(slot);
    if (!data.has_value()) {
        return;
    }
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(
            std::string{reinterpret_cast<const char*>(data->data()), data->size()});
    } catch (const std::exception&) {
        return;
    }

    StoredProfile* profile = nullptr;
    for (StoredProfile& candidate : profiles_) {
        if (candidate.id == active_id_) {
            profile = &candidate;
            break;
        }
    }
    if (profile != nullptr) {
        profile->token = ValueOr<std::string>(document, "token", {});
        profile->local.top_count = ValueOr<std::size_t>(document, "top_count", 5);
        profile->local.show_login_modal_on_start =
            ValueOr<bool>(document, "show_login_modal_on_start", true);
        profile->local.leaderboard_best_per_account =
            ValueOr<bool>(document, "leaderboard_best_per_account", true);
        profile->local.leaderboard_only_mine =
            ValueOr<bool>(document, "leaderboard_only_mine", false);
        profile->local.leaderboard_days =
            ValueOr<std::uint32_t>(document, "leaderboard_days", 0);
    }

    if (document.contains("scores") && document["scores"].is_array()) {
        for (const nlohmann::json& row : document["scores"]) {
            const std::uint64_t hash = ValueOr<std::uint64_t>(row, "config_hash", 0);
            LocalScore score;
            score.score = ValueOr<std::int32_t>(row, "score", 0);
            score.display_name = ValueOr<std::string>(row, "display_name", {});
            score.at = ValueOr<std::uint64_t>(row, "at", 0);
            scores_[hash].push_back(std::move(score));
        }
    }
    if (document.contains("pending") && document["pending"].is_array()) {
        for (const nlohmann::json& row : document["pending"]) {
            PendingSubmission submission;
            submission.config_hash = ValueOr<std::uint64_t>(row, "config_hash", 0);
            submission.run_id = ValueOr<std::uint64_t>(row, "run_id", 0);
            submission.score = ValueOr<std::int32_t>(row, "score", 0);
            pending_.push_back(submission);
        }
    }
}

std::filesystem::path ProfileStore::ProfileSlot(std::string_view id) const {
    return storage_->SavePath("profile-" + std::string{id});
}

nlohmann::json ProfileStore::ProfileDataToJson() const {
    nlohmann::json document;
    document["token"] = "";
    document["top_count"] = 5;
    document["show_login_modal_on_start"] = true;
    document["leaderboard_best_per_account"] = true;
    document["leaderboard_only_mine"] = false;
    document["leaderboard_days"] = 0;

    if (const StoredProfile* profile = Active(); profile != nullptr) {
        document["token"] = profile->token;
        document["top_count"] = profile->local.top_count;
        document["show_login_modal_on_start"] = profile->local.show_login_modal_on_start;
        document["leaderboard_best_per_account"] = profile->local.leaderboard_best_per_account;
        document["leaderboard_only_mine"] = profile->local.leaderboard_only_mine;
        document["leaderboard_days"] = profile->local.leaderboard_days;
    }

    nlohmann::json scores = nlohmann::json::array();
    for (const auto& [hash, rows] : scores_) {
        for (const LocalScore& row : rows) {
            nlohmann::json entry;
            entry["config_hash"] = hash;
            entry["score"] = row.score;
            entry["display_name"] = row.display_name;
            entry["at"] = row.at;
            scores.push_back(std::move(entry));
        }
    }
    document["scores"] = std::move(scores);

    nlohmann::json pending = nlohmann::json::array();
    for (const PendingSubmission& submission : pending_) {
        nlohmann::json entry;
        entry["config_hash"] = submission.config_hash;
        entry["run_id"] = submission.run_id;
        entry["score"] = submission.score;
        pending.push_back(std::move(entry));
    }
    document["pending"] = std::move(pending);
    return document;
}

bool ProfileStore::Save() {
    nlohmann::json index;
    index["active"] = active_id_;
    nlohmann::json profiles = nlohmann::json::array();
    for (const StoredProfile& profile : profiles_) {
        nlohmann::json entry;
        entry["id"] = profile.id;
        entry["username"] = profile.username;
        entry["display_name"] = profile.display_name;
        profiles.push_back(std::move(entry));
    }
    index["profiles"] = std::move(profiles);

    const std::expected<void, VulkanShared::Storage::Error> config_written =
        storage_->WriteConfig(index.dump(2));
    if (!config_written.has_value()) {
        return false;
    }
    if (active_id_.empty()) {
        return true;
    }

    const std::string json = ProfileDataToJson().dump(2);
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(json.data()), json.size()};
    const std::expected<void, VulkanShared::Storage::Error> written =
        storage_->WriteSave("profile-" + active_id_, bytes,
                            VulkanShared::Storage::Visibility::Private);
    return written.has_value();
}

const StoredProfile* ProfileStore::Find(std::string_view id) const noexcept {
    for (const StoredProfile& profile : profiles_) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

const StoredProfile* ProfileStore::Active() const noexcept {
    return Find(active_id_);
}

const StoredProfile& ProfileStore::AddProfile(std::string username, std::string display_name) {
    StoredProfile profile;
    profile.id = NewProfileId();
    profile.username = std::move(username);
    profile.display_name = std::move(display_name);
    profiles_.push_back(std::move(profile));
    active_id_ = profiles_.back().id;
    scores_.clear();
    pending_.clear();
    Save();
    return profiles_.back();
}

bool ProfileStore::RemoveProfile(std::string_view id) {
    // Own the id: callers commonly pass a view into a StoredProfile, which the
    // erase below would invalidate.
    const std::string owned_id{id};
    const auto it = std::find_if(profiles_.begin(), profiles_.end(),
                                 [&owned_id](const StoredProfile& profile) {
                                     return profile.id == owned_id;
                                 });
    if (it == profiles_.end()) {
        return false;
    }
    const bool was_active = it->id == active_id_;
    profiles_.erase(it);
    if (was_active) {
        active_id_ = profiles_.empty() ? std::string{} : profiles_.front().id;
        LoadActiveData();
    }
    // Remove the credential slot too; a removed profile must not leave a
    // readable token behind.
    std::error_code ec;
    std::filesystem::remove(ProfileSlot(owned_id), ec);
    return Save();
}

bool ProfileStore::SetActive(std::string_view id) {
    if (Find(id) == nullptr) {
        return false;
    }
    active_id_ = std::string{id};
    LoadActiveData();
    return true;
}

bool ProfileStore::SetToken(std::string_view id, std::string token) {
    for (StoredProfile& profile : profiles_) {
        if (profile.id == id) {
            profile.token = std::move(token);
            return Save();
        }
    }
    return false;
}

bool ProfileStore::UpdateDisplayName(std::string_view id, std::string display_name) {
    for (StoredProfile& profile : profiles_) {
        if (profile.id == id) {
            profile.display_name = std::move(display_name);
            return Save();
        }
    }
    return false;
}

bool ProfileStore::UpdateLocal(std::string_view id, LocalSettings settings) {
    for (StoredProfile& profile : profiles_) {
        if (profile.id == id) {
            profile.local = settings;
            return Save();
        }
    }
    return false;
}

std::vector<LocalEntry> ProfileStore::LocalTop(std::uint64_t config_hash, std::size_t count) const {
    std::vector<LocalEntry> out;
    const auto it = scores_.find(config_hash);
    if (it == scores_.end()) {
        return out;
    }
    const std::size_t limit = std::min(count, it->second.size());
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        out.push_back(LocalEntry{static_cast<std::int32_t>(i) + 1, it->second[i].score,
                                 it->second[i].display_name, it->second[i].at});
    }
    return out;
}

std::int32_t ProfileStore::LocalBest(std::uint64_t config_hash) const {
    const auto it = scores_.find(config_hash);
    return it == scores_.end() || it->second.empty() ? 0 : it->second.front().score;
}

void ProfileStore::RecordRun(std::uint64_t config_hash, std::uint64_t run_id, std::int32_t score,
                             std::string_view display_name) {
    if (active_id_.empty()) {
        return;
    }
    std::vector<LocalScore>& board = scores_[config_hash];
    board.push_back(LocalScore{score, std::string{display_name}, NowSeconds()});
    std::stable_sort(board.begin(), board.end(), [](const LocalScore& a, const LocalScore& b) {
        return a.score > b.score;
    });
    constexpr std::size_t kMaxLocalScores = 100;
    if (board.size() > kMaxLocalScores) {
        board.resize(kMaxLocalScores);
    }

    pending_.push_back(PendingSubmission{config_hash, run_id, score});
    Save();
}

void ProfileStore::ClearPending() {
    pending_.clear();
    Save();
}

} // namespace Examples::InfiniteRunner::Account
