module;

export module Examples.InfiniteRunner.Leaderboard.Server;

import std;

import VulkanEngine.KeyExchange;
import VulkanEngine.PasswordHash;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.AccountStore;
import Examples.InfiniteRunner.Leaderboard.Store;
import Examples.InfiniteRunner.Leaderboard.Transport;

export namespace Examples::InfiniteRunner::Leaderboard {

struct ServerOptions {
    std::uint16_t port = 7777;
    // v1 fallback channel secret.
    std::vector<std::byte> psk;
    std::filesystem::path store_path{};    // score persistence
    std::filesystem::path account_path{};  // account persistence
    // Long-term X25519 secret (hex). Generated and written when absent.
    std::filesystem::path identity_path{};
    VulkanEngine::Security::Argon2Params argon2{};
    std::chrono::seconds session_ttl{std::chrono::hours(1)};
    bool accept_unknown_configs = true;
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
        std::uint16_t proto = kProtocolVersionMax();
        bool authenticated = false;
        AccountInfo account{};
        SyncedSettings settings{};
    };

    [[nodiscard]] static constexpr std::uint16_t kProtocolVersionMax() { return 2; }

    void HandleClient(TcpSocket socket);
    [[nodiscard]] bool LoadOrCreateIdentity();

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
