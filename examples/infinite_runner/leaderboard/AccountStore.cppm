module;

export module Examples.InfiniteRunner.Leaderboard.AccountStore;

import std;

import VulkanEngine.PasswordHash;
export import Examples.InfiniteRunner.Leaderboard.Account;

export namespace Examples::InfiniteRunner::Leaderboard {

// Wall-clock seconds since the Unix epoch. Session and account timestamps are
// in this unit on the wire, so the server and client agree without a clock
// synchronisation step (only ordering matters).
[[nodiscard]] std::uint64_t UnixNowSeconds();

struct AccountStoreOptions {
    // Empty means in-memory only (tests, ephemeral servers).
    std::filesystem::path path{};
    // Server-side credential work factor. Tests inject tiny parameters.
    VulkanEngine::Security::Argon2Params argon2{};
    std::size_t max_run_ids_per_account = 256;
    // Server policy for usernames and display names: blocked substrings plus an
    // allow list of roots that exempt the span they cover.
    NamePolicy name_policy{};
};

// Persistent account records. Stores only Argon2id verifiers, never the raw
// registration token, so a leaked accounts file cannot be replayed.
//
// `hidden` is a legacy field kept only so a protocol-v2 client that still
// carries the old "show on leaderboard" setting keeps working: the server
// accepts its submissions but does not record them. Protocol-v3 accounts are
// always public and ignore it.
class AccountStore {
public:
    struct RegisterResult {
        SyncStatus status = SyncStatus::ServerError;
        std::string reason; // populated for refusals (Rejected / invalid name)
        UserId user_id = 0;
        std::string token; // hex, present only when status == Ok
        std::string display_name;
    };

    struct LoginResult {
        SyncStatus status = SyncStatus::ServerError;
        std::string reason;
        AccountInfo account{};
    };

    struct UpdateResult {
        SyncStatus status = SyncStatus::ServerError;
        std::string reason;
        AccountInfo account{};
    };

    enum class ScoreResult {
        Accepted,
        Duplicate,      // the run_id was already recorded for this account
        UnknownAccount,
    };

    explicit AccountStore(AccountStoreOptions options = {});
    ~AccountStore();

    AccountStore(const AccountStore&) = delete;
    AccountStore& operator=(const AccountStore&) = delete;

    // Canonicalizes the username, rejects duplicates, mints a registration token
    // and stores its verifier. Offline-facing: returns a token the client keeps.
    [[nodiscard]] RegisterResult Register(std::string_view username, std::string_view display_name);

    [[nodiscard]] LoginResult Login(std::string_view username, std::string_view token_hex);

    // Renames the account (display name only). Does not touch the legacy
    // visibility bit, so a rename can never reveal a hidden v2 account.
    [[nodiscard]] UpdateResult Rename(UserId id, std::string_view display_name);

    // Legacy protocol-v2 settings update: display name plus the old
    // show_on_leaderboard flag, stored inverted as `hidden`.
    [[nodiscard]] UpdateResult UpdateLegacy(UserId id, std::string_view display_name,
                                            bool show_on_leaderboard);

    // Idempotent score recording: a retried run_id is accepted once.
    [[nodiscard]] ScoreResult RecordRun(UserId id, std::uint64_t run_id);

    [[nodiscard]] std::optional<AccountInfo> FindById(UserId id) const;
    // Name to show on a public board: nullopt when the account is unknown, the
    // empty string when the legacy hidden bit is set, otherwise the name.
    [[nodiscard]] std::optional<std::string> ShownName(UserId id) const;
    [[nodiscard]] bool IsHidden(UserId id) const;
    [[nodiscard]] std::size_t AccountCount() const;

private:
    struct Record {
        UserId id = 0;
        std::string username;    // canonical
        std::string display_name;
        std::string verifier;    // Argon2id encoded string
        bool hidden = false;     // legacy v2 "do not record my scores"
        std::uint64_t created_at = 0;
        std::uint64_t last_seen = 0;
        std::vector<std::uint64_t> run_ids;
    };

    void Load();
    [[nodiscard]] bool Save() const;
    [[nodiscard]] Record* FindByUsername(std::string_view canonical);
    [[nodiscard]] const Record* FindByUsername(std::string_view canonical) const;
    [[nodiscard]] Record* FindByIdInternal(UserId id);
    // Structural validation + server policy. `reason` is set on failure.
    [[nodiscard]] std::optional<std::string> ValidateDisplayName(std::string_view raw,
                                                                std::string& reason) const;

    AccountStoreOptions options_;
    mutable std::mutex mutex_;
    std::vector<Record> accounts_;
    UserId next_id_ = 1;
};

struct SessionStoreOptions {
    std::chrono::seconds ttl{std::chrono::hours(1)};
    // Reject a session token once it has fewer than this many seconds left when
    // it is presented, so a request never starts on an almost-dead session.
    std::chrono::seconds minimum_remaining{std::chrono::seconds(30)};
};

// In-memory session tokens. A session authorises score submission without
// re-sending the account credential; expiry slides forward on use.
class SessionStore {
public:
    struct Issued {
        std::string token; // hex
        std::uint64_t expires_at = 0;
    };

    explicit SessionStore(SessionStoreOptions options = {});

    [[nodiscard]] Issued Issue(UserId user_id);
    // Returns the account id when the token is live, and refreshes its expiry.
    [[nodiscard]] std::optional<UserId> Resolve(std::string_view token_hex);
    void Revoke(std::string_view token_hex);
    void Sweep();
    [[nodiscard]] std::size_t Count() const;

private:
    struct Session {
        std::string token;
        UserId user_id = 0;
        std::uint64_t expires_at = 0;
    };

    SessionStoreOptions options_;
    mutable std::mutex mutex_;
    std::vector<Session> sessions_;
};

} // namespace Examples::InfiniteRunner::Leaderboard
