module;

export module Examples.InfiniteRunner.Leaderboard.Account;

import std;

export namespace Examples::InfiniteRunner::Leaderboard {

// ─────────────────────────────────────────────────────────────────────────────
// Account identity and player settings.
//
// A username is permanent and is the account's public identity; a display name
// is mutable and is what the leaderboard shows. The canonical username is the
// uniqueness key (trimmed, lowercased, restricted to a small character set) so
// "Alice" and "alice " cannot both exist.
// ─────────────────────────────────────────────────────────────────────────────

using UserId = std::uint64_t;

inline constexpr std::size_t kUsernameMinLength = 3;
inline constexpr std::size_t kUsernameMaxLength = 20;
inline constexpr std::size_t kDisplayNameMaxLength = 32;
inline constexpr std::size_t kTokenHexLength = 64;    // 32 random bytes
inline constexpr std::size_t kSessionHexLength = 64;  // 32 random bytes

// Trim + lowercase. Returns nullopt when the result is not a legal username.
[[nodiscard]] std::optional<std::string> CanonicalizeUsername(std::string_view raw);

// Trim and reject control characters / empty / over-long. Display names are
// free-form UTF-8, so no character class is imposed beyond that.
[[nodiscard]] std::optional<std::string> SanitizeDisplayName(std::string_view raw);

// Parses exactly kTokenHexLength / kSessionHexLength lowercase-or-uppercase hex
// characters into bytes. Nullopt on any other length or a non-hex digit.
[[nodiscard]] std::optional<std::vector<std::byte>> TokenFromHex(std::string_view hex);
[[nodiscard]] std::string TokenToHex(std::span<const std::byte> token);

// Result of an account operation. Values are part of the wire format and are
// never renumbered.
enum class SyncStatus : std::uint8_t {
    Ok = 0,
    UsernameTaken = 1,
    InvalidUsername = 2,
    InvalidDisplayName = 3,
    InvalidToken = 4,
    RateLimited = 5,
    NotAuthenticated = 6,
    UnknownAccount = 7,
    ServerError = 8,
};

[[nodiscard]] std::string_view ToString(SyncStatus status);

struct AccountInfo {
    UserId id = 0;
    std::string username;
    std::string display_name;
};

// The subset of settings that travels to the server and is visible to others.
struct SyncedSettings {
    bool show_on_leaderboard = true;
};

// Settings that stay on this machine. Kept next to the synced ones so a profile
// has one settings object with a clearly marked public part.
struct LocalSettings {
    std::size_t top_count = 5;
    bool show_login_modal_on_start = true;
    // Leaderboard display filters. Kept local because they only change what
    // this machine shows, not what the server records.
    bool leaderboard_best_per_account = false;
    std::uint32_t leaderboard_days = 0; // 0 = all time
};

struct ProfileSettings {
    SyncedSettings synced{};
    LocalSettings local{};
};

} // namespace Examples::InfiniteRunner::Leaderboard
