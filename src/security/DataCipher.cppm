module;

export module VulkanEngine.DataCipher;

import std;

export namespace VulkanEngine::Security {

// ── Versioned, per-datum encryption ─────────────────────────────────────
//
// A cipher variant identifies an algorithm + nonce/tag geometry. Variants are
// append-only: the ids are part of the on-disk envelope, so an old blob stays
// readable after new variants are added.
enum class CipherVariant : std::uint16_t {
    None = 0,                  // dev only: no sealing
    XChaCha20Poly1305 = 1,     // 24-byte nonce (recommended default)
    ChaCha20Poly1305Ietf = 2,  // 12-byte nonce
};

struct CipherSpec {
    CipherVariant variant;
    std::size_t key_bytes;
    std::size_t nonce_bytes;
    std::size_t tag_bytes;
    std::string_view name;
};

inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kMaxNonceBytes = 24;
inline constexpr std::size_t kTagBytes = 16;

[[nodiscard]] std::span<const CipherSpec> AllCipherSpecs() noexcept;
[[nodiscard]] const CipherSpec* GetCipherSpec(CipherVariant variant) noexcept;
[[nodiscard]] std::optional<std::size_t> NonceBytes(CipherVariant variant) noexcept;

// Several keys can be live at once (key rotation). The key id is recorded in
// the envelope header, so data sealed under an old key keeps working.
class KeyRing {
public:
    KeyRing() = default;
    ~KeyRing();
    KeyRing(const KeyRing&) = delete;
    KeyRing& operator=(const KeyRing&) = delete;
    KeyRing(KeyRing&&) noexcept = default;
    KeyRing& operator=(KeyRing&&) noexcept = default;

    void Add(std::uint8_t key_id, std::span<const std::byte> key);
    [[nodiscard]] bool Contains(std::uint8_t key_id) const noexcept;
    [[nodiscard]] std::span<const std::byte> Get(std::uint8_t key_id) const noexcept;
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;

private:
    std::vector<std::pair<std::uint8_t, std::vector<std::byte>>> keys_;
};

// Seal `plaintext` under `key_id` with a caller-supplied nonce. Deterministic
// callers (the build-time generator) derive the nonce from the content so
// generated artifacts and binaries stay reproducible.
[[nodiscard]] std::vector<std::byte> Seal(const KeyRing& keys,
                                          CipherVariant variant,
                                          std::uint8_t key_id,
                                          std::span<const std::byte> nonce,
                                          std::span<const std::byte> plaintext,
                                          std::span<const std::byte> aad = {});

// Seal with a freshly generated nonce.
[[nodiscard]] std::vector<std::byte> SealRandom(const KeyRing& keys,
                                                CipherVariant variant,
                                                std::uint8_t key_id,
                                                std::span<const std::byte> plaintext,
                                                std::span<const std::byte> aad = {});

// Open an envelope. Selects the key from the envelope's key id. Returns
// nullopt for a malformed envelope, an unknown variant, a missing key, or a
// failed authentication tag.
[[nodiscard]] std::optional<std::vector<std::byte>> Open(const KeyRing& keys,
                                                         std::span<const std::byte> blob,
                                                         std::span<const std::byte> aad = {});

// Header inspection, so a reader can log or dispatch before decrypting.
[[nodiscard]] std::optional<std::uint8_t> PeekKeyId(std::span<const std::byte> blob) noexcept;
[[nodiscard]] std::optional<CipherVariant> PeekVariant(std::span<const std::byte> blob) noexcept;

// Nonce bytes for the requested variant from the process random source.
[[nodiscard]] std::vector<std::byte> RandomNonce(CipherVariant variant);

// `count` bytes from the process random source (for handshake nonces and the
// like). Uniqueness, not secrecy.
[[nodiscard]] std::vector<std::byte> RandomBytes(std::size_t count);

// Simple BLAKE2b-based key derivation: hash(ikm || salt || context). The
// context string is domain separation, so the same inputs used for different
// purposes yield different keys. Not HKDF; adequate for a placeholder session
// key and easy to replace later.
[[nodiscard]] std::vector<std::byte> DeriveKey(std::span<const std::byte> ikm,
                                               std::span<const std::byte> salt,
                                               std::string_view context,
                                               std::size_t out_bytes);

} // namespace VulkanEngine::Security
