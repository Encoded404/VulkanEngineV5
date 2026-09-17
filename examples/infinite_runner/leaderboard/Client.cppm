module;

export module Examples.InfiniteRunner.Leaderboard.Client;

import std;

import VulkanEngine.DataCipher;
import VulkanEngine.KeyExchange;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.Protocol;
import Examples.InfiniteRunner.Leaderboard.Transport;

export namespace Examples::InfiniteRunner::Leaderboard {

struct ClientOptions {
    std::string host;
    std::uint16_t port = 0;
    std::uint64_t config_hash = 0;
    // Pinned server X25519 public key. Required: the client only speaks the
    // authenticated handshake.
    std::optional<VulkanEngine::Security::X25519Key> server_public_key{};

    // ── Connection health and retry policy ──
    std::chrono::milliseconds connect_timeout{std::chrono::seconds(3)};
    // A single request must complete within this long, including the roundtrip.
    std::chrono::milliseconds request_timeout{std::chrono::seconds(10)};
    // A Ping is sent after this much idle time so the server's idle timer and
    // any NAT/firewall state stay fresh, and a dead peer is noticed promptly.
    std::chrono::milliseconds keepalive_interval{std::chrono::seconds(20)};
    // A Ping that gets no Pong within this long marks the connection lost.
    std::chrono::milliseconds keepalive_timeout{std::chrono::seconds(5)};
    // A transport failure is re-sent (after reconnecting) up to this many
    // attempts before the request is reported as failed.
    std::size_t max_attempts = 3;
};

// Result of a register/login/resume/rename request. `ok()` is the only success
// test; the populated fields differ per operation. `reason` carries the
// server's explanation for a refusal.
struct AccountResult {
    SyncStatus status = SyncStatus::ServerError;
    std::string message;
    std::string reason;
    UserId user_id = 0;
    std::string username;
    std::string display_name;
    std::string token;          // registration token (Register)
    std::string session_token;  // session token (Login/Resume)
    std::uint64_t session_expires_at = 0;

    [[nodiscard]] bool ok() const { return status == SyncStatus::Ok; }
};

// Asynchronous leaderboard/account client.
//
// All socket work happens on a worker thread; the game thread enqueues requests
// and reads a cached snapshot or a future result, so the frame loop never
// blocks on I/O. The worker keeps one connection and re-establishes it, with
// the stored credentials, after a failure.
class Client {
public:
    struct Snapshot {
        bool connected = false;
        bool authenticated = false;
        std::string status = "starting";
        AccountInfo account{};
        bool has_rank = false;
        std::int32_t last_rank_runs = 0;
        std::int32_t last_rank_accounts = 0;
        std::int32_t last_best = 0;
        std::vector<TopEntry> top;
    };

    explicit Client(ClientOptions options);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void Start();
    void Stop();

    // Stored credentials make every (re)connection log in automatically. Call
    // after a successful Register or when loading a profile from disk.
    void SetCredentials(std::string username, std::string token_hex);
    void ClearCredentials();
    void SetSession(std::string session_token, std::uint64_t expires_at);
    void ForgetSession();

    // Drops the current connection so the worker reconnects and re-runs the
    // stored credentials. Used when the active profile changes.
    void RequestReconnect();

    // Returns the run id used for this submission so the caller can record the
    // same run locally and dedupe a retry. `best_per_account` selects which
    // rank the acknowledgement should emphasise.
    std::uint64_t SubmitScore(std::int32_t score, bool best_per_account);
    void RequestTop(std::size_t count);
    // Server-side filters: collapse to one row per account, only scores at or
    // after `since` (Unix seconds; 0 = no lower bound), and optionally only the
    // caller's own scores.
    void RequestTop(std::size_t count, bool best_per_account, std::uint64_t since,
                    bool only_mine = false);

    [[nodiscard]] std::future<AccountResult> Register(std::string username, std::string display_name);
    [[nodiscard]] std::future<AccountResult> Login(std::string username, std::string token_hex);
    [[nodiscard]] std::future<AccountResult> Resume(std::string session_token);
    [[nodiscard]] std::future<AccountResult> Rename(std::string display_name);

    [[nodiscard]] Snapshot GetSnapshot() const;

