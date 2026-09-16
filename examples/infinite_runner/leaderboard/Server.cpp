module;

module Examples.InfiniteRunner.Leaderboard.Server;

import std;

import VulkanEngine.DataCipher;
import VulkanEngine.KeyExchange;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.Config;
import Examples.InfiniteRunner.Leaderboard.Log;
import Examples.InfiniteRunner.Leaderboard.Protocol;
import Examples.InfiniteRunner.Leaderboard.SessionCrypto;
import Examples.InfiniteRunner.Leaderboard.Transport;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

using VulkanEngine::Security::CipherVariant;

[[nodiscard]] bool SendMessage(TcpSocket& socket, const FrameHeader& header,
                               std::span<const std::byte> payload) {
    const std::array<std::byte, kHeaderSize> header_bytes = EncodeHeader(header);
    std::vector<std::byte> frame(header_bytes.begin(), header_bytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return SendFrame(socket, frame);
}

[[nodiscard]] bool SendEncrypted(TcpSocket& socket, std::span<const std::byte> session_key,
                                 CipherVariant variant, std::uint16_t proto, MessageType type,
                                 std::uint64_t config_hash, std::uint64_t seq,
                                 std::span<const std::byte> inner) {
    FrameHeader header{proto, type, kFlagEncrypted, seq, config_hash, 0};
    const std::array<std::byte, kHeaderSize> header_bytes = EncodeHeader(header);
    const std::vector<std::byte> sealed = SealMessage(
        session_key, variant, seq, std::span<const std::byte>(header_bytes).subspan(0, kAadSize), inner);
    if (sealed.empty()) {
        return false;
    }
    header.payload_len = static_cast<std::uint32_t>(sealed.size());
    return SendMessage(socket, header, sealed);
}

[[nodiscard]] bool SendError(TcpSocket& socket, std::uint16_t proto, std::uint64_t config_hash,
                             std::uint64_t seq, std::uint16_t code, std::string_view text,
                             std::span<const std::byte> session_key, CipherVariant variant,
                             bool encrypted) {
    const ErrorMessage error{code, std::string(text)};
    const std::vector<std::byte> payload = Encode(error);
    if (encrypted) {
        return SendEncrypted(socket, session_key, variant, proto, MessageType::Error, config_hash, seq,
                             payload);
    }
    const FrameHeader header{proto, MessageType::Error, 0, seq, config_hash,
                             static_cast<std::uint32_t>(payload.size())};
    return SendMessage(socket, header, payload);
}

[[nodiscard]] std::optional<VulkanEngine::Security::X25519Key> ParseHexKey(std::string_view text) {
    std::string_view trimmed = text;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())) != 0) {
        trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) {
        trimmed.remove_suffix(1);
    }
    const std::optional<std::vector<std::byte>> bytes = TokenFromHex(trimmed);
    if (!bytes.has_value() || bytes->size() != VulkanEngine::Security::kX25519KeyBytes) {
        return std::nullopt;
    }
    VulkanEngine::Security::X25519Key key{};
    std::copy(bytes->begin(), bytes->end(), key.begin());
    return key;
}

} // namespace

Server::Server(ServerOptions options)
    : options_(std::move(options)),
      store_(options_.store_path),
      accounts_(AccountStoreOptions{.path = options_.account_path, .argon2 = options_.argon2}),
      sessions_(SessionStoreOptions{.ttl = options_.session_ttl}) {}

Server::~Server() {
    Stop();
}

bool Server::LoadOrCreateIdentity() {
    if (!options_.identity_path.empty()) {
        std::ifstream stream(options_.identity_path);
        if (stream) {
            std::string text;
            stream >> text;
            if (const std::optional<VulkanEngine::Security::X25519Key> secret = ParseHexKey(text);
                secret.has_value()) {
                identity_.secret = *secret;
                identity_.public_key = VulkanEngine::Security::X25519PublicFromSecret(*secret);
                return true;
            }
        }
    }

    identity_ = VulkanEngine::Security::GenerateX25519KeyPair();
    // A degenerate keypair means the random source failed; the handshake would
    // silently produce a predictable secret, so refuse it.
    bool secret_nonzero = false;
    for (const std::byte byte : identity_.secret) {
        secret_nonzero = secret_nonzero || byte != std::byte{0};
    }
    if (!secret_nonzero) {
        return false;
    }

    if (!options_.identity_path.empty()) {
        std::error_code ec;
        if (!options_.identity_path.parent_path().empty()) {
            std::filesystem::create_directories(options_.identity_path.parent_path(), ec);
        }
        std::ofstream out(options_.identity_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            // Not fatal: the identity still works for this process, clients just
            // have to re-pin after every restart.
            LogMessage(LogLevel::Warn,
                       std::format("server: cannot persist the identity to {}",
                                   options_.identity_path.string()));
            return true;
        }
        out << TokenToHex(identity_.secret) << '\n';
        if (out.fail()) {
            LogMessage(LogLevel::Warn,
                       std::format("server: failed writing the identity to {}",
                                   options_.identity_path.string()));
        }
    }
    return true;
}

