module;

module Examples.InfiniteRunner.Leaderboard.Client;

import std;

import VulkanEngine.DataCipher;
import VulkanEngine.KeyExchange;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.Log;
import Examples.InfiniteRunner.Leaderboard.Protocol;
import Examples.InfiniteRunner.Leaderboard.SessionCrypto;
import Examples.InfiniteRunner.Leaderboard.Transport;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

using VulkanEngine::Security::CipherVariant;

[[nodiscard]] bool SendCleartext(TcpSocket& socket, const FrameHeader& header,
                                 std::span<const std::byte> payload) {
    const std::array<std::byte, kHeaderSize> header_bytes = EncodeHeader(header);
    std::vector<std::byte> frame(header_bytes.begin(), header_bytes.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return SendFrame(socket, frame);
}

[[nodiscard]] std::uint64_t RandomRunId() {
    const std::vector<std::byte> bytes = VulkanEngine::Security::RandomBytes(8);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i])) << (8 * i);
    }
    return value;
}

template <typename T>
[[nodiscard]] bool CopyArray(const T& source, auto& destination) {
    if (source.size() != destination.size()) {
        return false;
    }
    std::copy(source.begin(), source.end(), destination.begin());
    return true;
}

} // namespace

Client::Client(ClientOptions options) : options_(std::move(options)) {}

Client::~Client() {
    Stop();
}

void Client::Start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&Client::WorkerLoop, this);
}

void Client::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Client::SetCredentials(std::string username, std::string token_hex) {
    std::lock_guard lock(mutex_);
    username_ = std::move(username);
    token_ = std::move(token_hex);
    session_token_.clear();
    session_expires_at_ = 0;
}

void Client::ClearCredentials() {
    std::lock_guard lock(mutex_);
    username_.clear();
    token_.clear();
}

void Client::SetSession(std::string session_token, std::uint64_t expires_at) {
    std::lock_guard lock(mutex_);
    session_token_ = std::move(session_token);
    session_expires_at_ = expires_at;
}

void Client::ForgetSession() {
    std::lock_guard lock(mutex_);
    session_token_.clear();
    session_expires_at_ = 0;
}

void Client::RequestReconnect() {
    reconnect_.store(true);
    condition_.notify_all();
}

std::uint64_t Client::SubmitScore(std::int32_t score) {
    Request request;
    request.kind = RequestKind::Submit;
    request.score = score;
    request.run_id = RandomRunId();
    const std::uint64_t run_id = request.run_id;
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(request));
    }
    condition_.notify_one();
    return run_id;
}

void Client::RequestTop(std::size_t count) {
    RequestTop(count, false, 0);
}

void Client::RequestTop(std::size_t count, bool best_per_account, std::uint64_t since) {
    Request request;
    request.kind = RequestKind::Top;
    request.count = count;
    request.best_per_account = best_per_account;
    request.since = since;
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(request));
    }
    condition_.notify_one();
}

std::future<AccountResult> Client::Register(std::string username, std::string display_name) {
    auto promise = std::make_shared<std::promise<AccountResult>>();
    std::future<AccountResult> future = promise->get_future();
    Request request;
    request.kind = RequestKind::Register;
    request.username = std::move(username);
    request.display_name = std::move(display_name);
    request.promise = std::move(promise);
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(request));
    }
    condition_.notify_one();
    return future;
}

std::future<AccountResult> Client::Login(std::string username, std::string token_hex) {
    auto promise = std::make_shared<std::promise<AccountResult>>();
    std::future<AccountResult> future = promise->get_future();
    Request request;
    request.kind = RequestKind::Login;
    request.username = std::move(username);
    request.token = std::move(token_hex);
    request.promise = std::move(promise);
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(request));
    }
    condition_.notify_one();
    return future;
}

std::future<AccountResult> Client::Resume(std::string session_token) {
    auto promise = std::make_shared<std::promise<AccountResult>>();
    std::future<AccountResult> future = promise->get_future();
    Request request;
    request.kind = RequestKind::Resume;
    request.token = std::move(session_token);
    request.promise = std::move(promise);
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(request));
    }
    condition_.notify_one();
    return future;
}

std::future<AccountResult> Client::UpdateSettings(std::string display_name, SyncedSettings settings) {
    auto promise = std::make_shared<std::promise<AccountResult>>();
    std::future<AccountResult> future = promise->get_future();
    Request request;
    request.kind = RequestKind::UpdateSettings;
    request.display_name = std::move(display_name);
    request.settings = settings;
    request.promise = std::move(promise);
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(request));
    }
    condition_.notify_one();
    return future;
}

