module;

export module Examples.InfiniteRunner.Leaderboard.Protocol;

import std;

import VulkanEngine.DataCipher;
export import Examples.InfiniteRunner.Leaderboard.Account;

export namespace Examples::InfiniteRunner::Leaderboard {

// ─────────────────────────────────────────────────────────────────────────────
// Leaderboard wire protocol.
//
// A frame is [FrameHeader][payload]. The header is cleartext so a peer can
// negotiate a protocol version and cipher before it holds a session key. The
// payload is sealed once a session exists; messages exchanged before the
// handshake completes are cleartext.
//
// Versions are additive: a new version gets its own encode/decode pair and the
// server keeps every handler it has ever shipped. The handshake advertises the
// highest version each side speaks and both drop to the lower one.
//
//   v1  PSK session key; anonymous scores.
//   v2  X25519 key agreement; accounts, sessions and synced settings.
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::uint16_t kProtocolVersion = 2;
inline constexpr std::uint16_t kProtocolMinVersion = 1;
inline constexpr std::uint16_t kProtocolMaxVersion = 2;

inline constexpr std::size_t kHeaderSize = 32;
// AAD for encrypted payloads: the header prefix up to (but not including)
// payload_len, which is only known after sealing. Binds magic, proto, type,
// flags, seq and config_hash.
inline constexpr std::size_t kAadSize = 24;
inline constexpr std::uint8_t kFlagEncrypted = 0x01;
inline constexpr std::size_t kNonceSize = 16;
inline constexpr std::size_t kPublicKeySize = 32;
inline constexpr std::size_t kMaxErrorText = 256;
inline constexpr std::size_t kMaxNameField = 128;

enum class MessageType : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    Submit = 3,
    SubmitAck = 4,
    TopRequest = 5,
    TopReply = 6,
    Error = 7,
    // v2 account messages.
    RegisterRequest = 8,
    RegisterReply = 9,
    LoginRequest = 10,
    LoginReply = 11,
    ResumeRequest = 12,
    ResumeReply = 13,
    SettingsUpdate = 14,
    SettingsReply = 15,
};

[[nodiscard]] std::string_view ToString(MessageType type);

struct FrameHeader {
    std::uint16_t proto = kProtocolVersion;
    MessageType type = MessageType::Error;
    std::uint8_t flags = 0;
    std::uint64_t seq = 0;
    std::uint64_t config_hash = 0;
    std::uint32_t payload_len = 0;
};

[[nodiscard]] std::array<std::byte, kHeaderSize> EncodeHeader(const FrameHeader& header);
[[nodiscard]] std::optional<FrameHeader> DecodeHeader(std::span<const std::byte> bytes);

// ── Hello / HelloAck (cleartext handshake) ──
struct HelloMessage {
    std::uint16_t max_proto = kProtocolVersion;
    std::uint16_t cipher_mask = 0; // bit i set = variant i supported
    std::uint64_t config_hash = 0;
    std::array<std::byte, kNonceSize> client_nonce{};
};

struct HelloAckMessage {
    std::uint16_t proto = kProtocolVersion;
    std::uint16_t cipher_variant = 0;
    std::uint8_t key_id = 0;
    std::array<std::byte, kNonceSize> server_nonce{};
    std::uint32_t session_id = 0;
};

// v2 adds the ephemeral X25519 public keys used to agree a session key.
struct HelloV2Message {
    std::uint16_t max_proto = kProtocolVersion;
    std::uint16_t cipher_mask = 0;
    std::uint64_t config_hash = 0;
    std::array<std::byte, kNonceSize> client_nonce{};
    std::array<std::byte, kPublicKeySize> client_public_key{};
};

struct HelloAckV2Message {
    std::uint16_t proto = kProtocolVersion;
    std::uint16_t cipher_variant = 0;
    std::uint8_t key_id = 0;
    std::array<std::byte, kNonceSize> server_nonce{};
    std::array<std::byte, kPublicKeySize> server_public_key{};
    std::uint32_t session_id = 0;
};

