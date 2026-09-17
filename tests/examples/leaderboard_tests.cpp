#include <gtest/gtest.h>

#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

import std;
import std.compat;

import VulkanEngine.KeyExchange;
import VulkanEngine.PasswordHash;
import Examples.InfiniteRunner.Leaderboard.Account;
import Examples.InfiniteRunner.Leaderboard.Client;
import Examples.InfiniteRunner.Leaderboard.Config;
import Examples.InfiniteRunner.Leaderboard.Log;
import Examples.InfiniteRunner.Leaderboard.Protocol;
import Examples.InfiniteRunner.Leaderboard.Server;
import Examples.InfiniteRunner.Leaderboard.Store;

namespace {

using namespace Examples::InfiniteRunner::Leaderboard;

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

ServerOptions TestServerOptions() {
    ServerOptions options;
    options.port = 0; // ephemeral
    options.accept_unknown_configs = true;
    options.argon2.blocks = 64;
    options.argon2.passes = 2;
    options.argon2.lanes = 1;
    options.rate_limit_refill = std::chrono::milliseconds{0}; // disabled
    return options;
}

ClientOptions TestClientOptions(const Server& server) {
    ClientOptions options;
    options.host = "127.0.0.1";
    options.port = server.Port();
    options.config_hash = CurrentBalanceHash();
    options.server_public_key = server.PublicKey();
    return options;
}

// A running server that always stops and joins, so an early ASSERT return
// cannot reach a joinable std::thread destructor.
struct RunningServer {
    Server server;
    std::thread thread;

