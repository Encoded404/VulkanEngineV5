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
// Versions are additive and asymmetric:
//   * the server speaks every version in [kProtocolMinVersion, kProtocolVersion]
//     and keeps every handler it has ever shipped, so old clients keep working;
//   * the client requires the negotiated version to equal its own maximum, so a
//     new client never silently degrades against an out-of-date server.
// A changed message gets a new MessageType rather than a reshaped payload, so
// the server never has to branch a decoder on the negotiated version.
//
//   v2  X25519 key agreement; accounts, sessions, legacy "show on leaderboard".
//   v3  authenticated-only play (no v1 PSK), per-account submit filter,
//       per-account retention-aware rank, rename with a rejection reason, and
//       application-level Ping/Pong so an idle connection is kept warm and a
//       dead peer is detected without waiting for the next request.
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::uint16_t kProtocolVersion = 3;
inline constexpr std::uint16_t kProtocolMinVersion = 2;

inline constexpr std::size_t kHeaderSize = 32;
// AAD for encrypted payloads: the header prefix up to (but not including)
// payload_len, which is only known after sealing. Binds magic, proto, type,
// flags, seq and config_hash.
inline constexpr std::size_t kAadSize = 24;
inline constexpr std::uint8_t kFlagEncrypted = 0x01;
inline constexpr std::size_t kNonceSize = 16;
inline constexpr std::size_t kPublicKeySize = 32;
inline constexpr std::size_t kMaxErrorText = 256;
inline constexpr std::size_t kMaxReasonText = 256;
inline constexpr std::size_t kMaxNameField = 128;

enum class MessageType : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    Submit = 3,
    SubmitAck = 4,
    TopRequest = 5,
    TopReply = 6,
    Error = 7,
    RegisterRequest = 8,
    RegisterReply = 9,
    LoginRequest = 10,
    LoginReply = 11,
    ResumeRequest = 12,
    ResumeReply = 13,
    SettingsUpdate = 14, // v2 only
    SettingsReply = 15,  // v2 only
    // ── v3 ──
    SubmitV3 = 16,
    SubmitAckV3 = 17,
    TopRequestV3 = 18,
    RegisterReplyV3 = 19,
    RenameRequest = 20,
    RenameReplyV3 = 21,
    // ── v3 liveness ──
    Ping = 22,
    Pong = 23,
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

// ── Secure handshake (cleartext, X25519) ──
// The payload shape is shared by every account-capable version; the version
// travels in the frame header and the reply's `proto`.
struct HelloSecureMessage {
    std::uint16_t max_proto = kProtocolVersion;
    std::uint16_t cipher_mask = 0; // bit i set = variant i supported
    std::uint64_t config_hash = 0;
    std::array<std::byte, kNonceSize> client_nonce{};
    std::array<std::byte, kPublicKeySize> client_public_key{};
};

struct HelloAckSecureMessage {
    std::uint16_t proto = kProtocolVersion;
    std::uint16_t cipher_variant = 0;
    std::uint8_t key_id = 0;
    std::array<std::byte, kNonceSize> server_nonce{};
    std::array<std::byte, kPublicKeySize> server_public_key{};
    std::uint32_t session_id = 0;
};

[[nodiscard]] std::vector<std::byte> Encode(const HelloSecureMessage& message);
[[nodiscard]] std::optional<HelloSecureMessage> DecodeHello(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const HelloAckSecureMessage& message);
[[nodiscard]] std::optional<HelloAckSecureMessage> DecodeHelloAck(std::span<const std::byte> payload);

// ── Score messages ──
struct SubmitMessage {
    std::uint64_t run_id = 0; // client-generated, for idempotent retries
    std::int32_t score = 0;
};

// v3 adds the display filter the acknowledgement should rank against.
struct SubmitV3Message {
    std::uint64_t run_id = 0;
    std::int32_t score = 0;
    bool best_per_account = false;
};

struct SubmitAckMessage {
    std::int32_t rank = 0;
    std::int32_t total = 0;
    std::int32_t best = 0;
};

// v3 drops `total` and reports both relative standings, so the client can show
// whichever the display toggle currently selects.
struct SubmitAckV3Message {
    std::int32_t rank_runs = 0;
    std::int32_t rank_accounts = 0;
    std::int32_t best = 0;
};

struct TopRequestMessage {
    std::uint16_t count = 10;
    bool best_per_account = false;
    std::uint64_t since = 0; // Unix seconds; 0 = no lower bound
};

