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
//
// There is no anonymous play and no "hide from the leaderboard" setting: an
// account is always public, and a player who does not want a leaderboard plays
// on a local-only profile instead. `SyncStatus::Rejected` carries a
// human-readable reason for content-policy refusals.
// ─────────────────────────────────────────────────────────────────────────────

using UserId = std::uint64_t;

inline constexpr std::size_t kUsernameMinLength = 3;
inline constexpr std::size_t kUsernameMaxLength = 20;
inline constexpr std::size_t kDisplayNameMinLength = 1;
inline constexpr std::size_t kDisplayNameMaxLength = 32;
inline constexpr std::size_t kTokenHexLength = 64;    // 32 random bytes
inline constexpr std::size_t kSessionHexLength = 64;  // 32 random bytes

// Trim + lowercase. Returns nullopt when the result is not a legal username.
[[nodiscard]] std::optional<std::string> CanonicalizeUsername(std::string_view raw);

// Trim and reject control characters / empty / over-long. Display names are
// free-form UTF-8, so no character class is imposed beyond that. Content policy
// (blocked substrings) is a server-side concern and is applied there.
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
    // Content policy refused the name. A `reason` string accompanies it.
    Rejected = 9,
};

[[nodiscard]] std::string_view ToString(SyncStatus status);

struct AccountInfo {
    UserId id = 0;
    std::string username;
    std::string display_name;
};

// Case-folds, trims, strips common zero-width/format characters and collapses
// whitespace, so "Bad", "bad " and "b<U+200B>ad" normalize to the same text.
// Both the policy file and every candidate name go through this, which is what
// keeps the two consistent.
[[nodiscard]] std::string NormalizeNameForPolicy(std::string_view raw);

// Drops every byte that is not an ASCII letter or digit, so separators cannot
// hide a blocked substring. Policy entries are compacted this way at load, and
// every name token is compacted the same way at check, so the two always meet
// on the same representation.
[[nodiscard]] std::string CompactNameForPolicy(std::string_view token);

// Content policy for usernames and display names.
//
// A name is split on whitespace into tokens. Each token is compacted by
// removing every non-alphanumeric byte, so "b.a.d" and "xXdickXx" compact to
// "bad" and "xxdickxx" and cannot hide a blocked word behind separators.
//
// Within a compact token, `blocked` entries are substrings and `allowed`
// entries are substrings that exempt the span they cover. Exempting only the
// covered span (rather than the whole token) is deliberate: with
// `block dick` and `allow dickin`, "dickinson" and "dickinger" pass, while
// "dickindick" is still rejected because the second "dick" is not covered.
//
// The allow list exists for the "Scunthorpe problem" — a legitimate name that
// happens to contain a blocked substring — and because a single allow prefix
// covers every variant, it never has to enumerate whole names.
struct NamePolicy {
    std::vector<std::string> blocked;         // normalized single tokens, sorted/deduped
    std::unordered_set<std::string> allowed;  // normalized roots

    // nullopt when the name is acceptable, otherwise a bare reason phrase
    // ("contains a blocked word") that the caller prefixes.
    [[nodiscard]] std::optional<std::string> Check(std::string_view name) const;
};

// Settings that stay on this machine. Display filters only change what this
// machine shows, not what the server records.
struct LocalSettings {
    std::size_t top_count = 5;
    bool show_login_modal_on_start = true;
    bool leaderboard_best_per_account = true;
    bool leaderboard_only_mine = false;
    std::uint32_t leaderboard_days = 0; // 0 = all time
};

} // namespace Examples::InfiniteRunner::Leaderboard
