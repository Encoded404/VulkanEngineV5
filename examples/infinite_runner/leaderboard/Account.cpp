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

std::string NormalizeNameForPolicy(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size();) {
        const auto c = static_cast<unsigned char>(raw[i]);
        // ASCII controls and DEL carry no meaning in a name.
        if (c < 0x20U || c == 0x7FU) {
            ++i;
            continue;
        }
        // Common zero-width / format code points, dropped so "b<U+200B>ad"
        // cannot slip past a substring check.
        if (c == 0xE2U && i + 2 < raw.size() &&
            static_cast<unsigned char>(raw[i + 1]) == 0x80U) {
            const auto third = static_cast<unsigned char>(raw[i + 2]);
            if (third >= 0x8BU && third <= 0x8FU) { // U+200B..U+200F
                i += 3;
                continue;
            }
        }
        if (c == 0xEFU && i + 2 < raw.size() &&
            static_cast<unsigned char>(raw[i + 1]) == 0xBBU &&
            static_cast<unsigned char>(raw[i + 2]) == 0xBFU) { // U+FEFF
            i += 3;
            continue;
        }
        if (c == 0xC2U && i + 1 < raw.size() &&
            static_cast<unsigned char>(raw[i + 1]) == 0xADU) { // U+00AD soft hyphen
            i += 2;
            continue;
        }
        if (std::isspace(c) != 0) {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
        ++i;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    std::size_t begin = 0;
    while (begin < out.size() && out[begin] == ' ') {
        ++begin;
    }
    out.erase(0, begin);
    return out;
}

std::string CompactNameForPolicy(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (const char c : token) {
        const auto value = static_cast<unsigned char>(c);
        if ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')) {
            out.push_back(static_cast<char>(value));
        }
    }
    return out;
}

std::optional<std::string> NamePolicy::Check(std::string_view name) const {
    const std::string normalized = NormalizeNameForPolicy(name);

    std::size_t begin = 0;
    while (begin < normalized.size()) {
        std::size_t end = normalized.find(' ', begin);
        if (end == std::string::npos) {
            end = normalized.size();
        }
        const std::string compact =
            CompactNameForPolicy(std::string_view{normalized}.substr(begin, end - begin));
        begin = end + 1;
        if (compact.empty()) {
            continue;
        }

        // Spans exempted by the allow list.
        std::vector<std::pair<std::size_t, std::size_t>> covered;
        for (const std::string& good : allowed) {
            if (good.empty()) {
                continue;
            }
            for (std::size_t at = compact.find(good); at != std::string::npos;
                 at = compact.find(good, at + 1)) {
                covered.emplace_back(at, at + good.size());
            }
        }

        for (const std::string& bad : blocked) {
            if (bad.empty()) {
                continue;
            }
            for (std::size_t at = compact.find(bad); at != std::string::npos;
                 at = compact.find(bad, at + 1)) {
                const std::size_t block_end = at + bad.size();
                const bool exempt = std::any_of(
                    covered.begin(), covered.end(),
                    [at, block_end](const std::pair<std::size_t, std::size_t>& span) {
                        return span.first <= at && block_end <= span.second;
                    });
                if (!exempt) {
                    return "contains a blocked word";
                }
            }
        }
    }
    return std::nullopt;
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
        case SyncStatus::Rejected: return "rejected";
    }
    return "unknown";
}

} // namespace Examples::InfiniteRunner::Leaderboard