// v3 can additionally restrict the reply to the caller's own scores.
struct TopRequestV3Message {
    std::uint16_t count = 10;
    bool best_per_account = false;
    std::uint64_t since = 0;
    bool only_mine = false;
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
[[nodiscard]] std::vector<std::byte> Encode(const SubmitV3Message& message);
[[nodiscard]] std::optional<SubmitV3Message> DecodeSubmitV3(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const SubmitAckMessage& message);
[[nodiscard]] std::optional<SubmitAckMessage> DecodeSubmitAck(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const SubmitAckV3Message& message);
[[nodiscard]] std::optional<SubmitAckV3Message> DecodeSubmitAckV3(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const TopRequestMessage& message);
[[nodiscard]] std::optional<TopRequestMessage> DecodeTopRequest(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const TopRequestV3Message& message);
[[nodiscard]] std::optional<TopRequestV3Message> DecodeTopRequestV3(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const TopReplyMessage& message);
[[nodiscard]] std::optional<TopReplyMessage> DecodeTopReply(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const ErrorMessage& message);
[[nodiscard]] std::optional<ErrorMessage> DecodeError(std::span<const std::byte> payload);

// ── Account messages ──
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

// v3 carries a human-readable reason for any refusal.
struct RegisterReplyV3Message {
    SyncStatus status = SyncStatus::ServerError;
    std::string reason;
    UserId user_id = 0;
    std::string token;
    std::string display_name;
};

struct LoginRequestMessage {
    std::string username;
    std::string token; // hex
};

// The trailing visibility byte is legacy: v3 clients ignore it, v2 clients
// still read it, and keeping the shape shared avoids a reply branch.
struct LoginReplyMessage {
    SyncStatus status = SyncStatus::ServerError;
    AccountInfo account{};
    bool show_on_leaderboard = true;
    std::string session_token;            // hex, present only when status == Ok
    std::uint64_t session_expires_at = 0; // unix seconds
};

struct ResumeRequestMessage {
    std::string session_token; // hex
};

// v2 settings update: rename plus the legacy visibility flag.
struct SettingsUpdateMessage {
    std::string display_name;
    bool show_on_leaderboard = true;
};

struct SettingsReplyMessage {
    SyncStatus status = SyncStatus::ServerError;
    AccountInfo account{};
    bool show_on_leaderboard = true;
};

// v3 rename: display name only, with a rejection reason in the reply.
struct RenameRequestMessage {
    std::string display_name;
};

struct RenameReplyV3Message {
    SyncStatus status = SyncStatus::ServerError;
    std::string reason;
    AccountInfo account{};
};

// ── v3 liveness ──
// Ping carries an opaque token that Pong echoes, so the sender can reject a
// stale or mismatched reply even though the frame sequence already pairs them.
struct PingMessage {
    std::uint64_t token = 0;
};

struct PongMessage {
    std::uint64_t token = 0;
};

[[nodiscard]] std::vector<std::byte> Encode(const RegisterRequestMessage& message);
[[nodiscard]] std::optional<RegisterRequestMessage> DecodeRegisterRequest(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const RegisterReplyMessage& message);
[[nodiscard]] std::optional<RegisterReplyMessage> DecodeRegisterReply(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const RegisterReplyV3Message& message);
[[nodiscard]] std::optional<RegisterReplyV3Message> DecodeRegisterReplyV3(std::span<const std::byte> payload);
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
[[nodiscard]] std::vector<std::byte> Encode(const RenameRequestMessage& message);
[[nodiscard]] std::optional<RenameRequestMessage> DecodeRenameRequest(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const RenameReplyV3Message& message);
[[nodiscard]] std::optional<RenameReplyV3Message> DecodeRenameReplyV3(std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> Encode(const PingMessage& message);
[[nodiscard]] std::optional<PingMessage> DecodePing(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> Encode(const PongMessage& message);
[[nodiscard]] std::optional<PongMessage> DecodePong(std::span<const std::byte> payload);

// Cipher negotiation helpers. Bit i corresponds to CipherVariant value i.
[[nodiscard]] std::uint16_t SupportedCipherMask();
[[nodiscard]] std::optional<VulkanEngine::Security::CipherVariant> PickCipher(std::uint16_t peer_mask);

} // namespace Examples::InfiniteRunner::Leaderboard