    explicit RunningServer(ServerOptions options) : server(std::move(options)) {
        EXPECT_TRUE(server.Start());
        thread = std::thread([this] { server.Run(); });
    }
    ~RunningServer() {
        server.Stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;
};

struct RunningClient {
    Client client;
    explicit RunningClient(ClientOptions options) : client(std::move(options)) { client.Start(); }
    ~RunningClient() { client.Stop(); }
    RunningClient(const RunningClient&) = delete;
    RunningClient& operator=(const RunningClient&) = delete;
};

// ── ScoreStore ──

TEST(LeaderboardTest, StoreSeparatesRulesetsAndRanks) {
    ScoreStore store;
    const auto a = store.Submit(0xAAAA, 10, 1, 1'700'000'000);
    const auto b = store.Submit(0xBBBB, 999, 2, 1'700'000'100);
    EXPECT_EQ(a.rank_accounts, 1);
    EXPECT_EQ(b.rank_accounts, 1);
    EXPECT_EQ(store.Size(0xAAAA), 1U);
    ASSERT_EQ(store.Top(0xBBBB, 5).size(), 1U);
    EXPECT_EQ(store.Top(0xBBBB, 5).front().score, 999);
    EXPECT_EQ(store.Top(0xBBBB, 5).front().rank, 1);
    EXPECT_EQ(store.Top(0xBBBB, 5).front().user_id, 2U);
    EXPECT_EQ(store.Top(0xBBBB, 5).front().recorded_at, 1'700'000'100U);
}

TEST(LeaderboardTest, StoreFiltersByDateAndBestPerAccount) {
    ScoreStore store;
    constexpr std::uint64_t kHash = 0x99;
    constexpr std::uint64_t kOld = 1'000'000;
    constexpr std::uint64_t kRecent = 2'000'000;
    // Alice has three distinct scores across two dates; Bob one.
    store.Submit(kHash, 50, 1, kOld);
    store.Submit(kHash, 90, 1, kRecent);
    store.Submit(kHash, 70, 1, kRecent);
    store.Submit(kHash, 80, 2, kRecent);

    // Unfiltered: every retained run, best first.
    EXPECT_EQ(store.Top(kHash, 10).size(), 4U);
    EXPECT_EQ(store.Top(kHash, 10).front().score, 90);

    // Date filter drops the old run.
    TopOptions since{};
    since.since = kOld + 1;
    EXPECT_EQ(store.Top(kHash, 10, since).size(), 3U);

    // Best per account keeps one row each.
    TopOptions best{};
    best.best_per_account = true;
    const std::vector<ScoreEntry> collapsed = store.Top(kHash, 10, best);
    ASSERT_EQ(collapsed.size(), 2U);
    EXPECT_EQ(collapsed[0].score, 90);
    EXPECT_EQ(collapsed[1].score, 80);

    // Both filters compose.
    best.since = kRecent;
    EXPECT_EQ(store.Top(kHash, 10, best).size(), 2U);

    // A window after the newest score is empty.
    TopOptions future{};
    future.since = kRecent + 1;
    EXPECT_TRUE(store.Top(kHash, 10, future).empty());
}

// A weak player is never evicted by a flood of better players: retention is per
// account, not per board.
TEST(LeaderboardTest, StoreRetainsEveryAccount) {
    ScoreStore store;
    constexpr std::uint64_t kHash = 0x42;
    for (int i = 0; i < 200; ++i) {
        store.Submit(kHash, 10'000 + i, /*user=*/1, 1'700'000'000 + static_cast<std::uint64_t>(i));
    }
    const auto weak = store.Submit(kHash, 1, /*user=*/2, 1'700'100'000);
    EXPECT_EQ(weak.rank_accounts, 2);

    TopOptions best{};
    best.best_per_account = true;
    const std::vector<ScoreEntry> collapsed = store.Top(kHash, 100, best);
    ASSERT_EQ(collapsed.size(), 2U);
    EXPECT_EQ(collapsed[0].user_id, 1U);
    EXPECT_EQ(collapsed[1].user_id, 2U);
}

// Retention is bounded per account, and the first run to reach a score wins.
TEST(LeaderboardTest, StoreRetainsTopNAndKeepsEarlierTies) {
    ScoreStoreOptions options{};
    options.max_scores_per_account = 3;
    ScoreStore store(options);
    constexpr std::uint64_t kHash = 0x7;
    for (int i = 0; i < 10; ++i) {
        store.Submit(kHash, i, /*user=*/1, 1'000 + static_cast<std::uint64_t>(i));
    }
    EXPECT_EQ(store.Size(kHash), 3U);
    EXPECT_EQ(store.Top(kHash, 10).front().score, 9);

    ScoreStore tied;
    tied.Submit(kHash, 5, /*user=*/1, 100);
    tied.Submit(kHash, 5, /*user=*/1, 200);
    const std::vector<ScoreEntry> rows = tied.Top(kHash, 10);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.front().recorded_at, 100U); // the earlier run is kept
}

TEST(LeaderboardTest, StoreRankOfUsesTheAccountsBestRun) {
    ScoreStore store;
    constexpr std::uint64_t kHash = 0x9;
    store.Submit(kHash, 100, 1, 10);
    store.Submit(kHash, 50, 1, 11);
    store.Submit(kHash, 90, 2, 12);

    const std::optional<ScoreStore::RankInfo> first = store.RankOf(kHash, 1);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->rank_accounts, 1);
    EXPECT_EQ(first->rank_runs, 1);
    EXPECT_EQ(first->best, 100);

    const std::optional<ScoreStore::RankInfo> second = store.RankOf(kHash, 2);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->rank_accounts, 2);
    EXPECT_EQ(second->rank_runs, 2);
}

TEST(LeaderboardTest, StorePersistsPerConfigJson) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "vkengine_scores_store_test";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);

    {
        ScoreStoreOptions options{};
        options.directory = directory;
        ScoreStore store(options);
        store.Submit(0xABC, 55, 7, 1'700'000'000);
        store.Submit(0xABC, 65, 8, 1'700'000'001);
        store.Flush();
    }
    {
        ScoreStoreOptions options{};
        options.directory = directory;
        ScoreStore store(options);
        EXPECT_EQ(store.Size(0xABC), 2U);
        TopOptions best{};
        best.best_per_account = true;
        const std::vector<ScoreEntry> top = store.Top(0xABC, 10, best);
        ASSERT_EQ(top.size(), 2U);
        EXPECT_EQ(top.front().score, 65);
    }

    std::filesystem::remove_all(directory, ec);
}

TEST(LeaderboardTest, StoreImportsLegacyTxtAndDropsAnonymousRows) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "vkengine_scores_legacy_test";
    const std::filesystem::path legacy = directory / "legacy.txt";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    {
        std::ofstream out(legacy);
        out << "abc 10 5 1700000000 Alice\n";   // kept
        out << "abc 20 0 1700000001 Ghost\n";   // anonymous: dropped
        out << "abc 30\n";                      // pre-account: dropped
        out << "abc 40 5 1700000002 Alice\n";   // kept
    }
    ScoreStoreOptions options{};
    options.directory = directory;
    options.legacy_file = legacy;
    {
        ScoreStore store(options);
        EXPECT_EQ(store.Size(0xabc), 2U);
        EXPECT_EQ(store.Top(0xabc, 10).front().score, 40);
    }
    EXPECT_TRUE(std::filesystem::exists(legacy.string() + ".bak"));

    std::filesystem::remove_all(directory, ec);
}

// ── End to end ──

TEST(LeaderboardTest, EndToEndAccountFlow) {
    RunningServer server(TestServerOptions());
    RunningClient client(TestClientOptions(server.server));

    const AccountResult registered = client.client.Register("Alice", "Alice").get();
    ASSERT_TRUE(registered.ok()) << registered.message;
    EXPECT_EQ(registered.username, "alice");
    EXPECT_EQ(registered.token.size(), kTokenHexLength);

    const AccountResult login = client.client.Login("alice", registered.token).get();
    ASSERT_TRUE(login.ok()) << login.message;
    EXPECT_EQ(login.session_token.size(), kSessionHexLength);
    EXPECT_GT(login.session_expires_at, 0U);

    client.client.SubmitScore(42, /*best_per_account=*/true);
    ASSERT_TRUE(WaitFor(
        [&client] {
            const Client::Snapshot snapshot = client.client.GetSnapshot();
            return snapshot.connected && snapshot.has_rank;
        },
        std::chrono::seconds(5)));
    EXPECT_EQ(client.client.GetSnapshot().last_best, 42);
    EXPECT_EQ(client.client.GetSnapshot().last_rank_accounts, 1);

    client.client.RequestTop(5, true, 0, false);
    ASSERT_TRUE(WaitFor(
        [&client] { return !client.client.GetSnapshot().top.empty(); }, std::chrono::seconds(5)));
    ASSERT_EQ(client.client.GetSnapshot().top.front().score, 42);
    EXPECT_EQ(client.client.GetSnapshot().top.front().display_name, "Alice");
    EXPECT_NE(client.client.GetSnapshot().top.front().user_id, 0U);
    EXPECT_GT(client.client.GetSnapshot().top.front().recorded_at, 0U);

    // A rename is reflected on the score already recorded: names are resolved
    // live, not snapshotted into the row.
    const AccountResult renamed = client.client.Rename("Alice Cooper").get();
    ASSERT_TRUE(renamed.ok()) << renamed.message;
    EXPECT_EQ(renamed.display_name, "Alice Cooper");

    client.client.SubmitScore(7, /*best_per_account=*/false);
    client.client.RequestTop(5, false, 0, false);
    ASSERT_TRUE(WaitFor(
        [&client] { return client.client.GetSnapshot().top.size() == 2; }, std::chrono::seconds(5)));
    for (const TopEntry& entry : client.client.GetSnapshot().top) {
        EXPECT_EQ(entry.display_name, "Alice Cooper");
    }

    // Only-mine returns just this account's rows.
    client.client.RequestTop(5, false, 0, /*only_mine=*/true);
    ASSERT_TRUE(WaitFor(
        [&client] { return client.client.GetSnapshot().top.size() == 2; }, std::chrono::seconds(5)));

    // A date window that starts after both runs is empty.
    const std::uint64_t far_future =
        client.client.GetSnapshot().top.front().recorded_at + 86'400;
    client.client.RequestTop(5, false, far_future, false);
    ASSERT_TRUE(WaitFor(
        [&client] { return client.client.GetSnapshot().top.empty(); }, std::chrono::seconds(5)));

    // A wrong token is refused.
    const AccountResult bad =
        client.client.Login("alice", std::string(kTokenHexLength, '0')).get();
    EXPECT_EQ(bad.status, SyncStatus::InvalidToken);
}

TEST(LeaderboardTest, RenameRejectionCarriesReason) {
    RunningServer server(TestServerOptions());
    RunningClient client(TestClientOptions(server.server));
    ASSERT_TRUE(client.client.Register("bob", "Bob").get().ok());

    const AccountResult bad = client.client.Rename("bad\nname").get();
    EXPECT_EQ(bad.status, SyncStatus::Rejected);
    EXPECT_FALSE(bad.reason.empty());
}

TEST(LeaderboardTest, RegisterRespectsServerNamePolicy) {
    ServerOptions options = TestServerOptions();
    options.name_policy.blocked = {"admin"};
    RunningServer server(std::move(options));
    RunningClient client(TestClientOptions(server.server));

    const AccountResult bad = client.client.Register("carol", "The Admin").get();
    EXPECT_EQ(bad.status, SyncStatus::Rejected);
    EXPECT_FALSE(bad.reason.empty());
    EXPECT_FALSE(bad.message.empty());
}

TEST(LeaderboardTest, UnknownRulesetIsRejectedBeforeTheHandshake) {
    ServerOptions options = TestServerOptions();
    options.accept_unknown_configs = false;
    options.accepted_configs = {CurrentBalanceHash()};
    RunningServer server(std::move(options));

    ClientOptions client_options = TestClientOptions(server.server);
    client_options.config_hash = 0xDEADBEEF;
    RunningClient client(std::move(client_options));

    ASSERT_TRUE(WaitFor(
        [&client] {
            return client.client.GetSnapshot().status.find("unknown ruleset") != std::string::npos;
        },
        std::chrono::seconds(5)))
        << client.client.GetSnapshot().status;
}

TEST(LeaderboardTest, SessionResumeSkipsCredential) {
    RunningServer server(TestServerOptions());

    std::string session_token;
    {
        RunningClient client(TestClientOptions(server.server));
        const AccountResult registered = client.client.Register("carol", "Carol").get();
        ASSERT_TRUE(registered.ok()) << registered.message;
        const AccountResult login = client.client.Login("carol", registered.token).get();
        ASSERT_TRUE(login.ok()) << login.message;
        session_token = login.session_token;
    }

    RunningClient client(TestClientOptions(server.server));
    client.client.SetSession(session_token, 0);
    ASSERT_TRUE(WaitFor(
        [&client] { return client.client.GetSnapshot().authenticated; }, std::chrono::seconds(5)));
    EXPECT_EQ(client.client.GetSnapshot().account.username, "carol");
}

// A pinned-key mismatch is the most common cause of "offline" against a running
// server, so the status must name it rather than reporting a generic failure.
TEST(LeaderboardTest, HandshakeReportsIdentityMismatch) {
    RunningServer server(TestServerOptions());

    ClientOptions options = TestClientOptions(server.server);
    VulkanEngine::Security::X25519Key wrong_key{};
    wrong_key.fill(std::byte{0x11});
    options.server_public_key = wrong_key;

    RunningClient client(std::move(options));
    ASSERT_TRUE(WaitFor(
        [&client] {
            return client.client.GetSnapshot().status.find("identity mismatch") != std::string::npos;
        },
        std::chrono::seconds(5)))
        << client.client.GetSnapshot().status;
}

TEST(LeaderboardTest, ClientWithoutPinnedKeyStaysOffline) {
    RunningServer server(TestServerOptions());

    ClientOptions options = TestClientOptions(server.server);
    options.server_public_key.reset();

    RunningClient client(std::move(options));
    ASSERT_TRUE(WaitFor(
        [&client] {
            return client.client.GetSnapshot().status.find("public key") != std::string::npos;
        },
        std::chrono::seconds(5)))
        << client.client.GetSnapshot().status;
}

// Scores are persisted by the server, as one JSON board per ruleset.
TEST(LeaderboardTest, ServerPersistsScoresToDisk) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "vkengine_scores_server_test";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);

