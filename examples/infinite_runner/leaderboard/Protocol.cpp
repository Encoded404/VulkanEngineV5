module;

module Examples.InfiniteRunner.Leaderboard.Protocol;

import std;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

constexpr std::array<std::byte, 4> kMagic = {
    static_cast<std::byte>('I'),
    static_cast<std::byte>('R'),
    static_cast<std::byte>('L'),
    static_cast<std::byte>('B'),
};

void PutU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFU));
}

void PutU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
    }
}

void PutU64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFU));
    }
}

void PutBytes(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void PutString(std::vector<std::byte>& out, std::string_view text) {
    PutU16(out, static_cast<std::uint16_t>(text.size()));
    PutBytes(out, {reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

struct Reader {
    std::span<const std::byte> data;
    std::size_t offset = 0;

    [[nodiscard]] bool ReadU8(std::uint8_t& value) {
        if (offset + 1 > data.size()) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(data[offset++]);
        return true;
    }

    [[nodiscard]] bool ReadU16(std::uint16_t& value) {
        if (offset + 2 > data.size()) {
            return false;
        }
        value = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[offset])) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[offset + 1])) << 8);
        offset += 2;
        return true;
    }

    [[nodiscard]] bool ReadU32(std::uint32_t& value) {
        if (offset + 4 > data.size()) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[offset + i])) << (8 * i);
        }
        offset += 4;
        return true;
    }

    [[nodiscard]] bool ReadU64(std::uint64_t& value) {
        if (offset + 8 > data.size()) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[offset + i])) << (8 * i);
        }
        offset += 8;
        return true;
    }

    [[nodiscard]] bool ReadBytes(std::size_t count, std::span<const std::byte>& value) {
        if (offset + count > data.size()) {
            return false;
        }
        value = data.subspan(offset, count);
        offset += count;
        return true;
    }

    [[nodiscard]] bool ReadString(std::string& value) {
        std::uint16_t length = 0;
        if (!ReadU16(length)) {
            return false;
        }
        std::span<const std::byte> bytes;
        if (!ReadBytes(length, bytes)) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    [[nodiscard]] bool AtEnd() const { return offset == data.size(); }
};

} // namespace

std::array<std::byte, kHeaderSize> EncodeHeader(const FrameHeader& header) {
    std::vector<std::byte> out;
    out.reserve(kHeaderSize);
    PutBytes(out, kMagic);
    PutU16(out, header.proto);
    out.push_back(static_cast<std::byte>(header.type));
    out.push_back(static_cast<std::byte>(header.flags));
    PutU64(out, header.seq);
    PutU64(out, header.config_hash);
    PutU32(out, header.payload_len);
    PutU32(out, 0);

    std::array<std::byte, kHeaderSize> result{};
    std::copy(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), result.begin());
    return result;
}

std::optional<FrameHeader> DecodeHeader(std::span<const std::byte> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::nullopt;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return std::nullopt;
    }

    Reader reader{bytes.subspan(4)};
    FrameHeader header{};
    std::uint8_t type = 0;
    if (!reader.ReadU16(header.proto) || !reader.ReadU8(type) || !reader.ReadU8(header.flags) ||
        !reader.ReadU64(header.seq) || !reader.ReadU64(header.config_hash) ||
        !reader.ReadU32(header.payload_len)) {
        return std::nullopt;
    }
    header.type = static_cast<MessageType>(type);
    return header;
}

std::vector<std::byte> Encode(const HelloMessage& message) {
    std::vector<std::byte> out;
    PutU16(out, message.max_proto);
    PutU16(out, message.cipher_mask);
    PutU64(out, message.config_hash);
    PutBytes(out, message.client_nonce);
    return out;
}

std::optional<HelloMessage> DecodeHello(std::span<const std::byte> payload) {
    Reader reader{payload};
    HelloMessage message{};
    std::span<const std::byte> nonce;
    if (!reader.ReadU16(message.max_proto) || !reader.ReadU16(message.cipher_mask) ||
        !reader.ReadU64(message.config_hash) || !reader.ReadBytes(kNonceSize, nonce)) {
        return std::nullopt;
    }
    std::copy(nonce.begin(), nonce.end(), message.client_nonce.begin());
    return message;
}

std::vector<std::byte> Encode(const HelloAckMessage& message) {
    std::vector<std::byte> out;
    PutU16(out, message.proto);
    PutU16(out, message.cipher_variant);
    out.push_back(static_cast<std::byte>(message.key_id));
    out.push_back(std::byte{0});
    PutBytes(out, message.server_nonce);
    PutU32(out, message.session_id);
    return out;
}

