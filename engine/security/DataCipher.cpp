module;

// The envelope format lives in a plain header so the build-time secrets
// generator can compile the exact same seal/open code. Included from the global
// module fragment: Monocypher's C declarations stay in the global module.
#include "engine/security/cipher_format.hpp"

module VulkanEngine.DataCipher;

import std;

namespace VulkanEngine::Security {

namespace {

constexpr std::array<CipherSpec, 3> kCipherSpecs = {{
    {CipherVariant::None, 0, 0, 0, "none"},
    {CipherVariant::XChaCha20Poly1305, kKeyBytes, kMaxNonceBytes, kTagBytes, "xchacha20-poly1305"},
    {CipherVariant::ChaCha20Poly1305Ietf, kKeyBytes, 12, kTagBytes, "chacha20-poly1305-ietf"},
}};

[[nodiscard]] ::vkengine::security::format::CipherVariant ToFormat(CipherVariant variant) noexcept {
    return static_cast<::vkengine::security::format::CipherVariant>(
        static_cast<std::uint16_t>(variant));
}

[[nodiscard]] std::span<const std::uint8_t> AsU8(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::vector<std::byte> ToBytes(std::span<const std::uint8_t> bytes) {
    std::vector<std::byte> out(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    return out;
}

// Nonces need uniqueness, not secrecy (the sealing key ships inside the
// binary), so a random_device-seeded xorshift mixed with a counter and the
// clock is adequate and stays dependency-free.
[[nodiscard]] std::vector<std::byte> RandomBytesImpl(std::size_t count) {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device device;
    std::uint64_t state = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
    state ^= (++counter) * 0x9E3779B97F4A7C15ULL;
    state ^= static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    std::vector<std::byte> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = static_cast<std::byte>(state & 0xFFU);
    }
    return out;
}

} // namespace

std::span<const CipherSpec> AllCipherSpecs() noexcept {
    return kCipherSpecs;
}

const CipherSpec* GetCipherSpec(CipherVariant variant) noexcept {
    for (const CipherSpec& spec : kCipherSpecs) {
        if (spec.variant == variant) {
            return &spec;
        }
    }
    return nullptr;
}

std::optional<std::size_t> NonceBytes(CipherVariant variant) noexcept {
    if (const CipherSpec* spec = GetCipherSpec(variant); spec != nullptr) {
        return spec->nonce_bytes;
    }
    return std::nullopt;
}

KeyRing::~KeyRing() {
    for (auto& [id, key] : keys_) {
        if (!key.empty()) {
            crypto_wipe(key.data(), key.size());
        }
        (void)id;
    }
}

void KeyRing::Add(std::uint8_t key_id, std::span<const std::byte> key) {
    for (auto& [existing_id, existing_key] : keys_) {
        if (existing_id == key_id) {
            existing_key.assign(key.begin(), key.end());
            return;
        }
    }
    keys_.emplace_back(key_id, std::vector<std::byte>(key.begin(), key.end()));
}

bool KeyRing::Contains(std::uint8_t key_id) const noexcept {
    for (const auto& [id, key] : keys_) {
        if (id == key_id) {
            return true;
        }
    }
    return false;
}

std::span<const std::byte> KeyRing::Get(std::uint8_t key_id) const noexcept {
    for (const auto& [id, key] : keys_) {
        if (id == key_id) {
            return key;
        }
    }
    return {};
}

bool KeyRing::Empty() const noexcept {
    return keys_.empty();
}

std::size_t KeyRing::Size() const noexcept {
    return keys_.size();
}

std::vector<std::byte> Seal(const KeyRing& keys,
                            CipherVariant variant,
                            std::uint8_t key_id,
                            std::span<const std::byte> nonce,
                            std::span<const std::byte> plaintext,
                            std::span<const std::byte> aad) {
    const std::span<const std::byte> key = keys.Get(key_id);
    if (key.empty()) {
        return {};
    }
    return ToBytes(::vkengine::security::format::Seal(
        ToFormat(variant), key_id, AsU8(key), AsU8(nonce), AsU8(plaintext), AsU8(aad)));
}

std::vector<std::byte> SealRandom(const KeyRing& keys,
                                  CipherVariant variant,
                                  std::uint8_t key_id,
                                  std::span<const std::byte> plaintext,
                                  std::span<const std::byte> aad) {
    const std::optional<std::size_t> nonce_bytes = NonceBytes(variant);
    if (!nonce_bytes.has_value() || *nonce_bytes == 0) {
        return {};
    }
    const std::vector<std::byte> nonce = RandomBytesImpl(*nonce_bytes);
    return Seal(keys, variant, key_id, nonce, plaintext, aad);
}

std::optional<std::vector<std::byte>> Open(const KeyRing& keys,
                                           std::span<const std::byte> blob,
                                           std::span<const std::byte> aad) {
    const std::optional<std::uint8_t> key_id =
        ::vkengine::security::format::PeekKeyId(AsU8(blob));
    if (!key_id.has_value()) {
        return std::nullopt;
    }
    const std::span<const std::byte> key = keys.Get(*key_id);
    if (key.empty()) {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> plain =
        ::vkengine::security::format::Open(AsU8(blob), AsU8(key), AsU8(aad));
    if (!plain.has_value()) {
        return std::nullopt;
    }
    std::vector<std::byte> out = ToBytes(*plain);
    if (!plain->empty()) {
        crypto_wipe(plain->data(), plain->size());
    }
    return out;
}

std::optional<std::uint8_t> PeekKeyId(std::span<const std::byte> blob) noexcept {
    return ::vkengine::security::format::PeekKeyId(AsU8(blob));
}

std::optional<CipherVariant> PeekVariant(std::span<const std::byte> blob) noexcept {
    const auto variant = ::vkengine::security::format::PeekVariant(AsU8(blob));
    if (!variant.has_value()) {
        return std::nullopt;
    }
    return static_cast<CipherVariant>(static_cast<std::uint16_t>(*variant));
}

std::vector<std::byte> RandomNonce(CipherVariant variant) {
    const std::optional<std::size_t> nonce_bytes = NonceBytes(variant);
    if (!nonce_bytes.has_value() || *nonce_bytes == 0) {
        return {};
    }
    return RandomBytesImpl(*nonce_bytes);
}

std::vector<std::byte> RandomBytes(std::size_t count) {
    return RandomBytesImpl(count);
}

std::vector<std::byte> DeriveKey(std::span<const std::byte> ikm,
                                 std::span<const std::byte> salt,
                                 std::string_view context,
                                 std::size_t out_bytes) {
    if (out_bytes == 0 || out_bytes > 64) {
        return {};
    }
    std::vector<std::uint8_t> input;
    input.reserve(ikm.size() + salt.size() + context.size());
    for (const std::byte b : ikm) {
        input.push_back(std::to_integer<std::uint8_t>(b));
    }
    for (const std::byte b : salt) {
        input.push_back(std::to_integer<std::uint8_t>(b));
    }
    for (const char c : context) {
        input.push_back(static_cast<std::uint8_t>(c));
    }

    std::vector<std::uint8_t> digest(out_bytes);
    crypto_blake2b(digest.data(), digest.size(), input.data(), input.size());
    if (!input.empty()) {
        crypto_wipe(input.data(), input.size());
    }

    std::vector<std::byte> out(out_bytes);
    std::memcpy(out.data(), digest.data(), out_bytes);
    crypto_wipe(digest.data(), digest.size());
    return out;
}

} // namespace VulkanEngine::Security