    {
        ServerOptions options = TestServerOptions();
        options.store_path = directory;
        RunningServer server(std::move(options));

        RunningClient client(TestClientOptions(server.server));
        ASSERT_TRUE(client.client.Register("dave", "Dave").get().ok());
        client.client.SubmitScore(321, /*best_per_account=*/true);
        ASSERT_TRUE(WaitFor([&client] { return client.client.GetSnapshot().has_rank; },
                            std::chrono::seconds(5)));
    }

    bool found = false;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, ec)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream stream(entry.path());
        const std::string contents{std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>()};
        if (contents.find("\"format\"") != std::string::npos &&
            contents.find("321") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "no board JSON under " << directory;

    std::filesystem::remove_all(directory, ec);
}

// Repeated connect/handshake/teardown against one server. This is where a
// hung accept or a stale connection that blocks the next one shows up as an
// intermittent handshake failure in play.
TEST(LeaderboardTest, RepeatedConnectionsAllHandshake) {
    RunningServer server(TestServerOptions());
    for (int i = 0; i < 25; ++i) {
        RunningClient client(TestClientOptions(server.server));
        const std::string username = std::format("user{}", i);
        const AccountResult registered = client.client.Register(username, username).get();
        ASSERT_TRUE(registered.ok()) << "iteration " << i << ": " << registered.message;
    }
}

// One idle connection must not block another client's handshake. Each
// connection is served on its own thread.
TEST(LeaderboardTest, SecondClientConnectsWhileFirstIsIdle) {
    RunningServer server(TestServerOptions());

    RunningClient idle(TestClientOptions(server.server));
    ASSERT_TRUE(idle.client.Register("idle", "Idle").get().ok());

    // `idle` stays connected and sends nothing.
    RunningClient active(TestClientOptions(server.server));
    std::future<AccountResult> future = active.client.Register("active", "Active");
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_TRUE(future.get().ok());
}

// ── Keepalive, idle handling and retries ──

TEST(LeaderboardTest, PingPongRoundTrip) {
    const std::vector<std::byte> encoded = Encode(PingMessage{0x1234567890ABCDEFULL});
    const std::optional<PingMessage> ping = DecodePing(encoded);
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->token, 0x1234567890ABCDEFULL);