    // Diagnostics: successful handshakes, and requests re-sent after a
    // transport failure. Both are monotonic for the life of the client.
    [[nodiscard]] std::uint64_t ConnectionCount() const { return connection_count_.load(); }
    [[nodiscard]] std::uint64_t RetryCount() const { return retry_count_.load(); }

private:
    enum class RequestKind { Submit, Top, Register, Login, Resume, Rename };

    // Outcome of one request/reply exchange at the transport level.
    enum class ExchangeResult {
        Ok,            // a complete reply was received and decrypted
        SendFailed,    // the request could not be written
        RecvFailed,    // no reply arrived (timeout, EOF, or reset)
        InvalidReply,  // a reply arrived but its frame/sequence/decryption was bad
    };

    // Handled: the reply was consumed and the connection stays up.
    // TransportFailure: nothing usable came back; reconnect and retry.
    // ProtocolError: a reply arrived but could not be used; reconnect, no retry.
    enum class ProcessOutcome { Handled, TransportFailure, ProtocolError };

    struct Request {
        RequestKind kind = RequestKind::Top;
        std::int32_t score = 0;
        std::uint64_t run_id = 0;
        std::size_t count = 0;
        bool best_per_account = false;
        bool only_mine = false;
        std::uint64_t since = 0;
        std::string username;
        std::string token;
        std::string display_name;
        int attempts = 0; // sends so far; drives the retry cap
        std::shared_ptr<std::promise<AccountResult>> promise;
    };

    void WorkerLoop();
    [[nodiscard]] bool ConnectAndHandshake(TcpSocket& socket,
                                           VulkanEngine::Security::CipherVariant& variant,
                                           std::vector<std::byte>& session_key);
    // Runs the stored credentials/session against a fresh connection.
    [[nodiscard]] bool Authenticate(TcpSocket& socket, VulkanEngine::Security::CipherVariant variant,
                                    std::span<const std::byte> session_key);
    [[nodiscard]] ProcessOutcome Process(TcpSocket& socket,
                                         VulkanEngine::Security::CipherVariant variant,
                                         std::span<const std::byte> session_key, Request& request,
                                         std::chrono::milliseconds timeout);
    // Sends an encrypted request and reads/decrypts the reply.
    [[nodiscard]] ExchangeResult Exchange(
        TcpSocket& socket, VulkanEngine::Security::CipherVariant variant,
        std::span<const std::byte> session_key, MessageType type,
        std::span<const std::byte> inner, std::chrono::milliseconds timeout,
        MessageType& reply_type, std::vector<std::byte>& plain);
    // Sends a Ping and waits for the matching Pong. False means the connection
    // is no longer usable and the worker should reconnect.
    [[nodiscard]] bool KeepAlive(TcpSocket& socket, VulkanEngine::Security::CipherVariant variant,
                                 std::span<const std::byte> session_key);
    // Register is excluded: its reply carries the only copy of the token, and a
    // retry after a lost reply is refused as a duplicate username.
    [[nodiscard]] bool RetryEligible(RequestKind kind) const;
    void ResolveFailure(Request& request, std::string message);
    void FailQueuedRequests(std::string_view message);
    // Sleeps without delaying Stop: wakes as soon as running_ clears.
    void WaitForRetry(std::chrono::milliseconds delay);
    void SetStatus(std::string status, bool connected);
    void PublishAccount(const AccountResult& result);

    ClientOptions options_;
    // Reason the last handshake failed, for the status line. Written and read
    // only by the worker thread.
    std::string handshake_error_;
    std::string username_;
    std::string token_;
    std::string session_token_;
    std::uint64_t session_expires_at_ = 0;
    std::uint64_t next_seq_ = 1;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> reconnect_{false};
    std::atomic<std::uint64_t> connection_count_{0};
    std::atomic<std::uint64_t> retry_count_{0};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Request> queue_;
    Snapshot snapshot_{};
    // Points at the socket the worker currently owns, so Stop() can shut it
    // down to unblock a blocking recv. Guarded by mutex_; non-null only while
    // the worker is serving a connection.
    TcpSocket* active_socket_ = nullptr;
};

} // namespace Examples::InfiniteRunner::Leaderboard
