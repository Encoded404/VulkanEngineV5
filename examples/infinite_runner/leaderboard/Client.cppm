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
    // v1 fallback channel secret.
    std::vector<std::byte> psk;
    // Pinned server X25519 public key. When set the client uses the v2
    // handshake; when unset it falls back to the v1 PSK handshake.
    std::optional<VulkanEngine::Security::X25519Key> server_public_key{};
};

// Result of a register/login/resume/settings request. `ok()` is the only
// success test; the populated fields differ per operation.
struct AccountResult {
    SyncStatus status = SyncStatus::ServerError;
    std::string message;
    UserId user_id = 0;
    std::string username;
    std::string display_name;
    std::string token;          // registration token (Register)
    std::string session_token;  // session token (Login/Resume)
    std::uint64_t session_expires_at = 0;
    SyncedSettings settings{};

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
        SyncedSettings settings{};
        bool has_rank = false;
        std::int32_t last_rank = 0;
        std::int32_t last_total = 0;
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
    // same run locally and dedupe a retry.
    std::uint64_t SubmitScore(std::int32_t score);
    void RequestTop(std::size_t count);
    // Server-side filters: collapse to one row per account and/or only scores
    // at or after `since` (Unix seconds; 0 = no lower bound).
    void RequestTop(std::size_t count, bool best_per_account, std::uint64_t since);

    [[nodiscard]] std::future<AccountResult> Register(std::string username, std::string display_name);
    [[nodiscard]] std::future<AccountResult> Login(std::string username, std::string token_hex);
    [[nodiscard]] std::future<AccountResult> Resume(std::string session_token);
    [[nodiscard]] std::future<AccountResult> UpdateSettings(std::string display_name,
                                                            SyncedSettings settings);

    [[nodiscard]] Snapshot GetSnapshot() const;

private:
    enum class RequestKind { Submit, Top, Register, Login, Resume, UpdateSettings };

    struct Request {
        RequestKind kind = RequestKind::Top;
        std::int32_t score = 0;
        std::uint64_t run_id = 0;
        std::size_t count = 0;
        bool best_per_account = false;
        std::uint64_t since = 0;
        std::string username;
        std::string token;
        std::string display_name;
        SyncedSettings settings{};
        std::shared_ptr<std::promise<AccountResult>> promise;
    };

    void WorkerLoop();
    [[nodiscard]] bool ConnectAndHandshake(TcpSocket& socket,
                                           VulkanEngine::Security::CipherVariant& variant,
                                           std::vector<std::byte>& session_key);
    // Runs the stored credentials/session against a fresh connection.
    [[nodiscard]] bool Authenticate(TcpSocket& socket, VulkanEngine::Security::CipherVariant variant,
                                    std::span<const std::byte> session_key);
    [[nodiscard]] bool Process(TcpSocket& socket, VulkanEngine::Security::CipherVariant variant,
                               std::span<const std::byte> session_key, Request& request);
    // Sends an encrypted request and reads/decrypts the reply of `expect` type.
    [[nodiscard]] std::optional<std::vector<std::byte>> Exchange(
        TcpSocket& socket, VulkanEngine::Security::CipherVariant variant,
        std::span<const std::byte> session_key, MessageType type,
        std::span<const std::byte> inner, MessageType& reply_type);
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
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Request> queue_;
    Snapshot snapshot_{};
};

} // namespace Examples::InfiniteRunner::Leaderboard