    const std::optional<PongMessage> pong = DecodePong(Encode(PongMessage{ping->token}));
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(pong->token, 0x1234567890ABCDEFULL);

    // A truncated payload and a payload with trailing bytes are both refused.
    EXPECT_FALSE(DecodePing(std::span<const std::byte>(encoded).first(4)).has_value());
    std::vector<std::byte> extended = encoded;
    extended.push_back(std::byte{0});
    EXPECT_FALSE(DecodePing(extended).has_value());
}

// The server closes a connection that stays silent past its idle timeout; a
// periodic Ping must keep an otherwise idle client alive indefinitely.
TEST(LeaderboardTest, KeepaliveKeepsAnIdleConnectionOpen) {
    ServerOptions server_options = TestServerOptions();
    server_options.idle_timeout = std::chrono::seconds(1);
    server_options.socket_poll_interval = std::chrono::milliseconds(50);
    RunningServer server(std::move(server_options));

    ClientOptions client_options = TestClientOptions(server.server);
    client_options.keepalive_interval = std::chrono::milliseconds(250);
    client_options.keepalive_timeout = std::chrono::milliseconds(250);
    RunningClient client(std::move(client_options));

    const AccountResult registered = client.client.Register("keep", "Keep").get();
    ASSERT_TRUE(registered.ok()) << registered.message;
    const std::uint64_t connections = client.client.ConnectionCount();

    // Idle well past the server's idle timeout with no requests outstanding.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    EXPECT_EQ(client.client.ConnectionCount(), connections) << "keepalive did not hold the link";
    EXPECT_TRUE(client.client.GetSnapshot().authenticated);
    EXPECT_EQ(client.client.GetSnapshot().status, "connected");

    // The kept-warm connection still serves requests.
    const AccountResult renamed = client.client.Rename("Kept").get();
    ASSERT_TRUE(renamed.ok()) << renamed.message;
    EXPECT_EQ(renamed.display_name, "Kept");
}

