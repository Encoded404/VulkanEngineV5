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
    return SendFrame(socket, frame) == IoStatus::Ok;
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
      store_(ScoreStoreOptions{.directory = options_.store_path,
                               .legacy_file = options_.legacy_store_path,
                               .max_scores_per_account = options_.max_scores_per_account}),
      accounts_(AccountStoreOptions{.path = options_.account_path,
                                    .argon2 = options_.argon2,
                                    .name_policy = options_.name_policy}),
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

bool Server::ConfigAccepted(std::uint64_t config_hash) const {
    if (options_.accept_unknown_configs) {
        return true;
    }
    if (config_hash == CurrentBalanceHash()) {
        return true;
    }
    return std::find(options_.accepted_configs.begin(), options_.accepted_configs.end(),
                     config_hash) != options_.accepted_configs.end();
}

bool Server::Start() {
    std::optional<TcpListener> listener = TcpListener::Bind(options_.port);
    if (!listener.has_value()) {
        return false;
    }
    listener_ = std::move(*listener);
    // A server without a usable identity cannot perform the secure handshake, so
    // a failure here is fatal rather than a silent fallback.
    if (!LoadOrCreateIdentity()) {
        LogMessage(LogLevel::Error, "server: could not load or create a server identity");
        listener_.Close();
        return false;
    }
    if (!options_.accept_unknown_configs) {
        const std::uint64_t current = CurrentBalanceHash();
        const bool listed = std::find(options_.accepted_configs.begin(),
                                      options_.accepted_configs.end(),
                                      current) != options_.accepted_configs.end();
        if (!listed) {
            LogMessage(LogLevel::Error,
                       std::format("server: current ruleset {:#x} is not in the accepted-configs "
                                   "list; add it before starting",
                                   current));
            listener_.Close();
            return false;
        }
    }
    running_.store(true);
    LogMessage(LogLevel::Info, std::format("server: listening on port {}", listener_.BoundPort()));
    LogMessage(LogLevel::Info, std::format("server: identity public key {}",
                                           TokenToHex(identity_.public_key).substr(0, 16)));
    LogMessage(LogLevel::Info,
               std::format("server: accepting {} ruleset(s), up to {} scores per account",
                           options_.accept_unknown_configs ? std::string_view{"all"}
                                                           : options_.accepted_configs.size() == 0
                                                                 ? std::string_view{"current only"}
                                                                 : std::string_view{"listed"},
                           options_.max_scores_per_account));
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
    // destroyed underneath it. A connection accepted just before the listener
    // closed may still be starting, so drain repeatedly rather than once.
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::vector<std::future<void>> pending;
        {
            std::lock_guard lock(clients_mutex_);
            pending.swap(clients_);
        }
        if (pending.empty()) {
            break;
        }
        for (std::future<void>& client : pending) {
            if (client.valid()) {
                client.wait();
            }
        }
    }
    // Persist whatever the timer had not yet written.
    store_.Flush();
}

void Server::Run() {
    auto last_flush = std::chrono::steady_clock::now();
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

        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= options_.flush_interval) {
            store_.Flush();
            last_flush = now;
        }
    }
}

IoStatus Server::RecvFrameWithIdle(TcpSocket& socket, std::vector<std::byte>& frame,
                                   std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        if (!running_.load()) {
            return IoStatus::Disconnected;
        }
        const IoStatus status = RecvFrame(socket, frame);
        if (status != IoStatus::Timeout) {
            return status;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return IoStatus::Timeout;
        }
    }
}

