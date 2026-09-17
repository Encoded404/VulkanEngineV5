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
    return SendFrame(socket, frame) == IoStatus::Ok;
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
    {
        std::lock_guard lock(mutex_);
        if (active_socket_ != nullptr) {
            // Interrupts a blocking recv/send inside Exchange so the worker can
            // notice running_ and exit promptly instead of waiting out the
            // request timeout. The worker still owns and closes the socket.
            active_socket_->Shutdown();
        }
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

std::uint64_t Client::SubmitScore(std::int32_t score, bool best_per_account) {
    Request request;
    request.kind = RequestKind::Submit;
    request.score = score;
    request.best_per_account = best_per_account;
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
    RequestTop(count, false, 0, false);
}

void Client::RequestTop(std::size_t count, bool best_per_account, std::uint64_t since,
                        bool only_mine) {
    Request request;
    request.kind = RequestKind::Top;
    request.count = count;
    request.best_per_account = best_per_account;
    request.since = since;
    request.only_mine = only_mine;
    {
        std::lock_guard lock(mutex_);
        // A top request is a snapshot with no promise: a newer one supersedes
        // any still queued, so a reconnect cannot replay a backlog of stale
        // boards and the queue stays bounded while offline.
        std::erase_if(queue_, [](const Request& queued) { return queued.kind == RequestKind::Top; });
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

std::future<AccountResult> Client::Rename(std::string display_name) {
    auto promise = std::make_shared<std::promise<AccountResult>>();
    std::future<AccountResult> future = promise->get_future();
    Request request;
    request.kind = RequestKind::Rename;
    request.display_name = std::move(display_name);
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

    if (!options_.server_public_key.has_value()) {
        return fail("no server public key pinned; cannot authenticate");
    }

    socket = TcpSocket{};
    std::optional<TcpSocket> connected =
        TcpSocket::Connect(options_.host, options_.port, options_.connect_timeout);
    if (!connected.has_value()) {
        return fail(std::format("cannot reach server at {}:{}", options_.host, options_.port));
    }
    socket = std::move(*connected);
    // The transport's default applies to the placeholder handshake; from here
    // on every blocking call is bounded by the configured request timeout.
    socket.SetTimeouts(options_.request_timeout, options_.request_timeout);

    const std::vector<std::byte> client_nonce = VulkanEngine::Security::RandomBytes(kNonceSize);
    if (client_nonce.size() != kNonceSize) {
        return fail("cannot generate a client nonce");
    }

    LogMessage(LogLevel::Debug,
               std::format("client: handshake with {}:{} (pinned server key {})", options_.host,
                           options_.port,
                           key_prefix(std::span<const std::byte>(options_.server_public_key->data(),
                                                                 options_.server_public_key->size()))));

    const VulkanEngine::Security::X25519KeyPair ephemeral =
        VulkanEngine::Security::GenerateX25519KeyPair();

    HelloSecureMessage hello{};
    hello.max_proto = kProtocolVersion;
    hello.cipher_mask = SupportedCipherMask();
    hello.config_hash = options_.config_hash;
    std::copy(client_nonce.begin(), client_nonce.end(), hello.client_nonce.begin());
    std::copy(ephemeral.public_key.begin(), ephemeral.public_key.end(), hello.client_public_key.begin());

    const std::vector<std::byte> payload = Encode(hello);
    const FrameHeader header{kProtocolVersion, MessageType::Hello, 0, 0, options_.config_hash,
                             static_cast<std::uint32_t>(payload.size())};
    if (!SendCleartext(socket, header, payload)) {
        return fail("cannot send the handshake");
    }

    std::vector<std::byte> frame;
    if (RecvFrame(socket, frame) != IoStatus::Ok || frame.size() < kHeaderSize) {
        return fail("no handshake reply from the server");
    }
    const std::optional<FrameHeader> reply = DecodeHeader(frame);
    if (!reply.has_value()) {
        return fail("malformed handshake reply");
    }
    const std::span<const std::byte> body = std::span<const std::byte>(frame).subspan(kHeaderSize);
    if (reply->type == MessageType::Error) {
        // The server refused before the handshake completed (unknown ruleset,
        // for example). Surface its text so the status line is actionable.
        const std::optional<ErrorMessage> error = DecodeError(body);
        return fail(error.has_value() ? error->text : std::string{"server refused the connection"});
    }
    if (reply->type != MessageType::HelloAck) {
        return fail("server did not complete the handshake");
    }
    if (reply->proto != kProtocolVersion) {
        return fail(std::format("server speaks protocol v{} but this build requires v{} "
                                "(server is out of date; redeploy it)",
                                reply->proto, kProtocolVersion));
    }
    const std::optional<HelloAckSecureMessage> ack = DecodeHelloAck(body);
    if (!ack.has_value()) {
        return fail("malformed handshake reply");
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
    LogMessage(LogLevel::Debug, "client: handshake complete");
    return true;
}

Client::ExchangeResult Client::Exchange(TcpSocket& socket, CipherVariant variant,
                                        std::span<const std::byte> session_key, MessageType type,
                                        std::span<const std::byte> inner,
                                        std::chrono::milliseconds timeout, MessageType& reply_type,
                                        std::vector<std::byte>& plain) {
    // Each request sets its own deadline: a normal request waits the request
    // timeout while a keepalive waits only for its Pong.
    socket.SetTimeouts(timeout, timeout);

    const std::uint64_t seq = next_seq_++;
    FrameHeader header{kProtocolVersion, type, kFlagEncrypted, seq, options_.config_hash, 0};
    const std::array<std::byte, kHeaderSize> header_bytes = EncodeHeader(header);
    const std::vector<std::byte> sealed = SealMessage(
        session_key, variant, seq, std::span<const std::byte>(header_bytes).subspan(0, kAadSize), inner);
    if (sealed.empty()) {
        LogMessage(LogLevel::Warn, std::format("client: cannot seal {}", ToString(type)));
        return ExchangeResult::SendFailed;
    }
    header.payload_len = static_cast<std::uint32_t>(sealed.size());
    if (!SendCleartext(socket, header, sealed)) {
        LogMessage(LogLevel::Debug, std::format("client: cannot send {}", ToString(type)));
        return ExchangeResult::SendFailed;
    }

    std::vector<std::byte> frame;
    const IoStatus receive_status = RecvFrame(socket, frame);
    if (receive_status != IoStatus::Ok) {
        LogMessage(LogLevel::Debug,
                   std::format("client: no reply to {} ({})", ToString(type),
                               receive_status == IoStatus::Timeout ? "timed out"
                                                                   : "connection lost"));
        return ExchangeResult::RecvFailed;
    }
    if (frame.size() < kHeaderSize) {
        LogMessage(LogLevel::Warn, std::format("client: short reply to {}", ToString(type)));
        return ExchangeResult::InvalidReply;
    }
    const std::optional<FrameHeader> reply = DecodeHeader(frame);
    if (!reply.has_value() || reply->seq != seq) {
        LogMessage(LogLevel::Warn, std::format("client: malformed reply to {} (seq {})",
                                               ToString(type), seq));
        return ExchangeResult::InvalidReply;
    }
    reply_type = reply->type;
    const std::span<const std::byte> payload = std::span<const std::byte>(frame).subspan(kHeaderSize);
    if ((reply->flags & kFlagEncrypted) == 0) {
        plain.assign(payload.begin(), payload.end());
        return ExchangeResult::Ok;
    }
    const std::optional<std::vector<std::byte>> opened =
        OpenMessage(session_key, variant, seq,
                    std::span<const std::byte>(frame).subspan(0, kAadSize), payload);
    if (!opened.has_value()) {
        LogMessage(LogLevel::Warn,
                   std::format("client: cannot decrypt the reply to {}", ToString(type)));
        return ExchangeResult::InvalidReply;
    }
    plain = *opened;
    return ExchangeResult::Ok;
}

Client::ProcessOutcome Client::Process(TcpSocket& socket, CipherVariant variant,
                                       std::span<const std::byte> session_key, Request& request,
                                       std::chrono::milliseconds timeout) {
    std::vector<std::byte> inner;
    MessageType type = MessageType::Error;
    switch (request.kind) {
        case RequestKind::Submit:
            type = MessageType::SubmitV3;
            inner = Encode(SubmitV3Message{request.run_id, request.score, request.best_per_account});
            break;
        case RequestKind::Top: {
            type = MessageType::TopRequestV3;
            TopRequestV3Message top{};
            top.count = static_cast<std::uint16_t>(request.count);
            top.best_per_account = request.best_per_account;
            top.since = request.since;
            top.only_mine = request.only_mine;
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
        case RequestKind::Rename:
            type = MessageType::RenameRequest;
            inner = Encode(RenameRequestMessage{request.display_name});
            break;
    }

    LogMessage(LogLevel::Debug,
               std::format("client: -> {} ({} bytes)", ToString(type), inner.size()));

    MessageType reply_type = MessageType::Error;
    std::vector<std::byte> plain;
    const ExchangeResult exchange =
        Exchange(socket, variant, session_key, type, inner, timeout, reply_type, plain);
    if (exchange == ExchangeResult::SendFailed || exchange == ExchangeResult::RecvFailed) {
        // Nothing usable came back. The worker decides whether this request may
        // be re-sent after reconnecting.
        LogMessage(LogLevel::Warn, std::format("client: no usable reply to {}", ToString(type)));
        return ProcessOutcome::TransportFailure;
    }
    if (exchange == ExchangeResult::InvalidReply) {
        LogMessage(LogLevel::Warn, std::format("client: unusable reply to {}", ToString(type)));
        return ProcessOutcome::ProtocolError;
    }
    LogMessage(LogLevel::Debug,
               std::format("client: <- {} ({} bytes)", ToString(reply_type), plain.size()));

    if (reply_type == MessageType::Error) {
        const std::optional<ErrorMessage> error = DecodeError(plain);
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
        return ProcessOutcome::Handled;
    }

    switch (request.kind) {
        case RequestKind::Submit: {
            const std::optional<SubmitAckV3Message> ack = DecodeSubmitAckV3(plain);
            if (!ack.has_value()) {
                return ProcessOutcome::ProtocolError;
            }
            std::lock_guard lock(mutex_);
            snapshot_.has_rank = true;
            snapshot_.last_rank_runs = ack->rank_runs;
            snapshot_.last_rank_accounts = ack->rank_accounts;
            snapshot_.last_best = ack->best;
            return ProcessOutcome::Handled;
        }
        case RequestKind::Top: {
            const std::optional<TopReplyMessage> reply = DecodeTopReply(plain);
            if (!reply.has_value()) {
                return ProcessOutcome::ProtocolError;
            }
            std::lock_guard lock(mutex_);
            snapshot_.top = reply->entries;
            return ProcessOutcome::Handled;
        }
        case RequestKind::Register: {
            const std::optional<RegisterReplyV3Message> reply = DecodeRegisterReplyV3(plain);
            if (!reply.has_value()) {
                return ProcessOutcome::ProtocolError;
            }
            AccountResult result;
            result.status = reply->status;
            result.reason = reply->reason;
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
                result.message = result.reason.empty() ? std::string{ToString(result.status)}
                                                        : result.reason;
            }
            if (request.promise) {
                request.promise->set_value(std::move(result));
            }
            return ProcessOutcome::Handled;
        }
        case RequestKind::Login:
        case RequestKind::Resume: {
            const std::optional<LoginReplyMessage> reply = DecodeLoginReply(plain);
            if (!reply.has_value()) {
                return ProcessOutcome::ProtocolError;
            }
            AccountResult result;
            result.status = reply->status;
            result.user_id = reply->account.id;
            result.username = reply->account.username;
            result.display_name = reply->account.display_name;
            result.session_token = reply->session_token;
            result.session_expires_at = reply->session_expires_at;
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
            return ProcessOutcome::Handled;
        }
        case RequestKind::Rename: {
            const std::optional<RenameReplyV3Message> reply = DecodeRenameReplyV3(plain);
            if (!reply.has_value()) {
                return ProcessOutcome::ProtocolError;
            }
            AccountResult result;
            result.status = reply->status;
            result.reason = reply->reason;
            result.user_id = reply->account.id;
            result.username = reply->account.username;
            result.display_name = reply->account.display_name;
            if (result.ok()) {
                PublishAccount(result);
            } else {
                result.message = result.reason.empty() ? std::string{ToString(result.status)}
                                                        : result.reason;
            }
            if (request.promise) {
                request.promise->set_value(std::move(result));
            }
            return ProcessOutcome::Handled;
        }
    }
    return ProcessOutcome::ProtocolError;
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
        return Process(socket, variant, session_key, request, options_.request_timeout) ==
               ProcessOutcome::Handled;
    }
    if (!session.empty()) {
        LogMessage(LogLevel::Debug, "client: resuming a stored session");
        Request request;
        request.kind = RequestKind::Resume;
        request.token = session;
        return Process(socket, variant, session_key, request, options_.request_timeout) ==
               ProcessOutcome::Handled;
    }
    LogMessage(LogLevel::Info, "client: connected without stored credentials");
    return true;
}

bool Client::RetryEligible(RequestKind kind) const {
    // Register mints a credential that is only ever sent in its reply; re-sending
    // after a lost reply would be refused as a duplicate username and the token
    // would be lost, so it is reported to the caller instead of retried.
    return kind != RequestKind::Register;
}

void Client::ResolveFailure(Request& request, std::string message) {
    if (!request.promise) {
        return;
    }
    AccountResult result;
    result.status = SyncStatus::ServerError;
    result.message = std::move(message);
    request.promise->set_value(std::move(result));
}

void Client::FailQueuedRequests(std::string_view message) {
    std::deque<Request> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(queue_);
    }
    for (Request& request : pending) {
        ResolveFailure(request, std::string{message});
    }
}

void Client::WaitForRetry(std::chrono::milliseconds delay) {
    std::unique_lock lock(mutex_);
    condition_.wait_for(lock, delay, [this] { return !running_.load(); });
}

bool Client::KeepAlive(TcpSocket& socket, CipherVariant variant,
                       std::span<const std::byte> session_key) {
    const std::uint64_t token = RandomRunId();
    MessageType reply_type = MessageType::Error;
    std::vector<std::byte> plain;
    const ExchangeResult result =
        Exchange(socket, variant, session_key, MessageType::Ping, Encode(PingMessage{token}),
                 options_.keepalive_timeout, reply_type, plain);
    if (result != ExchangeResult::Ok) {
        return false;
    }
    if (reply_type != MessageType::Pong) {
        LogMessage(LogLevel::Warn, std::format("client: keepalive got {} instead of a pong",
                                               ToString(reply_type)));
        return false;
    }
    const std::optional<PongMessage> pong = DecodePong(plain);
    if (!pong.has_value() || pong->token != token) {
        LogMessage(LogLevel::Warn, "client: keepalive pong did not match its ping");
        return false;
    }
    LogMessage(LogLevel::Trace, std::format("client: pong (token {:#x})", token));
    return true;
}

void Client::WorkerLoop() {
    // Start retrying quickly so a transient connect or handshake failure is
    // invisible in play, and back off towards a second so a genuinely down
    // server is not hammered. A rejected handshake (out-of-date server, wrong
    // ruleset) arrives here as a failure, so it backs off too.
    constexpr std::chrono::milliseconds kInitialReconnectDelay{150};
    constexpr std::chrono::milliseconds kMaxReconnectDelay{1000};
    // Short tick so a request that arrives while the worker is idle (and no
    // keepalive is due) is picked up promptly.
    constexpr std::chrono::milliseconds kQueuePollInterval{250};
    std::chrono::milliseconds reconnect_delay = kInitialReconnectDelay;
    LogMessage(LogLevel::Debug, "client: worker started");

    while (running_.load()) {
        TcpSocket socket;
        CipherVariant variant = CipherVariant::XChaCha20Poly1305;
        std::vector<std::byte> session_key;

        if (!ConnectAndHandshake(socket, variant, session_key)) {
            SetStatus("offline: " + handshake_error_, false);
            WaitForRetry(reconnect_delay);
            reconnect_delay = std::min(reconnect_delay * 2, kMaxReconnectDelay);
            continue;
        }
        // A clean connection resets the backoff.
        reconnect_delay = kInitialReconnectDelay;
        ++connection_count_;
        LogMessage(LogLevel::Info, std::format("client: connected to {}:{}", options_.host,
                                               options_.port));
        SetStatus("connected", true);
        next_seq_ = 1;
        {
            std::lock_guard lock(mutex_);
            active_socket_ = &socket;
        }
        if (!Authenticate(socket, variant, session_key)) {
            // A false here is a connection-level failure (a rejected credential
            // comes back as a reply, not a failure), so reconnect rather than
            // serving requests on a dead socket.
            LogMessage(LogLevel::Warn, "client: sign-in failed; reconnecting");
            {
                std::lock_guard lock(mutex_);
                active_socket_ = nullptr;
            }
            socket.Close();
            SetStatus("offline: sign-in failed", false);
            WaitForRetry(reconnect_delay);
            reconnect_delay = std::min(reconnect_delay * 2, kMaxReconnectDelay);
            continue;
        }

        auto last_activity = std::chrono::steady_clock::now();
        bool connection_lost = false;
        while (running_.load() && socket.IsOpen()) {
            Request request;
            bool have_request = false;
            {
                std::unique_lock lock(mutex_);
                const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_activity);
                const auto until_ping = options_.keepalive_interval > idle
                                            ? options_.keepalive_interval - idle
                                            : std::chrono::milliseconds::zero();
                condition_.wait_for(lock, std::min(kQueuePollInterval, until_ping), [this] {
                    return !queue_.empty() || !running_.load() || reconnect_.load();
                });
                if (!running_.load()) {
                    break;
                }
                if (reconnect_.exchange(false)) {
                    // The active profile changed; re-run the handshake with the
                    // new credentials before serving more requests.
                    LogMessage(LogLevel::Debug, "client: reconnecting to re-run credentials");
                    break;
                }
                if (!queue_.empty()) {
                    request = std::move(queue_.front());
                    queue_.pop_front();
                    have_request = true;
                }
            }

            if (!have_request) {
                // Idle: send a keepalive if one is due. This refreshes the
                // server's idle timer (and any NAT/firewall state) and detects a
                // peer that vanished without closing.
                const auto now = std::chrono::steady_clock::now();
                if (now - last_activity < options_.keepalive_interval) {
                    continue;
                }
                if (!KeepAlive(socket, variant, session_key)) {
                    LogMessage(LogLevel::Warn, "client: keepalive failed");
                    connection_lost = true;
                    break;
                }
                last_activity = now;
                continue;
            }

            ++request.attempts;
            const ProcessOutcome outcome =
                Process(socket, variant, session_key, request, options_.request_timeout);
            if (outcome == ProcessOutcome::Handled) {
                last_activity = std::chrono::steady_clock::now();
                continue;
            }

            // The connection is no longer usable. A transport failure may be a
            // dropped link rather than a bad request, so the request goes back
            // on the queue and is re-sent after the reconnect; a protocol error
            // is not retried.
            const bool retry = outcome == ProcessOutcome::TransportFailure &&
                               RetryEligible(request.kind) &&
                               request.attempts < static_cast<int>(options_.max_attempts);
            if (retry) {
                ++retry_count_;
                LogMessage(LogLevel::Debug,
                           std::format("client: re-sending after reconnect (attempt {}/{})",
                                       request.attempts + 1, options_.max_attempts));
                std::lock_guard lock(mutex_);
                queue_.push_front(std::move(request));
            } else {
                std::string message;
                if (outcome == ProcessOutcome::ProtocolError) {
                    message = "protocol error";
                } else if (request.kind == RequestKind::Register) {
                    message = "connection lost; the account may or may not have been created — "
                              "sign in to check";
                } else {
                    message = std::format("connection lost (gave up after {} attempts)",
                                          request.attempts);
                }
                ResolveFailure(request, std::move(message));
            }
            connection_lost = true;
            break;
        }

        {
            std::lock_guard lock(mutex_);
            active_socket_ = nullptr;
        }
        socket.Close();
        if (!running_.load()) {
            break;
        }
        if (connection_lost) {
            LogMessage(LogLevel::Warn, "client: connection lost");
            SetStatus("offline: connection lost", false);
        }
    }

    // A caller waiting on a future must not hang because the worker stopped
    // with requests still queued.
    FailQueuedRequests("client stopped");
    LogMessage(LogLevel::Debug, "client: worker stopped");
    SetStatus("stopped", false);
}

} // namespace Examples::InfiniteRunner::Leaderboard
