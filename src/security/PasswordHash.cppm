module;

export module VulkanEngine.PasswordHash;

import std;

export namespace VulkanEngine::Security {

// ─────────────────────────────────────────────────────────────────────────────
// Server-side credential hashing (Argon2id).
//
// Used to store an account credential without keeping anything a leak can
// replay: the stored string is a slow one-way verifier, never the credential
// itself. The salt is random per account, and the work parameters travel inside
// the encoded string so old verifiers keep verifying after the defaults change.
// ─────────────────────────────────────────────────────────────────────────────

struct Argon2Params {
    // Memory hardness in 1 KiB blocks; must be >= 8 * lanes. The default is
    // 64 MiB, deliberately slow for an interactive login and cheap enough for
    // this project. Tests use tiny values.
    std::uint32_t blocks = 65536;
    std::uint32_t passes = 3;
    std::uint32_t lanes = 1;
    std::uint32_t hash_bytes = 32;
    std::uint32_t salt_bytes = 16;

    // Argon2 is CPU-heavy on purpose; these bound the accepted work so a
    // malformed stored string cannot make a verifying server allocate forever.
    static constexpr std::uint32_t kMaxBlocks = 1u << 21; // 2 GiB
    static constexpr std::uint32_t kMaxPasses = 16;
    static constexpr std::uint32_t kMaxLanes = 8;
};

// Encoded as: argon2id$<blocks>$<passes>$<lanes>$<salt_hex>$<hash_hex>
// Returns empty when the parameters are unusable.
[[nodiscard]] std::string HashPassword(std::span<const std::byte> password,
                                       const Argon2Params& params = {});

// Recompute and compare in constant time. False for a malformed verifier or a
// wrong password.
[[nodiscard]] bool VerifyPassword(std::span<const std::byte> password,
                                  std::string_view stored);

// Constant-time equality. A length mismatch returns false; lengths are not
// treated as secret.
[[nodiscard]] bool ConstantTimeEquals(std::span<const std::byte> a,
                                      std::span<const std::byte> b) noexcept;

} // namespace VulkanEngine::Security
