module;

#include "monocypher.h"

module VulkanEngine.PasswordHash;

import std;

import VulkanEngine.DataCipher;

namespace VulkanEngine::Security {

namespace {

constexpr std::string_view kPrefix = "argon2id";
constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] std::string ToHex(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte b : bytes) {
        const auto value = std::to_integer<std::uint8_t>(b);
        out.push_back(kHexDigits[(value >> 4) & 0xF]);
        out.push_back(kHexDigits[value & 0xF]);
    }
    return out;
}

[[nodiscard]] std::optional<std::vector<std::byte>> FromHex(std::string_view text) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    };
    if (text.empty() || text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return out;
}

[[nodiscard]] bool ParseU32(std::string_view text, std::uint32_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint32_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find(separator, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

[[nodiscard]] bool Useable(const Argon2Params& params) {
    return params.lanes > 0 && params.lanes <= Argon2Params::kMaxLanes &&
           params.blocks >= 8u * params.lanes && params.blocks <= Argon2Params::kMaxBlocks &&
           params.passes >= 1 && params.passes <= Argon2Params::kMaxPasses &&
           params.hash_bytes > 0 && params.hash_bytes <= 64 &&
           params.salt_bytes >= 8 && params.salt_bytes <= 64;
}

// Argon2 needs a caller-provided work area of nb_blocks * 1024 bytes. It is a
// vector of u64 so the buffer is 8-byte aligned regardless of the allocator.
void RunArgon2(std::span<const std::byte> password,
               std::span<const std::byte> salt,
               const Argon2Params& params,
               std::span<std::byte> out_hash) {
    std::vector<std::uint64_t> work(static_cast<std::size_t>(params.blocks) * 128U);
    crypto_argon2_config config{};
    config.algorithm = CRYPTO_ARGON2_ID;
    config.nb_blocks = params.blocks;
    config.nb_passes = params.passes;
    config.nb_lanes = params.lanes;

    crypto_argon2_inputs inputs{};
    inputs.pass = reinterpret_cast<const std::uint8_t*>(password.data());
    inputs.salt = reinterpret_cast<const std::uint8_t*>(salt.data());
    inputs.pass_size = static_cast<std::uint32_t>(password.size());
    inputs.salt_size = static_cast<std::uint32_t>(salt.size());

    crypto_argon2(reinterpret_cast<std::uint8_t*>(out_hash.data()),
                  static_cast<std::uint32_t>(out_hash.size()),
                  work.data(), config, inputs, crypto_argon2_no_extras);
}

} // namespace

bool ConstantTimeEquals(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(a[i]) ^
                                                std::to_integer<std::uint8_t>(b[i]));
    }
    return difference == 0;
}

std::string HashPassword(std::span<const std::byte> password, const Argon2Params& params) {
    if (!Useable(params)) {
        return {};
    }
    const std::vector<std::byte> salt = RandomBytes(params.salt_bytes);
    if (salt.size() != params.salt_bytes) {
        return {};
    }

    std::vector<std::byte> hash(params.hash_bytes);
    RunArgon2(password, salt, params, hash);

    std::string out{kPrefix};
    out += '$';
    out += std::to_string(params.blocks);
    out += '$';
    out += std::to_string(params.passes);
    out += '$';
    out += std::to_string(params.lanes);
    out += '$';
    out += ToHex(salt);
    out += '$';
    out += ToHex(hash);
    return out;
}

bool VerifyPassword(std::span<const std::byte> password, std::string_view stored) {
    const std::vector<std::string_view> parts = Split(stored, '$');
    if (parts.size() != 6 || parts[0] != kPrefix) {
        return false;
    }

    Argon2Params params{};
    if (!ParseU32(parts[1], params.blocks) || !ParseU32(parts[2], params.passes) ||
        !ParseU32(parts[3], params.lanes)) {
        return false;
    }
    const std::optional<std::vector<std::byte>> salt = FromHex(parts[4]);
    const std::optional<std::vector<std::byte>> expected = FromHex(parts[5]);
    if (!salt.has_value() || !expected.has_value()) {
        return false;
    }
    params.salt_bytes = static_cast<std::uint32_t>(salt->size());
    params.hash_bytes = static_cast<std::uint32_t>(expected->size());
    if (!Useable(params)) {
        return false;
    }

    std::vector<std::byte> actual(params.hash_bytes);
    RunArgon2(password, *salt, params, actual);
    return ConstantTimeEquals(actual, *expected);
}

} // namespace VulkanEngine::Security