std::optional<HelloAckMessage> DecodeHelloAck(std::span<const std::byte> payload) {
    Reader reader{payload};
    HelloAckMessage message{};
    std::uint8_t reserved = 0;
    std::span<const std::byte> nonce;
    if (!reader.ReadU16(message.proto) || !reader.ReadU16(message.cipher_variant) ||
        !reader.ReadU8(message.key_id) || !reader.ReadU8(reserved) ||
        !reader.ReadBytes(kNonceSize, nonce) || !reader.ReadU32(message.session_id)) {
        return std::nullopt;
    }
    std::copy(nonce.begin(), nonce.end(), message.server_nonce.begin());
    return message;
}

std::vector<std::byte> Encode(const SubmitMessage& message) {
    std::vector<std::byte> out;
    PutU64(out, message.run_id);
    PutU32(out, static_cast<std::uint32_t>(message.score));
    return out;
}

std::optional<SubmitMessage> DecodeSubmit(std::span<const std::byte> payload) {
    Reader reader{payload};
    SubmitMessage message{};
    std::uint32_t score = 0;
    if (!reader.ReadU64(message.run_id) || !reader.ReadU32(score) || !reader.AtEnd()) {
        return std::nullopt;
    }
    message.score = static_cast<std::int32_t>(score);
    return message;
}

std::vector<std::byte> Encode(const SubmitAckMessage& message) {
    std::vector<std::byte> out;
    PutU32(out, static_cast<std::uint32_t>(message.rank));
    PutU32(out, static_cast<std::uint32_t>(message.total));
    PutU32(out, static_cast<std::uint32_t>(message.best));
    return out;
}

std::optional<SubmitAckMessage> DecodeSubmitAck(std::span<const std::byte> payload) {
    Reader reader{payload};
    SubmitAckMessage message{};
    std::uint32_t rank = 0;
    std::uint32_t total = 0;
    std::uint32_t best = 0;
    if (!reader.ReadU32(rank) || !reader.ReadU32(total) || !reader.ReadU32(best) || !reader.AtEnd()) {
        return std::nullopt;
    }
    message.rank = static_cast<std::int32_t>(rank);
    message.total = static_cast<std::int32_t>(total);
    message.best = static_cast<std::int32_t>(best);
    return message;
}

std::vector<std::byte> Encode(const TopRequestMessage& message) {
    std::vector<std::byte> out;
    PutU16(out, message.count);
    out.push_back(static_cast<std::byte>(message.best_per_account ? 1 : 0));
    PutU64(out, message.since);
    return out;
}

std::optional<TopRequestMessage> DecodeTopRequest(std::span<const std::byte> payload) {
    Reader reader{payload};
    TopRequestMessage message{};
    std::uint8_t best_per_account = 0;
    if (!reader.ReadU16(message.count) || !reader.ReadU8(best_per_account) ||
        !reader.ReadU64(message.since) || !reader.AtEnd()) {
        return std::nullopt;
    }
    message.best_per_account = best_per_account != 0;
    return message;
}

std::vector<std::byte> Encode(const TopReplyMessage& message) {
    std::vector<std::byte> out;
    PutU16(out, static_cast<std::uint16_t>(message.entries.size()));
    for (const TopEntry& entry : message.entries) {
        PutU32(out, static_cast<std::uint32_t>(entry.rank));
        PutU32(out, static_cast<std::uint32_t>(entry.score));
        PutString(out, entry.display_name.substr(0, kMaxNameField));
        PutU64(out, entry.user_id);
        PutU64(out, entry.recorded_at);
    }
    return out;
}

std::optional<TopReplyMessage> DecodeTopReply(std::span<const std::byte> payload) {
    Reader reader{payload};
    TopReplyMessage message{};
    std::uint16_t count = 0;
    if (!reader.ReadU16(count)) {
        return std::nullopt;
    }
    message.entries.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint32_t rank = 0;
        std::uint32_t score = 0;
        std::string display_name;
        std::uint64_t user_id = 0;
        std::uint64_t recorded_at = 0;
        if (!reader.ReadU32(rank) || !reader.ReadU32(score) || !reader.ReadString(display_name) ||
            !reader.ReadU64(user_id) || !reader.ReadU64(recorded_at)) {
            return std::nullopt;
        }
        message.entries.push_back(TopEntry{static_cast<std::int32_t>(rank),
                                           static_cast<std::int32_t>(score),
                                           std::move(display_name), user_id, recorded_at});
    }
    if (!reader.AtEnd()) {
        return std::nullopt;
    }
    return message;
}

std::vector<std::byte> Encode(const ErrorMessage& message) {
    std::vector<std::byte> out;
    PutU16(out, message.code);
    PutString(out, message.text.substr(0, kMaxErrorText));
    return out;
}