bool Server::Start() {
    std::optional<TcpListener> listener = TcpListener::Bind(options_.port);
    if (!listener.has_value()) {
        return false;
    }
    listener_ = std::move(*listener);
    // A server without a usable identity cannot perform the v2 handshake, so a
    // failure here is fatal rather than a silent fallback.
    if (!LoadOrCreateIdentity()) {
        LogMessage(LogLevel::Error, "server: could not load or create a server identity");
        listener_.Close();
        return false;
    }
    running_.store(true);
    LogMessage(LogLevel::Info, std::format("server: listening on port {}", listener_.BoundPort()));
    LogMessage(LogLevel::Info, std::format("server: identity public key {}",
                                           TokenToHex(identity_.public_key).substr(0, 16)));
    if (!options_.account_path.empty()) {
        LogMessage(LogLevel::Info,
                   std::format("server: accounts at {}", options_.account_path.string()));
    }
    if (options_.store_path.empty()) {
        LogMessage(LogLevel::Warn,
                   "server: scores are in memory only (ephemeral) and will be lost on exit");
    } else {
        LogMessage(LogLevel::Info,
                   std::format("server: scores at {}", options_.store_path.string()));
    }
    return true;
}

void Server::Stop() {
    running_.store(false);
    listener_.Close();

    // Let every in-flight handler finish before the stores and identity are
    // destroyed underneath it.
    std::vector<std::future<void>> pending;
    {
        std::lock_guard lock(clients_mutex_);
        pending.swap(clients_);
    }
    for (std::future<void>& client : pending) {
        if (client.valid()) {
            client.wait();
        }
    }
}

void Server::Run() {
    while (running_.load()) {
        std::optional<TcpSocket> socket = listener_.Accept(std::chrono::milliseconds(200));
        if (socket.has_value()) {
            try {
                std::lock_guard lock(clients_mutex_);
                clients_.push_back(std::async(
                    std::launch::async, [this, client = std::move(*socket)]() mutable {
                        HandleClient(std::move(client));
                    }));
            } catch (const std::exception& error) {
                // Thread creation failed; drop this connection rather than
                // taking the accept loop down with it.
                LogMessage(LogLevel::Error,
                           std::format("server: cannot start a connection handler: {}",
                                       error.what()));
                socket->Close();
            }
        }

        // Reap completed connections so the list cannot grow without bound.
        std::lock_guard lock(clients_mutex_);
        std::erase_if(clients_, [](std::future<void>& client) {
            return client.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        });
    }
}