Client::Snapshot Client::GetSnapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void Client::SetStatus(std::string status, bool connected) {
    std::lock_guard lock(mutex_);
    snapshot_.status = std::move(status);
    snapshot_.connected = connected;
    if (!connected) {
        snapshot_.authenticated = false;
    }
}

void Client::PublishAccount(const AccountResult& result) {
    std::lock_guard lock(mutex_);
    snapshot_.authenticated = true;
    snapshot_.account = AccountInfo{result.user_id, result.username, result.display_name};
    snapshot_.settings = result.settings;
    snapshot_.status = "connected";
}

bool Client::ConnectAndHandshake(TcpSocket& socket, CipherVariant& variant,
                                 std::vector<std::byte>& session_key) {
    const auto fail = [this](std::string reason) {
        handshake_error_ = std::move(reason);
        LogMessage(LogLevel::Warn, std::format("client: handshake failed: {}", handshake_error_));
        return false;
    };
    const auto key_prefix = [](std::span<const std::byte> key) {
        return TokenToHex(key).substr(0, 16);
    };

    socket = TcpSocket{};
    std::optional<TcpSocket> connected =
        TcpSocket::Connect(options_.host, options_.port, std::chrono::seconds(3));
    if (!connected.has_value()) {
        return fail(std::format("cannot reach server at {}:{}", options_.host, options_.port));
    }
    socket = std::move(*connected);

    const std::vector<std::byte> client_nonce = VulkanEngine::Security::RandomBytes(kNonceSize);
    if (client_nonce.size() != kNonceSize) {
        return fail("cannot generate a client nonce");
    }

    if (options_.server_public_key.has_value()) {
        // ── v2: X25519 key agreement, server authenticated by pinned key ──
        LogMessage(LogLevel::Debug,
                   std::format("client: v2 handshake with {}:{} (pinned server key {})",
                               options_.host, options_.port,
                               key_prefix(std::span<const std::byte>(
                                   options_.server_public_key->data(),
                                   options_.server_public_key->size()))));

        const VulkanEngine::Security::X25519KeyPair ephemeral =
            VulkanEngine::Security::GenerateX25519KeyPair();

        HelloV2Message hello{};
        hello.max_proto = kProtocolMaxVersion;
        hello.cipher_mask = SupportedCipherMask();
        hello.config_hash = options_.config_hash;
        std::copy(client_nonce.begin(), client_nonce.end(), hello.client_nonce.begin());
        std::copy(ephemeral.public_key.begin(), ephemeral.public_key.end(),
                  hello.client_public_key.begin());

        const std::vector<std::byte> payload = Encode(hello);
        const FrameHeader header{kProtocolMaxVersion, MessageType::Hello, 0, 0, options_.config_hash,
                                 static_cast<std::uint32_t>(payload.size())};
        if (!SendCleartext(socket, header, payload)) {
            return fail("cannot send the handshake");
        }

        std::vector<std::byte> frame;
        if (!RecvFrame(socket, frame) || frame.size() < kHeaderSize) {
            return fail("no handshake reply from the server");
        }
        const std::optional<FrameHeader> reply = DecodeHeader(frame);
        if (!reply.has_value() || reply->type != MessageType::HelloAck ||
            reply->proto != kProtocolMaxVersion) {
            return fail("server does not speak protocol v2 (older server?)");
        }
        const std::optional<HelloAckV2Message> ack =
            DecodeHelloAckV2(std::span<const std::byte>(frame).subspan(kHeaderSize));
        if (!ack.has_value()) {
            return fail("malformed v2 handshake reply");
        }

        VulkanEngine::Security::X25519Key server_public{};
        if (!CopyArray(ack->server_public_key, server_public)) {
            return fail("malformed server public key");
        }
        // Pin: the server must present the key this client was built with.
        if (server_public != *options_.server_public_key) {
            return fail(std::format(
                "server identity mismatch: pinned {}..., server offered {}... "
                "(re-pin leaderboard_server_pubkey.txt to this server's key and rebuild)",
                key_prefix(std::span<const std::byte>(options_.server_public_key->data(),
                                                      options_.server_public_key->size())),
                key_prefix(server_public)));
        }

        const std::optional<VulkanEngine::Security::X25519Key> shared =
            VulkanEngine::Security::X25519SharedSecret(ephemeral.secret, server_public);
        if (!shared.has_value()) {
            return fail("key exchange produced a degenerate shared secret");
        }

        variant = static_cast<CipherVariant>(ack->cipher_variant);
        session_key = DeriveX25519SessionKey(*shared, hello.client_nonce, ack->server_nonce,
                                             options_.config_hash);
        if (session_key.size() != 32) {
            return fail("session key derivation failed");
        }
        LogMessage(LogLevel::Debug, "client: v2 handshake complete");
        return true;
    }

    // ── v1: pre-shared key. Without a pinned server key there is no key
    // agreement, so the handshake must advertise v1 or the server will try to
    // parse a v2 Hello from this payload.
    LogMessage(LogLevel::Debug, "client: v1 (pre-shared key) handshake");
    HelloMessage hello{};
    hello.max_proto = kProtocolMinVersion;
    hello.cipher_mask = SupportedCipherMask();
    hello.config_hash = options_.config_hash;
    std::copy(client_nonce.begin(), client_nonce.end(), hello.client_nonce.begin());

    const std::vector<std::byte> payload = Encode(hello);
    const FrameHeader header{kProtocolMinVersion, MessageType::Hello, 0, 0, options_.config_hash,
                             static_cast<std::uint32_t>(payload.size())};
    if (!SendCleartext(socket, header, payload)) {
        return fail("cannot send the handshake");
    }

    std::vector<std::byte> frame;
    if (!RecvFrame(socket, frame) || frame.size() < kHeaderSize) {
        return fail("no handshake reply from the server");
    }
    const std::optional<FrameHeader> reply = DecodeHeader(frame);
    if (!reply.has_value() || reply->type != MessageType::HelloAck) {
        return fail("server does not speak protocol v1");
    }
    const std::optional<HelloAckMessage> ack =
        DecodeHelloAck(std::span<const std::byte>(frame).subspan(kHeaderSize));
    if (!ack.has_value()) {
        return fail("malformed v1 handshake reply");
    }
    variant = static_cast<CipherVariant>(ack->cipher_variant);
    session_key = DerivePskSessionKey(options_.psk, hello.client_nonce, ack->server_nonce,
                                      options_.config_hash);
    if (session_key.size() != 32) {
        return fail("session key derivation failed");
    }
    LogMessage(LogLevel::Debug, "client: v1 handshake complete");
    return true;
}