std::optional<ErrorMessage> DecodeError(std::span<const std::byte> payload) {
    Reader reader{payload};
    ErrorMessage message{};
    if (!reader.ReadU16(message.code) || !reader.ReadString(message.text)) {
        return std::nullopt;
    }
    return message;
}

std::uint16_t SupportedCipherMask() {
    // Variants we can both seal and open. Bit i corresponds to variant value i.
    return static_cast<std::uint16_t>((1U << 1U) | (1U << 2U));
}

std::optional<VulkanEngine::Security::CipherVariant> PickCipher(std::uint16_t peer_mask) {
    const std::uint16_t both = static_cast<std::uint16_t>(peer_mask & SupportedCipherMask());
    if ((both & (1U << 2U)) != 0U) {
        return VulkanEngine::Security::CipherVariant::ChaCha20Poly1305Ietf;
    }
    if ((both & (1U << 1U)) != 0U) {
        return VulkanEngine::Security::CipherVariant::XChaCha20Poly1305;
    }
    return std::nullopt;
}


std::string_view ToString(MessageType type) {
    switch (type) {
        case MessageType::Hello: return "hello";
        case MessageType::HelloAck: return "hello-ack";
        case MessageType::Submit: return "submit";
        case MessageType::SubmitAck: return "submit-ack";
        case MessageType::TopRequest: return "top-request";
        case MessageType::TopReply: return "top-reply";
        case MessageType::Error: return "error";
        case MessageType::RegisterRequest: return "register";
        case MessageType::RegisterReply: return "register-reply";
        case MessageType::LoginRequest: return "login";
        case MessageType::LoginReply: return "login-reply";
        case MessageType::ResumeRequest: return "resume";
        case MessageType::ResumeReply: return "resume-reply";
        case MessageType::SettingsUpdate: return "settings-update";
        case MessageType::SettingsReply: return "settings-reply";
    }
    return "unknown";
}

std::vector<std::byte> Encode(const HelloV2Message& message) {
    std::vector<std::byte> out;
    PutU16(out, message.max_proto);
    PutU16(out, message.cipher_mask);
    PutU64(out, message.config_hash);
    PutBytes(out, message.client_nonce);
    PutBytes(out, message.client_public_key);
    return out;
}

std::optional<HelloV2Message> DecodeHelloV2(std::span<const std::byte> payload) {
    Reader reader{payload};
    HelloV2Message message{};
    std::span<const std::byte> nonce;
    std::span<const std::byte> key;
    if (!reader.ReadU16(message.max_proto) || !reader.ReadU16(message.cipher_mask) ||
        !reader.ReadU64(message.config_hash) || !reader.ReadBytes(kNonceSize, nonce) ||
        !reader.ReadBytes(kPublicKeySize, key)) {
        return std::nullopt;
    }
    std::copy(nonce.begin(), nonce.end(), message.client_nonce.begin());
    std::copy(key.begin(), key.end(), message.client_public_key.begin());
    return message;
}

std::vector<std::byte> Encode(const HelloAckV2Message& message) {
    std::vector<std::byte> out;
    PutU16(out, message.proto);
    PutU16(out, message.cipher_variant);
    out.push_back(static_cast<std::byte>(message.key_id));
    out.push_back(std::byte{0});
    PutBytes(out, message.server_nonce);
    PutBytes(out, message.server_public_key);
    PutU32(out, message.session_id);
    return out;
}

std::optional<HelloAckV2Message> DecodeHelloAckV2(std::span<const std::byte> payload) {
    Reader reader{payload};
    HelloAckV2Message message{};
    std::uint8_t reserved = 0;
    std::span<const std::byte> nonce;
    std::span<const std::byte> key;
    if (!reader.ReadU16(message.proto) || !reader.ReadU16(message.cipher_variant) ||
        !reader.ReadU8(message.key_id) || !reader.ReadU8(reserved) ||
        !reader.ReadBytes(kNonceSize, nonce) || !reader.ReadBytes(kPublicKeySize, key) ||
        !reader.ReadU32(message.session_id)) {
        return std::nullopt;
    }
    std::copy(nonce.begin(), nonce.end(), message.server_nonce.begin());
    std::copy(key.begin(), key.end(), message.server_public_key.begin());
    return message;
}

std::vector<std::byte> Encode(const RegisterRequestMessage& message) {
    std::vector<std::byte> out;
    PutString(out, message.username.substr(0, kMaxNameField));
    PutString(out, message.display_name.substr(0, kMaxNameField));
    return out;
}

std::optional<RegisterRequestMessage> DecodeRegisterRequest(std::span<const std::byte> payload) {
    Reader reader{payload};
    RegisterRequestMessage message{};
    if (!reader.ReadString(message.username) || !reader.ReadString(message.display_name) ||
        !reader.AtEnd()) {
        return std::nullopt;
    }
    if (message.username.size() > kMaxNameField || message.display_name.size() > kMaxNameField) {
        return std::nullopt;
    }
    return message;
}