void Server::HandleClient(TcpSocket socket) {
    std::vector<std::byte> frame;
    if (!RecvFrame(socket, frame) || frame.size() < kHeaderSize) {
        LogMessage(LogLevel::Warn, "server: client sent no readable handshake");
        return;
    }
    const std::optional<FrameHeader> hello_header = DecodeHeader(frame);
    if (!hello_header.has_value() || hello_header->type != MessageType::Hello) {
        LogMessage(LogLevel::Warn,
                   std::format("server: expected a hello, got {}",
                               hello_header.has_value() ? ToString(hello_header->type) : "a bad header"));
        return;
    }

    // ── Negotiate version, cipher and (v2) key agreement ──
    CipherVariant variant = CipherVariant::XChaCha20Poly1305;
    std::vector<std::byte> session_key;
    std::uint16_t negotiated_proto = kProtocolMinVersion;
    std::uint64_t config_hash = hello_header->config_hash;
    std::vector<std::byte> client_nonce;
    std::vector<std::byte> server_nonce;
    std::vector<std::byte> hello_ack_payload;

    if (hello_header->proto >= 2) {
        const std::optional<HelloV2Message> hello =
            DecodeHelloV2(std::span<const std::byte>(frame).subspan(kHeaderSize));
        if (!hello.has_value()) {
            return;
        }
        config_hash = hello->config_hash;
        client_nonce.assign(hello->client_nonce.begin(), hello->client_nonce.end());
        server_nonce = VulkanEngine::Security::RandomBytes(kNonceSize);

        const std::optional<CipherVariant> picked = PickCipher(hello->cipher_mask);
        if (!picked.has_value()) {
            (void)SendError(socket, 2, config_hash, 0, 3, "no common cipher", {}, variant, false);
            return;
        }
        variant = *picked;

        VulkanEngine::Security::X25519Key client_public{};
        std::copy(hello->client_public_key.begin(), hello->client_public_key.end(), client_public.begin());
        const std::optional<VulkanEngine::Security::X25519Key> shared =
            VulkanEngine::Security::X25519SharedSecret(identity_.secret, client_public);
        if (!shared.has_value()) {
            return;
        }

        negotiated_proto = std::min<std::uint16_t>(2, hello->max_proto);
        LogMessage(LogLevel::Info,
                   std::format("server: v2 handshake (proto {}, cipher {}, config {:#x})",
                               negotiated_proto, static_cast<int>(variant), config_hash));
        HelloAckV2Message ack{};
        ack.proto = negotiated_proto;
        ack.cipher_variant = static_cast<std::uint16_t>(variant);
        ack.key_id = kSessionKeyId;
        std::copy(server_nonce.begin(), server_nonce.end(), ack.server_nonce.begin());
        std::copy(identity_.public_key.begin(), identity_.public_key.end(), ack.server_public_key.begin());
        hello_ack_payload = Encode(ack);
        session_key = DeriveX25519SessionKey(*shared, client_nonce, server_nonce, config_hash);
    } else {
        const std::optional<HelloMessage> hello =
            DecodeHello(std::span<const std::byte>(frame).subspan(kHeaderSize));
        if (!hello.has_value()) {
            return;
        }
        config_hash = hello->config_hash;
        client_nonce.assign(hello->client_nonce.begin(), hello->client_nonce.end());
        server_nonce = VulkanEngine::Security::RandomBytes(kNonceSize);

        const std::optional<CipherVariant> picked = PickCipher(hello->cipher_mask);
        if (!picked.has_value()) {
            (void)SendError(socket, 1, config_hash, 0, 3, "no common cipher", {}, variant, false);
            return;
        }
        variant = *picked;
        negotiated_proto = std::min<std::uint16_t>(1, hello->max_proto);
        LogMessage(LogLevel::Info,
                   std::format("server: v1 handshake (proto {}, cipher {}, config {:#x})",
                               negotiated_proto, static_cast<int>(variant), config_hash));

        HelloAckMessage ack{};
        ack.proto = negotiated_proto;
        ack.cipher_variant = static_cast<std::uint16_t>(variant);
        ack.key_id = kSessionKeyId;
        std::copy(server_nonce.begin(), server_nonce.end(), ack.server_nonce.begin());
        hello_ack_payload = Encode(ack);
        session_key = DerivePskSessionKey(options_.psk, client_nonce, server_nonce, config_hash);
    }

    if (session_key.size() != 32) {
        return;
    }
    const FrameHeader ack_header{negotiated_proto, MessageType::HelloAck, 0, 0, config_hash,
                                 static_cast<std::uint32_t>(hello_ack_payload.size())};
    if (!SendMessage(socket, ack_header, hello_ack_payload)) {
        return;
    }

    if (!options_.accept_unknown_configs && config_hash != CurrentBalanceHash()) {
        (void)SendError(socket, negotiated_proto, config_hash, 0, 2, "unknown ruleset", session_key,
                        variant, true);
        return;
    }

    ConnectionState connection{};
    connection.proto = negotiated_proto;

    // ── Serve requests ──
    LogMessage(LogLevel::Debug, "server: session established, serving requests");
    while (running_.load()) {
        if (!RecvFrame(socket, frame) || frame.size() < kHeaderSize) {
            LogMessage(LogLevel::Debug, "server: client disconnected");
            return;
        }
        const std::optional<FrameHeader> header = DecodeHeader(frame);
        if (!header.has_value() || header->config_hash != config_hash) {
            LogMessage(LogLevel::Warn, "server: dropping a frame with a bad header or ruleset");
            return;
        }
        const std::span<const std::byte> sealed_payload =
            std::span<const std::byte>(frame).subspan(kHeaderSize);
        const std::optional<std::vector<std::byte>> plain =
            OpenMessage(session_key, variant, header->seq,
                        std::span<const std::byte>(frame).subspan(0, kAadSize), sealed_payload);
        if (!plain.has_value()) {
            LogMessage(LogLevel::Warn,
                       std::format("server: cannot decrypt {} (seq {})", ToString(header->type),
                                   header->seq));
            return;
        }
        LogMessage(LogLevel::Debug,
                   std::format("server: <- {} (seq {}, {} bytes)", ToString(header->type),
                               header->seq, plain->size()));

        const auto reply = [&](MessageType type, std::span<const std::byte> inner) {
            return SendEncrypted(socket, session_key, variant, connection.proto, type, config_hash,
                                 header->seq, inner);
        };
        const auto fail = [&](std::string_view text) {
            return SendEncrypted(socket, session_key, variant, connection.proto, MessageType::Error,
                                 config_hash, header->seq, Encode(ErrorMessage{0, std::string{text}}));
        };

        switch (header->type) {
            case MessageType::RegisterRequest: {
                const std::optional<RegisterRequestMessage> request = DecodeRegisterRequest(*plain);
                if (!request.has_value()) {
                    return;
                }
                const AccountStore::RegisterResult result =
                    accounts_.Register(request->username, request->display_name);
                LogMessage(result.status == SyncStatus::Ok ? LogLevel::Info : LogLevel::Warn,
                           std::format("server: register '{}' -> {}", request->username,
                                       ToString(result.status)));
                RegisterReplyMessage reply_message{};
                reply_message.status = result.status;
                reply_message.user_id = result.user_id;
                reply_message.token = result.token;
                reply_message.display_name = result.display_name;
                if (result.status == SyncStatus::Ok) {
                    connection.authenticated = true;
                    connection.account = AccountInfo{result.user_id, request->username, result.display_name};
                    if (const auto settings = accounts_.SettingsOf(result.user_id); settings.has_value()) {
                        connection.settings = *settings;
                    }
                }
                if (!reply(MessageType::RegisterReply, Encode(reply_message))) {
                    return;
                }
                break;
            }
            case MessageType::LoginRequest: {
                const std::optional<LoginRequestMessage> request = DecodeLoginRequest(*plain);
                if (!request.has_value()) {
                    return;
                }
                const AccountStore::LoginResult result = accounts_.Login(request->username, request->token);
                LogMessage(result.status == SyncStatus::Ok ? LogLevel::Info : LogLevel::Warn,
                           std::format("server: login '{}' -> {}", request->username,
                                       ToString(result.status)));
                LoginReplyMessage reply_message{};
                reply_message.status = result.status;
                if (result.status == SyncStatus::Ok) {
                    const SessionStore::Issued session = sessions_.Issue(result.account.id);
                    reply_message.account = result.account;
                    reply_message.settings = result.settings;
                    reply_message.session_token = session.token;
                    reply_message.session_expires_at = session.expires_at;
                    connection.authenticated = true;
                    connection.account = result.account;
                    connection.settings = result.settings;
                }
                if (!reply(MessageType::LoginReply, Encode(reply_message))) {
                    return;
                }
                break;
            }
            case MessageType::ResumeRequest: {
                const std::optional<ResumeRequestMessage> request = DecodeResumeRequest(*plain);
                if (!request.has_value()) {
                    return;
                }
                LoginReplyMessage reply_message{};
                const std::optional<UserId> user_id = sessions_.Resolve(request->session_token);
                LogMessage(user_id.has_value() ? LogLevel::Debug : LogLevel::Warn,
                           user_id.has_value()
                               ? std::format("server: resumed session for account {}", *user_id)
                               : std::string{"server: resume rejected (unknown or expired session)"});
                if (!user_id.has_value()) {
                    reply_message.status = SyncStatus::InvalidToken;
                } else if (const std::optional<AccountInfo> account = accounts_.FindById(*user_id);
                           !account.has_value()) {
                    reply_message.status = SyncStatus::UnknownAccount;
                } else {
                    const std::optional<SyncedSettings> settings = accounts_.SettingsOf(*user_id);
                    reply_message.status = SyncStatus::Ok;
                    reply_message.account = *account;
                    reply_message.settings = settings.value_or(SyncedSettings{});
                    const SessionStore::Issued session = sessions_.Issue(*user_id);
                    reply_message.session_token = session.token;
                    reply_message.session_expires_at = session.expires_at;
                    connection.authenticated = true;
                    connection.account = *account;
                    connection.settings = reply_message.settings;
                }
                if (!reply(MessageType::LoginReply, Encode(reply_message))) {
                    return;
                }
                break;
            }
            case MessageType::SettingsUpdate: {
                const std::optional<SettingsUpdateMessage> request = DecodeSettingsUpdate(*plain);
                if (!request.has_value()) {
                    return;
                }
                SettingsReplyMessage reply_message{};
                if (!connection.authenticated) {
                    LogMessage(LogLevel::Warn, "server: settings update without a signed-in account");
                    reply_message.status = SyncStatus::NotAuthenticated;
                } else {
                    const AccountStore::UpdateResult result = accounts_.UpdateSettings(
                        connection.account.id, request->display_name, request->settings);
                    LogMessage(result.status == SyncStatus::Ok ? LogLevel::Info : LogLevel::Warn,
                               std::format("server: settings for '{}' -> {}",
                                           connection.account.username, ToString(result.status)));
                    reply_message.status = result.status;
                    if (result.status == SyncStatus::Ok) {
                        reply_message.account = result.account;
                        reply_message.settings = result.settings;
                        connection.account = result.account;
                        connection.settings = result.settings;
                    }
                }
                if (!reply(MessageType::SettingsReply, Encode(reply_message))) {
                    return;
                }
                break;
            }
            case MessageType::Submit: {
                const std::optional<SubmitMessage> submit = DecodeSubmit(*plain);
                if (!submit.has_value()) {
                    return;
                }
                if (!connection.authenticated) {
                    LogMessage(LogLevel::Warn,
                               std::format("server: rejected score {} from an anonymous connection",
                                           submit->score));
                    if (!fail("not authenticated")) {
                        return;
                    }
                    break;
                }
                const AccountStore::ScoreResult recorded =
                    accounts_.RecordRun(connection.account.id, submit->run_id);
                if (recorded == AccountStore::ScoreResult::UnknownAccount) {
                    LogMessage(LogLevel::Warn,
                               std::format("server: score for unknown account {}", connection.account.id));
                    if (!fail("unknown account")) {
                        return;
                    }
                    break;
                }
                SubmitAckMessage ack{};
                if (recorded == AccountStore::ScoreResult::Accepted) {
                    const std::string shown =
                        connection.settings.show_on_leaderboard ? connection.account.display_name
                                                                : std::string{};
                    const ScoreStore::SubmitResult stored =
                        store_.Submit(config_hash, submit->score, shown, connection.account.id,
                                      UnixNowSeconds());
                    LogMessage(LogLevel::Info,
                               std::format("server: score {} from '{}' accepted (rank {}/{})",
                                           submit->score, connection.account.username, stored.rank,
                                           stored.total));
                    ack.rank = stored.rank;
                    ack.total = stored.total;
                    ack.best = stored.best;
                } else {
                    LogMessage(LogLevel::Debug,
                               std::format("server: duplicate run {} from '{}' ignored", submit->run_id,
                                           connection.account.username));
                    // Duplicate: report the current board state without recording twice.
                    const std::vector<ScoreEntry> top = store_.Top(config_hash, 1);
                    ack.best = top.empty() ? 0 : top.front().score;
                    ack.total = static_cast<std::int32_t>(store_.Size(config_hash));
                    ack.rank = 1;
                }
                if (!reply(MessageType::SubmitAck, Encode(ack))) {
                    return;
                }
                break;
            }
            case MessageType::TopRequest: {
                const std::optional<TopRequestMessage> request = DecodeTopRequest(*plain);
                if (!request.has_value()) {
                    return;
                }
                TopOptions options{};
                options.best_per_account = request->best_per_account;
                options.since = request->since;
                const std::vector<ScoreEntry> top =
                    store_.Top(config_hash, std::min<std::size_t>(request->count, 100), options);
                LogMessage(LogLevel::Debug,
                           std::format("server: top {} for {:#x} ({} rows, best-per-account {}, since {})",
                                       request->count, config_hash, top.size(),
                                       request->best_per_account, request->since));
                TopReplyMessage reply_message{};
                reply_message.entries.reserve(top.size());
                for (const ScoreEntry& entry : top) {
                    reply_message.entries.push_back(TopEntry{entry.rank, entry.score,
                                                             entry.display_name, entry.user_id,
                                                             entry.recorded_at});
                }
                if (!reply(MessageType::TopReply, Encode(reply_message))) {
                    return;
                }
                break;
            }
            default:
                LogMessage(LogLevel::Warn,
                           std::format("server: unexpected message {}", ToString(header->type)));
                return;
        }
    }
}

} // namespace Examples::InfiniteRunner::Leaderboard
