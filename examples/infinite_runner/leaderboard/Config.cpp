module;

module Examples.InfiniteRunner.Leaderboard.Config;

import std;

import VulkanEngine.KeyExchange;
import Examples.InfiniteRunner.Balance;
import Examples.InfiniteRunner.Leaderboard.Log;
import Examples.InfiniteRunner.Secrets;

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

[[nodiscard]] int HexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] std::vector<std::byte> BytesFromHex(std::string_view text) {
    std::vector<std::byte> out;
    if (text.size() % 2 != 0) {
        return out;
    }
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = HexNibble(text[i]);
        const int low = HexNibble(text[i + 1]);
        if (high < 0 || low < 0) {
            return {};
        }
        out.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return out;
}

// First non-empty, non-comment line, trimmed. The committed *.example templates
// carry comments, and copying one in without deleting them is easy, so every
// sealed text value is read this way instead of parsing the whole blob.
[[nodiscard]] std::optional<std::string> FirstContentLine(std::string_view text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string_view line =
            end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
        const std::string trimmed = Trim(line);
        if (!trimmed.empty() && trimmed[0] != '#') {
            return trimmed;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

} // namespace

std::optional<Endpoint> LoadEndpoint() {
    const std::optional<std::string> text = Secrets::GetString(kEndpointSecret);
    if (!text.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::string> content = FirstContentLine(*text);
    if (!content.has_value()) {
        return std::nullopt;
    }
    const std::string& trimmed = *content;
    const std::size_t colon = trimmed.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= trimmed.size()) {
        return std::nullopt;
    }
    const std::string host = trimmed.substr(0, colon);
    const int port = std::atoi(trimmed.c_str() + colon + 1);
    if (port <= 0 || port > 65535) {
        return std::nullopt;
    }
    return Endpoint{host, static_cast<std::uint16_t>(port)};
}

std::optional<VulkanEngine::Security::X25519Key> LoadServerPublicKey() {
    const std::optional<std::string> text = Secrets::GetString(kServerPublicKeySecret);
    if (!text.has_value()) {
        return std::nullopt;
    }
    const std::optional<std::string> content = FirstContentLine(*text);
    if (!content.has_value() || content->size() != VulkanEngine::Security::kX25519KeyBytes * 2) {
        return std::nullopt;
    }
    const std::vector<std::byte> bytes = BytesFromHex(*content);
    if (bytes.size() != VulkanEngine::Security::kX25519KeyBytes) {
        return std::nullopt;
    }
    VulkanEngine::Security::X25519Key key{};
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return key;
}

std::uint64_t CurrentBalanceHash() {
    return BalanceConfig{}.Hash();
}

std::vector<std::uint64_t> LoadAcceptedConfigs(const std::filesystem::path& path) {
    std::vector<std::uint64_t> configs;
    std::ifstream stream(path);
    if (!stream) {
        return configs;
    }
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        try {
            configs.push_back(std::stoull(trimmed, nullptr, 16));
        } catch (const std::exception&) {
            // A malformed line is skipped; the startup check still guarantees
            // the current hash is listed.
        }
    }
    return configs;
}

NamePolicy LoadNamePolicy(const std::filesystem::path& path) {
    NamePolicy policy;
    std::ifstream stream(path);
    if (!stream) {
        return policy;
    }
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const std::size_t split = trimmed.find_first_of(" \t");
        const std::string directive =
            split == std::string::npos ? trimmed : trimmed.substr(0, split);
        const std::string value =
            split == std::string::npos ? std::string{} : Trim(std::string_view{trimmed}.substr(split));
        if (value.empty()) {
            LogMessage(LogLevel::Warn,
                       std::format("names: '{}' directive without a value in {}", directive,
                                   path.string()));
            continue;
        }
        const std::string normalized = NormalizeNameForPolicy(value);
        if (normalized.empty()) {
            continue;
        }
        if (normalized.find(' ') != std::string::npos) {
            // Matching is per whitespace token, so a phrase can never match.
            LogMessage(LogLevel::Warn,
                       std::format("names: '{} {}' is multi-word and cannot match; use single "
                                   "tokens (skipped)",
                                   directive, normalized));
            continue;
        }
        // Entries meet name tokens in compacted form, so a blocked entry may be
        // written with separators ("f.u.c.k") and still match.
        const std::string entry = CompactNameForPolicy(normalized);
        if (entry.empty()) {
            continue;
        }
        if (directive == "block") {
            policy.blocked.push_back(entry);
        } else if (directive == "allow") {
            policy.allowed.insert(entry);
        } else {
            LogMessage(LogLevel::Warn,
                       std::format("names: unknown directive '{}' in {}", directive, path.string()));
        }
    }
    std::sort(policy.blocked.begin(), policy.blocked.end());
    policy.blocked.erase(std::unique(policy.blocked.begin(), policy.blocked.end()),
                         policy.blocked.end());
    LogMessage(LogLevel::Info,
               std::format("names: loaded {} blocked and {} allowed entries from {}",
                           policy.blocked.size(), policy.allowed.size(), path.string()));
    return policy;
}

} // namespace Examples::InfiniteRunner::Leaderboard