// A request that lands on a connection the peer already dropped is re-sent
// after a reconnect instead of being reported as a failure.
TEST(LeaderboardTest, ADroppedConnectionIsRetriedAfterReconnect) {
    ServerOptions server_options = TestServerOptions();
    server_options.idle_timeout = std::chrono::seconds(1);
    server_options.socket_poll_interval = std::chrono::milliseconds(50);
    RunningServer server(std::move(server_options));

    ClientOptions client_options = TestClientOptions(server.server);
    // Disable the keepalive so the server drops the idle connection.
    client_options.keepalive_interval = std::chrono::hours(1);
    RunningClient client(std::move(client_options));

    const AccountResult registered = client.client.Register("retry", "Retry").get();
    ASSERT_TRUE(registered.ok()) << registered.message;
    const std::uint64_t connections = client.client.ConnectionCount();

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    const AccountResult renamed = client.client.Rename("Retried").get();
    ASSERT_TRUE(renamed.ok()) << renamed.message;
    EXPECT_EQ(renamed.display_name, "Retried");
    EXPECT_GT(client.client.ConnectionCount(), connections);
    EXPECT_GE(client.client.RetryCount(), 1U);
}

// Register is the one request that must not be replayed: its reply carries the
// only copy of the token, so a retry after a lost reply would be refused as a
// duplicate username. The caller is told instead.
TEST(LeaderboardTest, RegisterIsNotRetriedAfterATransportFailure) {
    ServerOptions server_options = TestServerOptions();
    server_options.idle_timeout = std::chrono::seconds(1);
    server_options.socket_poll_interval = std::chrono::milliseconds(50);
    RunningServer server(std::move(server_options));

    ClientOptions client_options = TestClientOptions(server.server);
    client_options.keepalive_interval = std::chrono::hours(1);
    RunningClient client(std::move(client_options));

    ASSERT_TRUE(client.client.Register("first", "First").get().ok());
    const std::uint64_t retries = client.client.RetryCount();

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    const AccountResult second = client.client.Register("second", "Second").get();
    EXPECT_FALSE(second.ok());
    EXPECT_EQ(second.status, SyncStatus::ServerError);
    EXPECT_NE(second.message.find("may or may not have been created"), std::string::npos);
    EXPECT_EQ(client.client.RetryCount(), retries) << "Register must not be retried";
}