void Server::HandleClient(TcpSocket socket) {
    // The recv timeout is the poll tick, not the idle budget: RecvFrameWithIdle
    // is what enforces the deadline, so a timeout there means "still healthy".
    socket.SetTimeouts(options_.socket_poll_interval, options_.send_timeout);

    std::vector<std::byte> frame;
    const auto handshake_deadline = std::chrono::steady_clock::now() + options_.idle_timeout;
    const IoStatus handshake_status = RecvFrameWithIdle(socket, frame, handshake_deadline);
    if (handshake_status == IoStatus::Timeout) {
        LogMessage(LogLevel::Debug, "server: closing a connection idle before the handshake");
        return;
    }
    if (handshake_status != IoStatus::Ok || frame.size() < kHeaderSize) {
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
    if (hello_header->proto < kProtocolMinVersion || hello_header->proto > kProtocolVersion) {
        LogMessage(LogLevel::Warn,
                   std::format("server: client speaks v{}, server supports {}-{}",
                               hello_header->proto, kProtocolMinVersion, kProtocolVersion));
        (void)SendError(socket, kProtocolMinVersion, hello_header->config_hash, 0, 1,
                        std::format("unsupported protocol v{}; server speaks {}-{}",
                                    hello_header->proto, kProtocolMinVersion, kProtocolVersion),
                        {}, CipherVariant::XChaCha20Poly1305, false);
        return;
    }

    const std::optional<HelloSecureMessage> hello =
        DecodeHello(std::span<const std::byte>(frame).subspan(kHeaderSize));
    if (!hello.has_value()) {
        return;
    }
    const std::uint64_t config_hash = hello->config_hash;

    // ── Negotiate cipher, version and key agreement ──
    CipherVariant variant = CipherVariant::XChaCha20Poly1305;
    std::vector<std::byte> session_key;
    const std::uint16_t negotiated_proto =
        std::min<std::uint16_t>(kProtocolVersion, hello->max_proto);
    const std::vector<std::byte> client_nonce(hello->client_nonce.begin(),
                                              hello->client_nonce.end());
    const std::vector<std::byte> server_nonce = VulkanEngine::Security::RandomBytes(kNonceSize);

    // Reject an unknown ruleset before the key exchange: the client then fails
    // inside its handshake loop (which backs off) and reads an actionable
    // reason, and the server skips the expensive X25519 work.
    if (!ConfigAccepted(config_hash)) {
        LogMessage(LogLevel::Warn,
                   std::format("server: rejected unknown ruleset {:#x}", config_hash));
        (void)SendError(socket, negotiated_proto, config_hash, 0, 2, "unknown ruleset", {}, variant,
                        false);
        return;
    }

    const std::optional<CipherVariant> picked = PickCipher(hello->cipher_mask);
    if (!picked.has_value()) {
        (void)SendError(socket, negotiated_proto, config_hash, 0, 3, "no common cipher", {}, variant,
                        false);
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
    LogMessage(LogLevel::Info,
               std::format("server: handshake (proto {}, cipher {}, config {:#x})",
                           negotiated_proto, static_cast<int>(variant), config_hash));

    HelloAckSecureMessage ack{};
    ack.proto = negotiated_proto;
    ack.cipher_variant = static_cast<std::uint16_t>(variant);
    ack.key_id = kSessionKeyId;
    std::copy(server_nonce.begin(), server_nonce.end(), ack.server_nonce.begin());
    std::copy(identity_.public_key.begin(), identity_.public_key.end(), ack.server_public_key.begin());
    const std::vector<std::byte> hello_ack_payload = Encode(ack);
    session_key = DeriveX25519SessionKey(*shared, client_nonce, server_nonce, config_hash);
    if (session_key.size() != 32) {
        return;
    }
    const FrameHeader ack_header{negotiated_proto, MessageType::HelloAck, 0, 0, config_hash,
                                 static_cast<std::uint32_t>(hello_ack_payload.size())};
    if (!SendMessage(socket, ack_header, hello_ack_payload)) {
        return;
    }

    ConnectionState connection{};
    connection.proto = negotiated_proto;

    // Simple per-connection token bucket. A zero refill disables it.
    const bool rate_limited = options_.rate_limit_refill.count() > 0;
    double tokens = static_cast<double>(options_.rate_limit_burst);
    auto last_refill = std::chrono::steady_clock::now();

    // ── Serve requests ──
    LogMessage(LogLevel::Debug, "server: session established, serving requests");
    auto idle_deadline = std::chrono::steady_clock::now() + options_.idle_timeout;
    while (running_.load()) {
        const IoStatus receive_status = RecvFrameWithIdle(socket, frame, idle_deadline);
        if (receive_status == IoStatus::Timeout) {
            LogMessage(LogLevel::Debug,
                       std::format("server: closing a connection idle for {}s",
                                   options_.idle_timeout.count()));
            return;
        }
        if (receive_status != IoStatus::Ok || frame.size() < kHeaderSize) {
            LogMessage(LogLevel::Debug, "server: client disconnected");
            return;
        }
        // Any frame counts as activity, including a keepalive Ping.
        idle_deadline = std::chrono::steady_clock::now() + options_.idle_timeout;
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

        if (rate_limited) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - last_refill;
            if (elapsed >= options_.rate_limit_refill) {
                const auto steps = elapsed / options_.rate_limit_refill;
                tokens = std::min<double>(
                    tokens + static_cast<double>(steps),
                    static_cast<double>(options_.rate_limit_burst));
                last_refill += options_.rate_limit_refill * steps;
            }
            if (tokens < 1.0) {
                LogMessage(LogLevel::Warn, "server: rate limited a connection");
                (void)fail("rate limited");
                return;
            }
            tokens -= 1.0;
        }

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
                if (result.status == SyncStatus::Ok) {
                    connection.authenticated = true;
                    connection.account =
                        AccountInfo{result.user_id, request->username, result.display_name};
                    connection.hidden = false;
                }
                if (connection.proto >= 3) {
                    RegisterReplyV3Message reply_message{};
                    reply_message.status = result.status;
                    reply_message.reason = result.reason;
                    reply_message.user_id = result.user_id;
                    reply_message.token = result.token;
                    reply_message.display_name = result.display_name;
                    if (!reply(MessageType::RegisterReplyV3, Encode(reply_message))) {
                        return;
                    }
                } else {
                    RegisterReplyMessage reply_message{};
                    reply_message.status = result.status;
                    reply_message.user_id = result.user_id;
                    reply_message.token = result.token;
                    reply_message.display_name = result.display_name;
                    if (!reply(MessageType::RegisterReply, Encode(reply_message))) {
                        return;
                    }
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
                    reply_message.show_on_leaderboard = !accounts_.IsHidden(result.account.id);
                    reply_message.session_token = session.token;
                    reply_message.session_expires_at = session.expires_at;
                    connection.authenticated = true;
                    connection.account = result.account;
                    connection.hidden = !reply_message.show_on_leaderboard;
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
                    reply_message.status = SyncStatus::Ok;
                    reply_message.account = *account;
                    reply_message.show_on_leaderboard = !accounts_.IsHidden(*user_id);
                    const SessionStore::Issued session = sessions_.Issue(*user_id);
                    reply_message.session_token = session.token;
                    reply_message.session_expires_at = session.expires_at;
                    connection.authenticated = true;
                    connection.account = *account;
                    connection.hidden = !reply_message.show_on_leaderboard;
                }
                if (!reply(MessageType::LoginReply, Encode(reply_message))) {
                    return;
                }
                break;
            }
            case MessageType::SettingsUpdate: {
                // Legacy v2 rename + visibility. A v3 client never sends this.
                const std::optional<SettingsUpdateMessage> request = DecodeSettingsUpdate(*plain);
                if (!request.has_value()) {
                    return;
                }
                SettingsReplyMessage reply_message{};
                if (!connection.authenticated) {
                    LogMessage(LogLevel::Warn, "server: settings update without a signed-in account");
                    reply_message.status = SyncStatus::NotAuthenticated;
                } else {
                    const AccountStore::UpdateResult result = accounts_.UpdateLegacy(
                        connection.account.id, request->display_name, request->show_on_leaderboard);
                    LogMessage(result.status == SyncStatus::Ok ? LogLevel::Info : LogLevel::Warn,
                               std::format("server: legacy settings for '{}' -> {}",
                                           connection.account.username, ToString(result.status)));
                    reply_message.status = result.status;
                    if (result.status == SyncStatus::Ok) {
                        reply_message.account = result.account;
                        reply_message.show_on_leaderboard = request->show_on_leaderboard;
                        connection.account = result.account;
                        connection.hidden = !request->show_on_leaderboard;
                    }
                }
                if (!reply(MessageType::SettingsReply, Encode(reply_message))) {
                    return;
                }
                break;
            }
            case MessageType::RenameRequest: {
                const std::optional<RenameRequestMessage> request = DecodeRenameRequest(*plain);
                if (!request.has_value()) {
                    return;
                }
                RenameReplyV3Message reply_message{};
                if (!connection.authenticated) {
                    LogMessage(LogLevel::Warn, "server: rename without a signed-in account");
                    reply_message.status = SyncStatus::NotAuthenticated;
                    reply_message.reason = "not authenticated";
                } else {
                    const AccountStore::UpdateResult result =
                        accounts_.Rename(connection.account.id, request->display_name);
                    LogMessage(result.status == SyncStatus::Ok ? LogLevel::Info : LogLevel::Warn,
                               std::format("server: rename '{}' -> {}", connection.account.username,
                                           ToString(result.status)));
                    reply_message.status = result.status;
                    reply_message.reason = result.reason;
                    reply_message.account = result.account;
                    if (result.status == SyncStatus::Ok) {
                        connection.account = result.account;
                    }
                }
                if (!reply(MessageType::RenameReplyV3, Encode(reply_message))) {
                    return;
                }
                break;
            }
            case MessageType::Ping: {
                const std::optional<PingMessage> request = DecodePing(*plain);
                if (!request.has_value()) {
                    LogMessage(LogLevel::Warn, "server: malformed ping");
                    return;
                }
                LogMessage(LogLevel::Trace, std::format("server: ping (token {:#x})", request->token));
                if (!reply(MessageType::Pong, Encode(PongMessage{request->token}))) {
                    return;
                }
                break;
            }
            case MessageType::Submit:
            case MessageType::SubmitV3: {
                std::uint64_t run_id = 0;
                std::int32_t score = 0;
                bool best_per_account = false;
                const bool v3 = header->type == MessageType::SubmitV3;
                if (v3) {
                    const std::optional<SubmitV3Message> submit = DecodeSubmitV3(*plain);
                    if (!submit.has_value()) {
                        return;
                    }
                    run_id = submit->run_id;
                    score = submit->score;
                    best_per_account = submit->best_per_account;
                } else {
                    const std::optional<SubmitMessage> submit = DecodeSubmit(*plain);
                    if (!submit.has_value()) {
                        return;
                    }
                    run_id = submit->run_id;
                    score = submit->score;
                }
                if (!connection.authenticated) {
                    LogMessage(LogLevel::Warn,
                               std::format("server: rejected score {} from an anonymous connection",
                                           score));
                    if (!fail("not authenticated")) {
                        return;
                    }
                    break;
                }
                const AccountStore::ScoreResult recorded =
                    accounts_.RecordRun(connection.account.id, run_id);
                if (recorded == AccountStore::ScoreResult::UnknownAccount) {
                    LogMessage(LogLevel::Warn,
                               std::format("server: score for unknown account {}", connection.account.id));
                    if (!fail("unknown account")) {
                        return;
                    }
                    break;
                }
                ScoreStore::RankInfo ranks{};
                if (recorded == AccountStore::ScoreResult::Accepted && !connection.hidden) {
                    const ScoreStore::SubmitResult stored =
                        store_.Submit(config_hash, score, connection.account.id, UnixNowSeconds());
                    ranks = ScoreStore::RankInfo{stored.rank_runs, stored.rank_accounts, stored.best};
                    LogMessage(LogLevel::Info,
                               std::format("server: score {} from '{}' accepted (rank {}/{})", score,
                                           connection.account.username, stored.rank_runs,
                                           stored.rank_accounts));
                } else {
                    // Duplicate run, or a legacy v2 client that hid itself:
                    // acknowledge without recording.
                    const std::optional<ScoreStore::RankInfo> info =
                        store_.RankOf(config_hash, connection.account.id);
                    if (info.has_value()) {
                        ranks = *info;
                    }
                    LogMessage(LogLevel::Debug,
                               std::format("server: run {} from '{}' not recorded (hidden or duplicate)",
                                           run_id, connection.account.username));
                }
                if (v3) {
                    SubmitAckV3Message ack{};
                    ack.rank_runs = ranks.rank_runs;
                    ack.rank_accounts = ranks.rank_accounts;
                    ack.best = ranks.best;
                    (void)best_per_account; // both standings travel; the UI picks.
                    if (!reply(MessageType::SubmitAckV3, Encode(ack))) {
                        return;
                    }
                } else {
                    SubmitAckMessage ack{};
                    ack.rank = ranks.rank_runs;
                    ack.total = static_cast<std::int32_t>(store_.Size(config_hash));
                    ack.best = ranks.best;
                    if (!reply(MessageType::SubmitAck, Encode(ack))) {
                        return;
                    }
                }
                break;
            }
            case MessageType::TopRequest:
            case MessageType::TopRequestV3: {
                std::size_t count = 10;
                bool best_per_account = false;
                std::uint64_t since = 0;
                bool only_mine = false;
                if (header->type == MessageType::TopRequestV3) {
                    const std::optional<TopRequestV3Message> request = DecodeTopRequestV3(*plain);
                    if (!request.has_value()) {
                        return;
                    }
                    count = request->count;
                    best_per_account = request->best_per_account;
                    since = request->since;
                    only_mine = request->only_mine;
                } else {
                    const std::optional<TopRequestMessage> request = DecodeTopRequest(*plain);
                    if (!request.has_value()) {
                        return;
                    }
                    count = request->count;
                    best_per_account = request->best_per_account;
                    since = request->since;
                }
                if (only_mine && !connection.authenticated) {
                    if (!fail("not authenticated")) {
                        return;
                    }
                    break;
                }
                TopOptions options{};
                options.best_per_account = best_per_account;
                options.since = since;
                options.only_user = only_mine ? connection.account.id : 0;
                const std::vector<ScoreEntry> top =
                    store_.Top(config_hash, std::min<std::size_t>(count, 100), options);
                LogMessage(LogLevel::Debug,
                           std::format("server: top {} for {:#x} ({} rows, best-per-account {}, "
                                       "since {}, only-mine {})",
                                       count, config_hash, top.size(), best_per_account, since,
                                       only_mine));
                TopReplyMessage reply_message{};
                reply_message.entries.reserve(top.size());
                for (const ScoreEntry& entry : top) {
                    // Names are resolved live, so a rename is reflected on every
                    // historical row and a hidden legacy account stays nameless.
                    const std::string name =
                        accounts_.ShownName(entry.user_id).value_or(std::string{});
                    reply_message.entries.push_back(TopEntry{entry.rank, entry.score, name,
                                                             entry.user_id, entry.recorded_at});
                }
                if (!reply(MessageType::TopReply, Encode(reply_message))) {
                    return;
                }
                break;
            }
            default:
                // The session key is established, so the peer can be told why
                // it is about to be dropped instead of just seeing EOF.
                LogMessage(LogLevel::Warn,
                           std::format("server: unsupported message {}", ToString(header->type)));
                (void)fail("unsupported message");
                return;
        }
    }
}

} // namespace Examples::InfiniteRunner::Leaderboard
