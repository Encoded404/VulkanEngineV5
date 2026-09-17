module;

export module Examples.InfiniteRunner.Leaderboard.Server;

import std;

import VulkanEngine.KeyExchange;
import VulkanEngine.PasswordHash;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.AccountStore;
import Examples.InfiniteRunner.Leaderboard.Protocol;
import Examples.InfiniteRunner.Leaderboard.Store;
import Examples.InfiniteRunner.Leaderboard.Transport;

export namespace Examples::InfiniteRunner::Leaderboard {

struct ServerOptions {
    std::uint16_t port = 7777;
    // Directory holding one JSON board per ruleset.
    std::filesystem::path store_path{};
    // Optional legacy line-based score file to import once at startup.
    std::filesystem::path legacy_store_path{};
    // Account persistence.
    std::filesystem::path account_path{};
    // Long-term X25519 secret (hex). Generated and written when absent.
    std::filesystem::path identity_path{};
    VulkanEngine::Security::Argon2Params argon2{};
    std::chrono::seconds session_ttl{std::chrono::hours(1)};
    // Retained scores per account, independent of the display count.
    std::size_t max_scores_per_account = 32;
    // Ruleset fingerprints the server will serve, beyond the current build's.
    std::vector<std::uint64_t> accepted_configs;
    // Tests use this; production sets an accepted_configs list instead.
    bool accept_unknown_configs = false;
    // Username/display-name content policy: blocked substrings plus an allow
    // list of roots that exempt the span they cover.
    NamePolicy name_policy;
    // Simple per-connection token bucket. A request costs one token; the bucket
    // refills one token every `rate_limit_refill`. A zero refill disables it.
    std::size_t rate_limit_burst = 8;
    std::chrono::milliseconds rate_limit_refill{500};
    // How often dirty boards are written to disk.
    std::chrono::milliseconds flush_interval{2000};
    // A client that has sent nothing for this long is closed. A Ping resets it,
    // and the same budget bounds the wait for the handshake.
    std::chrono::seconds idle_timeout{std::chrono::seconds(90)};
    // Accepted sockets use this as the blocking recv timeout, so a handler can
    // wake to notice Stop() and the idle deadline while waiting for a request.
    std::chrono::milliseconds socket_poll_interval{std::chrono::milliseconds(500)};
    // A reply must be writable within this long; a peer that stops reading must
    // not wedge a handler forever.
    std::chrono::milliseconds send_timeout{std::chrono::seconds(10)};
};

// Minimal leaderboard server: accepts a client, negotiates the protocol version
// and cipher, then serves account and score requests until the client
// disconnects or errors. Each connection is served on its own thread, so one
// idle or slow client cannot delay another's handshake. The stores are
// internally synchronised and the options/identity are read-only after Start.
class Server {
public:
    explicit Server(ServerOptions options);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    [[nodiscard]] bool Start();
    void Run();
    void Stop();

    [[nodiscard]] std::uint16_t Port() const { return listener_.BoundPort(); }
    [[nodiscard]] const ScoreStore& Store() const { return store_; }
    [[nodiscard]] const AccountStore& Accounts() const { return accounts_; }
    [[nodiscard]] const VulkanEngine::Security::X25519Key& PublicKey() const { return identity_.public_key; }

private:
    struct ConnectionState {
        std::uint16_t proto = kProtocolVersion;
        bool authenticated = false;
        AccountInfo account{};
        // Legacy v2 "show on leaderboard" flag: when set, a submission is
        // acknowledged but not recorded. Always false for v3 clients.
        bool hidden = false;
    };

    void HandleClient(TcpSocket socket);
    [[nodiscard]] bool LoadOrCreateIdentity();
    // True when `config_hash` may be served.
    [[nodiscard]] bool ConfigAccepted(std::uint64_t config_hash) const;
    // Reads one frame, waking every `socket_poll_interval` to check Stop() and
    // `deadline`. Returns IoStatus::Timeout when the deadline passed with the
    // connection still healthy, and the transport status otherwise.
    [[nodiscard]] IoStatus RecvFrameWithIdle(TcpSocket& socket, std::vector<std::byte>& frame,
                                             std::chrono::steady_clock::time_point deadline);

    ServerOptions options_;
    ScoreStore store_;
    AccountStore accounts_;
    SessionStore sessions_;
    VulkanEngine::Security::X25519KeyPair identity_{};
    TcpListener listener_;
    std::atomic<bool> running_{false};

    // One task per live connection. Finished tasks are reaped by Run(); Stop()
    // waits for the rest so no handler outlives the server.
    std::mutex clients_mutex_;
    std::vector<std::future<void>> clients_;
};

} // namespace Examples::InfiniteRunner::Leaderboard
