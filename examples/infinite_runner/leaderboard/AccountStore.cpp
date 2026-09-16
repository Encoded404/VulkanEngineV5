module;

#include <nlohmann/json.hpp>

module Examples.InfiniteRunner.Leaderboard.AccountStore;

import std;

import VulkanEngine.DataCipher;
import VulkanEngine.PasswordHash;
import Examples.InfiniteRunner.Leaderboard.Log;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

[[nodiscard]] std::string RandomTokenHex() {
    const std::vector<std::byte> token = VulkanEngine::Security::RandomBytes(32);
    return TokenToHex(token);
}

[[nodiscard]] std::optional<std::vector<std::byte>> TokenBytes(std::string_view hex) {
    return TokenFromHex(hex);
}

} // namespace

std::uint64_t UnixNowSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

AccountStore::AccountStore(AccountStoreOptions options) : options_(std::move(options)) {
    if (!options_.path.empty()) {
        Load();
        LogMessage(LogLevel::Info,
                   std::format("accounts: loaded {} from {}", accounts_.size(),
                               options_.path.string()));
    }
}

AccountStore::~AccountStore() = default;

void AccountStore::Load() {
    std::ifstream stream(options_.path);
    if (!stream) {
        return;
    }
    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception&) {
        return; // a corrupt file is treated as "no accounts" rather than fatal
    }
    if (!document.is_array()) {
        return;
    }

    std::lock_guard lock(mutex_);
    for (const nlohmann::json& entry : document) {
        Record record{};
        record.id = entry.value("id", 0ULL);
        record.username = entry.value("username", std::string{});
        record.display_name = entry.value("display_name", std::string{});
        record.verifier = entry.value("verifier", std::string{});
        record.settings.show_on_leaderboard = entry.value("show_on_leaderboard", true);
        record.created_at = entry.value("created_at", 0ULL);
        record.last_seen = entry.value("last_seen", 0ULL);
        if (entry.contains("run_ids") && entry["run_ids"].is_array()) {
            for (const nlohmann::json& run : entry["run_ids"]) {
                if (run.is_number_unsigned()) {
                    record.run_ids.push_back(run.get<std::uint64_t>());
                }
            }
        }
        if (record.id == 0 || record.username.empty()) {
            continue;
        }
        next_id_ = std::max(next_id_, record.id + 1);
        accounts_.push_back(std::move(record));
    }
}

bool AccountStore::Save() const {
    if (options_.path.empty()) {
        return true;
    }
    // Callers already hold mutex_; this only serialises the file it writes.
    nlohmann::json document = nlohmann::json::array();
    for (const Record& record : accounts_) {
        nlohmann::json entry;
        entry["id"] = record.id;
        entry["username"] = record.username;
        entry["display_name"] = record.display_name;
        entry["verifier"] = record.verifier;
        entry["show_on_leaderboard"] = record.settings.show_on_leaderboard;
        entry["created_at"] = record.created_at;
        entry["last_seen"] = record.last_seen;
        entry["run_ids"] = record.run_ids;
        document.push_back(std::move(entry));
    }

    std::error_code ec;
    if (!options_.path.parent_path().empty()) {
        std::filesystem::create_directories(options_.path.parent_path(), ec);
    }
    std::filesystem::path staging = options_.path;
    staging += ".tmp";
    {
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << document.dump(2);
        out.close();
        if (out.fail()) {
            std::filesystem::remove(staging, ec);
            return false;
        }
    }
    std::filesystem::rename(staging, options_.path, ec);
    if (ec) {
        std::filesystem::remove(staging, ec);
        return false;
    }
    return true;
}

AccountStore::Record* AccountStore::FindByUsername(std::string_view canonical) {
    for (Record& record : accounts_) {
        if (record.username == canonical) {
            return &record;
        }
    }
    return nullptr;
}

const AccountStore::Record* AccountStore::FindByUsername(std::string_view canonical) const {
    for (const Record& record : accounts_) {
        if (record.username == canonical) {
            return &record;
        }
    }
    return nullptr;
}