std::optional<std::vector<std::byte>> Client::Exchange(TcpSocket& socket, CipherVariant variant,
                                                       std::span<const std::byte> session_key,
                                                       MessageType type, std::span<const std::byte> inner,
                                                       MessageType& reply_type) {
    const std::uint64_t seq = next_seq_++;
    FrameHeader header{kProtocolVersion, type, kFlagEncrypted, seq, options_.config_hash, 0};
    const std::array<std::byte, kHeaderSize> header_bytes = EncodeHeader(header);
    const std::vector<std::byte> sealed = SealMessage(
        session_key, variant, seq, std::span<const std::byte>(header_bytes).subspan(0, kAadSize), inner);
    if (sealed.empty()) {
        return std::nullopt;
    }
    header.payload_len = static_cast<std::uint32_t>(sealed.size());
    if (!SendCleartext(socket, header, sealed)) {
        return std::nullopt;
    }

    std::vector<std::byte> frame;
    if (!RecvFrame(socket, frame) || frame.size() < kHeaderSize) {
        return std::nullopt;
    }
    const std::optional<FrameHeader> reply = DecodeHeader(frame);
    if (!reply.has_value() || reply->seq != seq) {
        return std::nullopt;
    }
    reply_type = reply->type;
    const std::span<const std::byte> payload = std::span<const std::byte>(frame).subspan(kHeaderSize);
    if ((reply->flags & kFlagEncrypted) == 0) {
        return std::vector<std::byte>(payload.begin(), payload.end());
    }
    return OpenMessage(session_key, variant, seq, std::span<const std::byte>(frame).subspan(0, kAadSize),
                       payload);
}