std::vector<std::byte> Encode(const RegisterReplyMessage& message) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(message.status));
    PutU64(out, message.user_id);
    PutString(out, message.token);
    PutString(out, message.display_name);
    return out;
}

std::optional<RegisterReplyMessage> DecodeRegisterReply(std::span<const std::byte> payload) {
    Reader reader{payload};
    RegisterReplyMessage message{};
    std::uint8_t status = 0;
    if (!reader.ReadU8(status) || !reader.ReadU64(message.user_id) ||
        !reader.ReadString(message.token) || !reader.ReadString(message.display_name) ||
        !reader.AtEnd()) {
        return std::nullopt;
    }
    message.status = static_cast<SyncStatus>(status);
    return message;
}

std::vector<std::byte> Encode(const LoginRequestMessage& message) {
    std::vector<std::byte> out;
    PutString(out, message.username);
    PutString(out, message.token);
    return out;
}

std::optional<LoginRequestMessage> DecodeLoginRequest(std::span<const std::byte> payload) {
    Reader reader{payload};
    LoginRequestMessage message{};
    if (!reader.ReadString(message.username) || !reader.ReadString(message.token) || !reader.AtEnd()) {
        return std::nullopt;
    }
    return message;
}

std::vector<std::byte> Encode(const LoginReplyMessage& message) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(message.status));
    PutU64(out, message.account.id);
    PutString(out, message.account.username);
    PutString(out, message.account.display_name);
    out.push_back(static_cast<std::byte>(message.settings.show_on_leaderboard ? 1 : 0));
    PutString(out, message.session_token);
    PutU64(out, message.session_expires_at);
    return out;
}

std::optional<LoginReplyMessage> DecodeLoginReply(std::span<const std::byte> payload) {
    Reader reader{payload};
    LoginReplyMessage message{};
    std::uint8_t status = 0;
    std::uint8_t show = 0;
    if (!reader.ReadU8(status) || !reader.ReadU64(message.account.id) ||
        !reader.ReadString(message.account.username) || !reader.ReadString(message.account.display_name) ||
        !reader.ReadU8(show) || !reader.ReadString(message.session_token) ||
        !reader.ReadU64(message.session_expires_at) || !reader.AtEnd()) {
        return std::nullopt;
    }
    message.status = static_cast<SyncStatus>(status);
    message.settings.show_on_leaderboard = show != 0;
    return message;
}

std::vector<std::byte> Encode(const ResumeRequestMessage& message) {
    std::vector<std::byte> out;
    PutString(out, message.session_token);
    return out;
}

std::optional<ResumeRequestMessage> DecodeResumeRequest(std::span<const std::byte> payload) {
    Reader reader{payload};
    ResumeRequestMessage message{};
    if (!reader.ReadString(message.session_token) || !reader.AtEnd()) {
        return std::nullopt;
    }
    return message;
}

std::vector<std::byte> Encode(const SettingsUpdateMessage& message) {
    std::vector<std::byte> out;
    PutString(out, message.display_name.substr(0, kMaxNameField));
    out.push_back(static_cast<std::byte>(message.settings.show_on_leaderboard ? 1 : 0));
    return out;
}

std::optional<SettingsUpdateMessage> DecodeSettingsUpdate(std::span<const std::byte> payload) {
    Reader reader{payload};
    SettingsUpdateMessage message{};
    std::uint8_t show = 0;
    if (!reader.ReadString(message.display_name) || !reader.ReadU8(show) || !reader.AtEnd()) {
        return std::nullopt;
    }
    message.settings.show_on_leaderboard = show != 0;
    return message;
}

std::vector<std::byte> Encode(const SettingsReplyMessage& message) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(message.status));
    PutU64(out, message.account.id);
    PutString(out, message.account.username);
    PutString(out, message.account.display_name);
    out.push_back(static_cast<std::byte>(message.settings.show_on_leaderboard ? 1 : 0));
    return out;
}

std::optional<SettingsReplyMessage> DecodeSettingsReply(std::span<const std::byte> payload) {
    Reader reader{payload};
    SettingsReplyMessage message{};
    std::uint8_t status = 0;
    std::uint8_t show = 0;
    if (!reader.ReadU8(status) || !reader.ReadU64(message.account.id) ||
        !reader.ReadString(message.account.username) || !reader.ReadString(message.account.display_name) ||
        !reader.ReadU8(show) || !reader.AtEnd()) {
        return std::nullopt;
    }
    message.status = static_cast<SyncStatus>(status);
    message.settings.show_on_leaderboard = show != 0;
    return message;
}

} // namespace Examples::InfiniteRunner::Leaderboard
