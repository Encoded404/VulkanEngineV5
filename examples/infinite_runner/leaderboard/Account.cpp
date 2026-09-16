module;

module Examples.InfiniteRunner.Leaderboard.Account;

import std;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

[[nodiscard]] std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

[[nodiscard]] bool IsUsernameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] int HexNibble(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

} // namespace

std::optional<std::string> CanonicalizeUsername(std::string_view raw) {
    std::string canonical = Trim(raw);
    for (char& c : canonical) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (canonical.size() < kUsernameMinLength || canonical.size() > kUsernameMaxLength) {
        return std::nullopt;
    }
    if (canonical.front() < 'a' || canonical.front() > 'z') {
        return std::nullopt; // must start with a letter
    }
    for (const char c : canonical) {
        if (!IsUsernameChar(c)) {
            return std::nullopt;
        }
    }
    return canonical;
}

std::optional<std::string> SanitizeDisplayName(std::string_view raw) {
    std::string name = Trim(raw);
    if (name.empty() || name.size() > kDisplayNameMaxLength) {
        return std::nullopt;
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) < 0x20U || c == 0x7F) {
            return std::nullopt;
        }
    }
    return name;
}

std::optional<std::vector<std::byte>> TokenFromHex(std::string_view hex) {
    if (hex.size() != kTokenHexLength && hex.size() != kSessionHexLength) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = HexNibble(hex[i]);
        const int low = HexNibble(hex[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return out;
}

std::string TokenToHex(std::span<const std::byte> token) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(token.size() * 2);
    for (const std::byte b : token) {
        const auto value = std::to_integer<std::uint8_t>(b);
        out.push_back(kDigits[(value >> 4) & 0xF]);
        out.push_back(kDigits[value & 0xF]);
    }
    return out;
}

std::string_view ToString(SyncStatus status) {
    switch (status) {
        case SyncStatus::Ok: return "ok";
        case SyncStatus::UsernameTaken: return "username taken";
        case SyncStatus::InvalidUsername: return "invalid username";
        case SyncStatus::InvalidDisplayName: return "invalid display name";
        case SyncStatus::InvalidToken: return "invalid token";
        case SyncStatus::RateLimited: return "rate limited";
        case SyncStatus::NotAuthenticated: return "not authenticated";
        case SyncStatus::UnknownAccount: return "unknown account";
        case SyncStatus::ServerError: return "server error";
    }
    return "unknown";
}

} // namespace Examples::InfiniteRunner::Leaderboard