bool Client::Process(TcpSocket& socket, CipherVariant variant, std::span<const std::byte> session_key,
                     Request& request) {
    std::vector<std::byte> inner;
    MessageType type = MessageType::Error;
    switch (request.kind) {
        case RequestKind::Submit:
            type = MessageType::Submit;
            inner = Encode(SubmitMessage{request.run_id, request.score});
            break;
        case RequestKind::Top: {
            type = MessageType::TopRequest;
            TopRequestMessage top{};
            top.count = static_cast<std::uint16_t>(request.count);
            top.best_per_account = request.best_per_account;
            top.since = request.since;
            inner = Encode(top);
            break;
        }
        case RequestKind::Register:
            type = MessageType::RegisterRequest;
            inner = Encode(RegisterRequestMessage{request.username, request.display_name});
            break;
        case RequestKind::Login:
            type = MessageType::LoginRequest;
            inner = Encode(LoginRequestMessage{request.username, request.token});
            break;
        case RequestKind::Resume:
            type = MessageType::ResumeRequest;
            inner = Encode(ResumeRequestMessage{request.token});
            break;
        case RequestKind::UpdateSettings:
            type = MessageType::SettingsUpdate;
            inner = Encode(SettingsUpdateMessage{request.display_name, request.settings});
            break;
    }

    LogMessage(LogLevel::Debug,
               std::format("client: -> {} ({} bytes)", ToString(type), inner.size()));

    MessageType reply_type = MessageType::Error;
    const std::optional<std::vector<std::byte>> plain =
        Exchange(socket, variant, session_key, type, inner, reply_type);
    if (!plain.has_value()) {
        LogMessage(LogLevel::Warn, std::format("client: no usable reply to {}", ToString(type)));
        if (request.promise) {
            AccountResult result;
            result.status = SyncStatus::ServerError;
            result.message = "connection lost";
            request.promise->set_value(std::move(result));
        }
        return false;
    }
    LogMessage(LogLevel::Debug,
               std::format("client: <- {} ({} bytes)", ToString(reply_type), plain->size()));

    if (reply_type == MessageType::Error) {
        const std::optional<ErrorMessage> error = DecodeError(*plain);
        const std::string text = error.has_value() ? error->text : std::string{"server error"};
        LogMessage(LogLevel::Warn, std::format("client: server rejected {}: {}", ToString(type), text));
        if (request.promise) {
            AccountResult result;
            result.status = SyncStatus::ServerError;
            result.message = text;
            request.promise->set_value(std::move(result));
        } else {
            SetStatus(text, true);
        }
        return true;
    }

    switch (request.kind) {
        case RequestKind::Submit: {
            const std::optional<SubmitAckMessage> ack = DecodeSubmitAck(*plain);
            if (!ack.has_value()) {
                return false;
            }
            std::lock_guard lock(mutex_);
            snapshot_.has_rank = true;
            snapshot_.last_rank = ack->rank;
            snapshot_.last_total = ack->total;
            snapshot_.last_best = ack->best;
            return true;
        }
        case RequestKind::Top: {
            const std::optional<TopReplyMessage> reply = DecodeTopReply(*plain);
            if (!reply.has_value()) {
                return false;
            }
            std::lock_guard lock(mutex_);
            snapshot_.top = reply->entries;
            return true;
        }
        case RequestKind::Register: {
            const std::optional<RegisterReplyMessage> reply = DecodeRegisterReply(*plain);
            if (!reply.has_value()) {
                return false;
            }
            AccountResult result;
            result.status = reply->status;
            result.user_id = reply->user_id;
            // The server stores the canonical form, so report and persist that
            // rather than whatever casing the caller typed.
            result.username = CanonicalizeUsername(request.username).value_or(request.username);
            result.display_name = reply->display_name;
            result.token = reply->token;
            if (result.ok()) {
                SetCredentials(result.username, result.token);
                PublishAccount(result);
            } else {
                result.message = std::string{ToString(result.status)};
            }
            if (request.promise) {
                request.promise->set_value(std::move(result));
            }
            return true;
        }
        case RequestKind::Login:
        case RequestKind::Resume: {
            const std::optional<LoginReplyMessage> reply = DecodeLoginReply(*plain);
            if (!reply.has_value()) {
                return false;
            }
            AccountResult result;
            result.status = reply->status;
            result.user_id = reply->account.id;
            result.username = reply->account.username;
            result.display_name = reply->account.display_name;
            result.session_token = reply->session_token;
            result.session_expires_at = reply->session_expires_at;
            result.settings = reply->settings;
            if (result.ok()) {
                SetSession(result.session_token, result.session_expires_at);
                PublishAccount(result);
            } else {
                result.message = std::string{ToString(result.status)};
                if (request.kind == RequestKind::Resume) {
                    ForgetSession();
                }
            }
            if (request.promise) {
                request.promise->set_value(std::move(result));
            }
            return true;
        }
        case RequestKind::UpdateSettings: {
            const std::optional<SettingsReplyMessage> reply = DecodeSettingsReply(*plain);
            if (!reply.has_value()) {
                return false;
            }
            AccountResult result;
            result.status = reply->status;
            result.user_id = reply->account.id;
            result.username = reply->account.username;
            result.display_name = reply->account.display_name;
            result.settings = reply->settings;
            if (result.ok()) {
                PublishAccount(result);
            } else {
                result.message = std::string{ToString(result.status)};
            }
            if (request.promise) {
                request.promise->set_value(std::move(result));
            }
            return true;
        }
    }
    return false;
}