AccountStore::Record* AccountStore::FindByIdInternal(UserId id) {
    for (Record& record : accounts_) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

AccountStore::RegisterResult AccountStore::Register(std::string_view username,
                                                    std::string_view display_name) {
    // Every early return is just a status, so build the result in one place
    // rather than partially initialising it with designated initializers.
    const auto failure = [](SyncStatus status) {
        RegisterResult result;
        result.status = status;
        return result;
    };

    const std::optional<std::string> canonical = CanonicalizeUsername(username);
    if (!canonical.has_value()) {
        return failure(SyncStatus::InvalidUsername);
    }
    const std::optional<std::string> clean_name = SanitizeDisplayName(display_name);
    if (!clean_name.has_value()) {
        return failure(SyncStatus::InvalidDisplayName);
    }

    const std::string token = RandomTokenHex();
    const std::optional<std::vector<std::byte>> token_bytes = TokenBytes(token);
    if (!token_bytes.has_value()) {
        return failure(SyncStatus::ServerError);
    }
    const std::string verifier =
        VulkanEngine::Security::HashPassword(*token_bytes, options_.argon2);
    if (verifier.empty()) {
        return failure(SyncStatus::ServerError);
    }

    std::lock_guard lock(mutex_);
    if (FindByUsername(*canonical) != nullptr) {
        return failure(SyncStatus::UsernameTaken);
    }

    Record record{};
    record.id = next_id_++;
    record.username = *canonical;
    record.display_name = *clean_name;
    record.verifier = verifier;
    record.created_at = UnixNowSeconds();
    record.last_seen = record.created_at;
    accounts_.push_back(std::move(record));
    LogMessage(LogLevel::Info,
               std::format("accounts: registered '{}' as id {}", accounts_.back().username,
                           accounts_.back().id));

    (void)Save();
    return RegisterResult{.status = SyncStatus::Ok,
                          .user_id = accounts_.back().id,
                          .token = token,
                          .display_name = accounts_.back().display_name};
}

AccountStore::LoginResult AccountStore::Login(std::string_view username,
                                              std::string_view token_hex) {
    const std::optional<std::string> canonical = CanonicalizeUsername(username);
    const std::optional<std::vector<std::byte>> token = TokenBytes(token_hex);
    if (!canonical.has_value() || !token.has_value()) {
        return LoginResult{.status = SyncStatus::InvalidToken};
    }

    std::lock_guard lock(mutex_);
    Record* record = FindByUsername(*canonical);
    if (record == nullptr) {
        return LoginResult{.status = SyncStatus::UnknownAccount};
    }
    if (!VulkanEngine::Security::VerifyPassword(*token, record->verifier)) {
        return LoginResult{.status = SyncStatus::InvalidToken};
    }

    record->last_seen = UnixNowSeconds();
    (void)Save();

    LoginResult result{};
    result.status = SyncStatus::Ok;
    result.account = AccountInfo{record->id, record->username, record->display_name};
    result.settings = record->settings;
    return result;
}

AccountStore::UpdateResult AccountStore::UpdateSettings(UserId id, std::string_view display_name,
                                                        const SyncedSettings& settings) {
    const std::optional<std::string> clean_name = SanitizeDisplayName(display_name);
    if (!clean_name.has_value()) {
        return UpdateResult{.status = SyncStatus::InvalidDisplayName};
    }

    std::lock_guard lock(mutex_);
    Record* record = FindByIdInternal(id);
    if (record == nullptr) {
        return UpdateResult{.status = SyncStatus::UnknownAccount};
    }
    record->display_name = *clean_name;
    record->settings = settings;
    (void)Save();

    UpdateResult result{};
    result.status = SyncStatus::Ok;
    result.account = AccountInfo{record->id, record->username, record->display_name};
    result.settings = record->settings;
    return result;
}

AccountStore::ScoreResult AccountStore::RecordRun(UserId id, std::uint64_t run_id) {
    std::lock_guard lock(mutex_);
    Record* record = FindByIdInternal(id);
    if (record == nullptr) {
        return ScoreResult::UnknownAccount;
    }
    if (std::find(record->run_ids.begin(), record->run_ids.end(), run_id) != record->run_ids.end()) {
        return ScoreResult::Duplicate;
    }
    record->run_ids.push_back(run_id);
    if (record->run_ids.size() > options_.max_run_ids_per_account) {
        record->run_ids.erase(record->run_ids.begin());
    }
    record->last_seen = UnixNowSeconds();
    (void)Save();
    return ScoreResult::Accepted;
}

std::optional<SyncedSettings> AccountStore::SettingsOf(UserId id) const {
    std::lock_guard lock(mutex_);
    for (const Record& record : accounts_) {
        if (record.id == id) {
            return record.settings;
        }
    }
    return std::nullopt;
}

std::optional<AccountInfo> AccountStore::FindById(UserId id) const {
    std::lock_guard lock(mutex_);
    for (const Record& record : accounts_) {
        if (record.id == id) {
            return AccountInfo{record.id, record.username, record.display_name};
        }
    }
    return std::nullopt;
}

std::size_t AccountStore::AccountCount() const {
    std::lock_guard lock(mutex_);
    return accounts_.size();
}

SessionStore::SessionStore(SessionStoreOptions options) : options_(options) {}

SessionStore::Issued SessionStore::Issue(UserId user_id) {
    const std::string token = RandomTokenHex();
    const std::uint64_t expires_at =
        UnixNowSeconds() + static_cast<std::uint64_t>(options_.ttl.count());

    std::lock_guard lock(mutex_);
    // Opportunistically drop dead sessions so the table cannot grow unbounded
    // between explicit sweeps.
    const std::uint64_t now = UnixNowSeconds();
    std::erase_if(sessions_, [now](const Session& session) { return session.expires_at <= now; });

    sessions_.push_back(Session{token, user_id, expires_at});
    return Issued{token, expires_at};
}

std::optional<UserId> SessionStore::Resolve(std::string_view token_hex) {
    const std::uint64_t now = UnixNowSeconds();
    std::lock_guard lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (it->token != token_hex) {
            continue;
        }
        const std::uint64_t minimum_remaining =
            static_cast<std::uint64_t>(options_.minimum_remaining.count());
        if (it->expires_at <= now + minimum_remaining) {
            sessions_.erase(it);
            return std::nullopt;
        }
        it->expires_at = now + static_cast<std::uint64_t>(options_.ttl.count());
        return it->user_id;
    }
    return std::nullopt;
}

void SessionStore::Revoke(std::string_view token_hex) {
    std::lock_guard lock(mutex_);
    std::erase_if(sessions_, [token_hex](const Session& session) {
        return session.token == token_hex;
    });
}

void SessionStore::Sweep() {
    const std::uint64_t now = UnixNowSeconds();
    std::lock_guard lock(mutex_);
    std::erase_if(sessions_, [now](const Session& session) {
        return session.expires_at <= now;
    });
}

std::size_t SessionStore::Count() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

} // namespace Examples::InfiniteRunner::Leaderboard