[[nodiscard]] std::vector<std::byte> Encode(const HelloMessage& message);
[[nodiscard]] std::optional<HelloMessage> DecodeHello(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const HelloAckMessage& message);
[[nodiscard]] std::optional<HelloAckMessage> DecodeHelloAck(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const HelloV2Message& message);
[[nodiscard]] std::optional<HelloV2Message> DecodeHelloV2(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const HelloAckV2Message& message);
[[nodiscard]] std::optional<HelloAckV2Message> DecodeHelloAckV2(std::span<const std::byte> payload);

// ── Score messages ──
struct SubmitMessage {
    std::uint64_t run_id = 0; // client-generated, for idempotent retries
    std::int32_t score = 0;
};

struct SubmitAckMessage {
    std::int32_t rank = 0;
    std::int32_t total = 0;
    std::int32_t best = 0;
};

struct TopRequestMessage {
    std::uint16_t count = 10;
    // Server-side filters, so the client never receives rows it would discard.
    bool best_per_account = false;
    std::uint64_t since = 0; // Unix seconds; 0 = no lower bound
};

struct TopEntry {
    std::int32_t rank = 0;
    std::int32_t score = 0;
    std::string display_name;
    std::uint64_t user_id = 0;
    std::uint64_t recorded_at = 0; // Unix seconds
};

struct TopReplyMessage {
    std::vector<TopEntry> entries;
};

struct ErrorMessage {
    std::uint16_t code = 0;
    std::string text;
};

[[nodiscard]] std::vector<std::byte> Encode(const SubmitMessage& message);
[[nodiscard]] std::optional<SubmitMessage> DecodeSubmit(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const SubmitAckMessage& message);
[[nodiscard]] std::optional<SubmitAckMessage> DecodeSubmitAck(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const TopRequestMessage& message);
[[nodiscard]] std::optional<TopRequestMessage> DecodeTopRequest(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const TopReplyMessage& message);
[[nodiscard]] std::optional<TopReplyMessage> DecodeTopReply(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const ErrorMessage& message);
[[nodiscard]] std::optional<ErrorMessage> DecodeError(std::span<const std::byte> payload);

// ── v2 account messages ──
struct RegisterRequestMessage {
    std::string username;
    std::string display_name;
};

struct RegisterReplyMessage {
    SyncStatus status = SyncStatus::ServerError;
    UserId user_id = 0;
    std::string token; // hex, present only when status == Ok
    std::string display_name;
};

struct LoginRequestMessage {
    std::string username;
    std::string token; // hex
};

struct LoginReplyMessage {
    SyncStatus status = SyncStatus::ServerError;
    AccountInfo account{};
    SyncedSettings settings{};
    std::string session_token;            // hex, present only when status == Ok
    std::uint64_t session_expires_at = 0; // unix seconds
};

struct ResumeRequestMessage {
    std::string session_token; // hex
};

struct SettingsUpdateMessage {
    std::string display_name;
    SyncedSettings settings{};
};

struct SettingsReplyMessage {
    SyncStatus status = SyncStatus::ServerError;
    AccountInfo account{};
    SyncedSettings settings{};
};

[[nodiscard]] std::vector<std::byte> Encode(const RegisterRequestMessage& message);
[[nodiscard]] std::optional<RegisterRequestMessage> DecodeRegisterRequest(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const RegisterReplyMessage& message);
[[nodiscard]] std::optional<RegisterReplyMessage> DecodeRegisterReply(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const LoginRequestMessage& message);
[[nodiscard]] std::optional<LoginRequestMessage> DecodeLoginRequest(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const LoginReplyMessage& message);
[[nodiscard]] std::optional<LoginReplyMessage> DecodeLoginReply(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const ResumeRequestMessage& message);
[[nodiscard]] std::optional<ResumeRequestMessage> DecodeResumeRequest(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const SettingsUpdateMessage& message);
[[nodiscard]] std::optional<SettingsUpdateMessage> DecodeSettingsUpdate(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const SettingsReplyMessage& message);
[[nodiscard]] std::optional<SettingsReplyMessage> DecodeSettingsReply(std::span<const std::byte> payload);

// Cipher negotiation helpers. Bit i corresponds to CipherVariant value i.
[[nodiscard]] std::uint16_t SupportedCipherMask();
[[nodiscard]] std::optional<VulkanEngine::Security::CipherVariant> PickCipher(std::uint16_t peer_mask);

} // namespace Examples::InfiniteRunner::Leaderboard