#if !defined(_WIN32)
// A GUI process receives signals all the time; a blocking recv/send/select that
// returns EINTR has not failed and must be resumed.
TEST(LeaderboardTest, HandshakeSurvivesInterruptingSignals) {
    std::signal(SIGUSR1, [](int) {});

    std::atomic<bool> stop{false};
    std::thread signaller([&stop] {
        while (!stop.load()) {
            ::kill(::getpid(), SIGUSR1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    RunningServer server(TestServerOptions());
    RunningClient client(TestClientOptions(server.server));
    const AccountResult registered = client.client.Register("signal", "Signal").get();
    EXPECT_TRUE(registered.ok()) << registered.message;

    stop.store(true);
    signaller.join();
}
#endif

TEST(LeaderboardTest, LogSinkIsLevelFiltered) {
    std::mutex received_mutex;
    std::vector<std::pair<LogLevel, std::string>> received;
    SetLogSink([&](LogLevel level, std::string_view message) {
        std::lock_guard lock(received_mutex);
        received.emplace_back(level, std::string{message});
    });
    SetLogLevel(LogLevel::Warn);

    LogMessage(LogLevel::Debug, "below the floor");
    LogMessage(LogLevel::Info, "also below the floor");
    LogMessage(LogLevel::Warn, "at the floor");
    LogMessage(LogLevel::Error, "above the floor");

    {
        std::lock_guard lock(received_mutex);
        ASSERT_EQ(received.size(), 2U);
        EXPECT_EQ(received[0].first, LogLevel::Warn);
        EXPECT_EQ(received[0].second, "at the floor");
        EXPECT_EQ(received[1].first, LogLevel::Error);
    }

    SetLogLevel(LogLevel::Info);
    SetLogSink({});
}

} // namespace