bool Client::Authenticate(TcpSocket& socket, CipherVariant variant,
                          std::span<const std::byte> session_key) {
    std::string username;
    std::string token;
    std::string session;
    {
        std::lock_guard lock(mutex_);
        username = username_;
        token = token_;
        session = session_token_;
    }

    if (!username.empty() && !token.empty()) {
        LogMessage(LogLevel::Info, std::format("client: signing in as {}", username));
        Request request;
        request.kind = RequestKind::Login;
        request.username = username;
        request.token = token;
        return Process(socket, variant, session_key, request);
    }
    if (!session.empty()) {
        LogMessage(LogLevel::Debug, "client: resuming a stored session");
        Request request;
        request.kind = RequestKind::Resume;
        request.token = session;
        return Process(socket, variant, session_key, request);
    }
    LogMessage(LogLevel::Info, "client: connected anonymously (no stored account)");
    return true;
}

void Client::WorkerLoop() {
    // Start retrying quickly so a transient connect or handshake failure is
    // invisible in play, and back off towards a second so a genuinely down
    // server is not hammered.
    constexpr std::chrono::milliseconds kInitialReconnectDelay{150};
    constexpr std::chrono::milliseconds kMaxReconnectDelay{1000};
    std::chrono::milliseconds reconnect_delay = kInitialReconnectDelay;
    LogMessage(LogLevel::Debug, "client: worker started");

    while (running_.load()) {
        TcpSocket socket;
        CipherVariant variant = CipherVariant::XChaCha20Poly1305;
        std::vector<std::byte> session_key;

        if (!ConnectAndHandshake(socket, variant, session_key)) {
            SetStatus("offline: " + handshake_error_, false);
            std::this_thread::sleep_for(reconnect_delay);
            reconnect_delay = std::min(reconnect_delay * 2, kMaxReconnectDelay);
            continue;
        }
        // A clean connection resets the backoff.
        reconnect_delay = kInitialReconnectDelay;
        LogMessage(LogLevel::Info, std::format("client: connected to {}:{}", options_.host,
                                               options_.port));
        SetStatus("connected", true);
        next_seq_ = 1;
        if (!Authenticate(socket, variant, session_key)) {
            // A false here is a connection-level failure (a rejected credential
            // comes back as a reply, not a failure), so reconnect rather than
            // serving requests on a dead socket.
            LogMessage(LogLevel::Warn, "client: sign-in failed; reconnecting");
            socket.Close();
            SetStatus("offline: sign-in failed", false);
            std::this_thread::sleep_for(reconnect_delay);
            reconnect_delay = std::min(reconnect_delay * 2, kMaxReconnectDelay);
            continue;
        }

        while (running_.load() && socket.IsOpen()) {
            std::unique_lock lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(250),
                                [this] { return !queue_.empty() || !running_.load(); });
            if (!running_.load()) {
                break;
            }
            if (reconnect_.exchange(false)) {
                // The active profile changed; re-run the handshake with the new
                // credentials before serving more requests.
                LogMessage(LogLevel::Debug, "client: reconnecting to re-run credentials");
                break;
            }
            if (queue_.empty()) {
                continue;
            }
            Request request = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();

            if (!Process(socket, variant, session_key, request)) {
                LogMessage(LogLevel::Warn, "client: connection lost");
                socket.Close();
                SetStatus("offline: connection lost", false);
                break;
            }
        }
    }
    LogMessage(LogLevel::Debug, "client: worker stopped");
    SetStatus("stopped", false);
}

} // namespace Examples::InfiniteRunner::Leaderboard
