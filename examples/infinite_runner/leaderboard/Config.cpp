module;

module Examples.InfiniteRunner.Leaderboard.Config;

import std;

import VulkanEngine.KeyExchange;
import Examples.InfiniteRunner.Balance;
import Examples.InfiniteRunner.Secrets;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

// Development-only session secret. Used when no sealed PSK is present so the
// example still works out of the box; never rely on it for anything real.
constexpr std::string_view kDevPsk = "infinite-runner-dev-psk-do-not-use-in-production";

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

[[nodiscard]] std::uint8_t HexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    return static_cast<std::uint8_t>(c - 'A' + 10);
}

[[nodiscard]] std::vector<std::byte> PskFromHex(std::string_view text) {
    std::vector<std::byte> out;
    if (text.size() % 2 != 0) {
        return out;
    }
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        out.push_back(static_cast<std::byte>((HexNibble(text[i]) << 4) | HexNibble(text[i + 1])));
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> PskFromText(std::string_view text) {
    std::vector<std::byte> out(32);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i % text.size()]);
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

std::vector<std::byte> LoadSessionPsk() {
    const std::optional<std::string> text = Secrets::GetString(kPskSecret);
    if (text.has_value()) {
        if (const std::optional<std::string> content = FirstContentLine(*text);
            content.has_value()) {
            // Accept a hex key; anything else is treated as raw text so a short
            // local file still produces a deterministic key.
            if (content->size() == 64 &&
                std::all_of(content->begin(), content->end(),
                            [](unsigned char c) { return std::isxdigit(c) != 0; })) {
                return PskFromHex(*content);
            }
            if (!content->empty()) {
                return PskFromText(*content);
            }
        }
    }
    return PskFromText(kDevPsk);
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
    const std::vector<std::byte> bytes = PskFromHex(*content);
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

} // namespace Examples::InfiniteRunner::Leaderboard
