#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Sealed-blob envelope format, shared by the runtime DataCipher module and the
// build-time secrets generator.
//
// This header is deliberately dependency-free apart from Monocypher: the host
// generator compiles it standalone, and the runtime includes it from the global
// module fragment of DataCipher.cpp. The format is the contract between the two,
// so it lives in exactly one place.
//
// Envelope layout (all integers little-endian):
//   [0..1]  format_version  u16
//   [2..3]  cipher_variant  u16
//   [4]     key_id          u8
//   [5]     reserved        u8 (0)
//   [6..]   nonce           spec.nonce_bytes
//   [...]   ciphertext      plaintext.size()
//   [...]   tag             spec.tag_bytes
//
// The header is cleartext on purpose: a reader must be able to select the
// cipher and key before it can decrypt anything, which is what keeps old blobs
// readable after the format or cipher variants grow.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "monocypher.h"

namespace vkengine::security::format {

inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::size_t kHeaderFixedBytes = 6;
inline constexpr std::size_t kMacBytes = 16;
inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kMaxNonceBytes = 24;

enum class CipherVariant : std::uint16_t {
    None = 0,
    XChaCha20Poly1305 = 1,   // 24-byte nonce
    ChaCha20Poly1305Ietf = 2, // 12-byte nonce
};

struct CipherSpec {
    CipherVariant variant;
    std::size_t key_bytes;
    std::size_t nonce_bytes;
    std::size_t tag_bytes;
    const char* name;
};

inline constexpr CipherSpec kSpecs[] = {
    {CipherVariant::None, 0, 0, 0, "none"},
    {CipherVariant::XChaCha20Poly1305, kKeyBytes, kMaxNonceBytes, kMacBytes, "xchacha20-poly1305"},
    {CipherVariant::ChaCha20Poly1305Ietf, kKeyBytes, 12, kMacBytes, "chacha20-poly1305-ietf"},
};

[[nodiscard]] inline const CipherSpec* GetSpec(CipherVariant variant) noexcept {
    for (const CipherSpec& spec : kSpecs) {
        if (spec.variant == variant) {
            return &spec;
        }
    }
    return nullptr;
}

inline void WriteU16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

[[nodiscard]] inline std::uint16_t ReadU16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>(in[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8);
}

// Seal `plaintext` into a self-describing envelope. The caller owns nonce
// selection so that deterministic (reproducible) or random sealing are both
// possible. Returns empty on a malformed request.
[[nodiscard]] inline std::vector<std::uint8_t> Seal(
    CipherVariant variant,
    std::uint8_t key_id,
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> nonce,
    std::span<const std::uint8_t> plaintext,
    std::span<const std::uint8_t> aad) {
    const CipherSpec* spec = GetSpec(variant);
    if (spec == nullptr || variant == CipherVariant::None) {
        return {};
    }
    if (key.size() != spec->key_bytes || nonce.size() != spec->nonce_bytes) {
        return {};
    }

    std::vector<std::uint8_t> out(kHeaderFixedBytes + spec->nonce_bytes +
                                  plaintext.size() + spec->tag_bytes);
    WriteU16(out.data(), kFormatVersion);
    WriteU16(out.data() + 2, static_cast<std::uint16_t>(variant));
    out[4] = key_id;
    out[5] = 0;
    std::memcpy(out.data() + kHeaderFixedBytes, nonce.data(), nonce.size());

    std::uint8_t* cipher = out.data() + kHeaderFixedBytes + spec->nonce_bytes;
    std::uint8_t* tag = cipher + plaintext.size();

    if (variant == CipherVariant::XChaCha20Poly1305) {
        crypto_aead_lock(cipher, tag, key.data(), nonce.data(),
                         aad.data(), aad.size(), plaintext.data(), plaintext.size());
    } else {
        crypto_aead_ctx ctx;
        crypto_aead_init_ietf(&ctx, key.data(), nonce.data());
        crypto_aead_write(&ctx, cipher, tag, aad.data(), aad.size(),
                          plaintext.data(), plaintext.size());
        crypto_wipe(&ctx, sizeof(ctx));
    }
    return out;
}

// Open an envelope. `key` must match the key_id recorded in the header; the
// caller selects it (see PeekKeyId). Returns nullopt for a malformed envelope,
// an unknown variant, or a failed authentication tag.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> Open(
    std::span<const std::uint8_t> blob,
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> aad) {
    if (blob.size() < kHeaderFixedBytes) {
        return std::nullopt;
    }
    if (ReadU16(blob.data()) != kFormatVersion) {
        return std::nullopt;
    }

    const auto variant = static_cast<CipherVariant>(ReadU16(blob.data() + 2));
    const CipherSpec* spec = GetSpec(variant);
    if (spec == nullptr || variant == CipherVariant::None) {
        return std::nullopt;
    }
    if (key.size() != spec->key_bytes) {
        return std::nullopt;
    }

    const std::size_t prefix = kHeaderFixedBytes + spec->nonce_bytes;
    if (blob.size() < prefix + spec->tag_bytes) {
        return std::nullopt;
    }

    const std::uint8_t* nonce = blob.data() + kHeaderFixedBytes;
    const std::size_t cipher_len = blob.size() - prefix - spec->tag_bytes;
    const std::uint8_t* cipher = blob.data() + prefix;
    const std::uint8_t* tag = cipher + cipher_len;

    std::vector<std::uint8_t> plain(cipher_len);
    int mismatch = -1;
    if (variant == CipherVariant::XChaCha20Poly1305) {
        mismatch = crypto_aead_unlock(plain.data(), tag, key.data(), nonce,
                                      aad.data(), aad.size(), cipher, cipher_len);
    } else {
        crypto_aead_ctx ctx;
        crypto_aead_init_ietf(&ctx, key.data(), nonce);
        mismatch = crypto_aead_read(&ctx, plain.data(), tag,
                                    aad.data(), aad.size(), cipher, cipher_len);
        crypto_wipe(&ctx, sizeof(ctx));
    }

    if (mismatch != 0) {
        if (!plain.empty()) {
            crypto_wipe(plain.data(), plain.size());
        }
        return std::nullopt;
    }
    return plain;
}

[[nodiscard]] inline std::optional<std::uint8_t> PeekKeyId(std::span<const std::uint8_t> blob) noexcept {
    if (blob.size() < kHeaderFixedBytes) {
        return std::nullopt;
    }
    return blob[4];
}

[[nodiscard]] inline std::optional<CipherVariant> PeekVariant(std::span<const std::uint8_t> blob) noexcept {
    if (blob.size() < kHeaderFixedBytes) {
        return std::nullopt;
    }
    return static_cast<CipherVariant>(ReadU16(blob.data() + 2));
}

} // namespace vkengine::security::format
