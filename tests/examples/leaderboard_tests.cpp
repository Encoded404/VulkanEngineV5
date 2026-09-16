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

TEST(LeaderboardTest, StoreSeparatesRulesetsAndKeepsNames) {
    ScoreStore store;
    const auto a = store.Submit(0xAAAA, 10, "Alice", 1, 1'700'000'000);
    const auto b = store.Submit(0xBBBB, 999, "Bob", 2, 1'700'000'100);
    EXPECT_EQ(a.rank, 1);
    EXPECT_EQ(b.rank, 1);
    EXPECT_EQ(store.Size(0xAAAA), 1U);
    ASSERT_EQ(store.Top(0xBBBB, 5).size(), 1U);
    EXPECT_EQ(store.Top(0xBBBB, 5).front().score, 999);
    EXPECT_EQ(store.Top(0xBBBB, 5).front().display_name, "Bob");
    EXPECT_EQ(store.Top(0xBBBB, 5).front().rank, 1);
    EXPECT_EQ(store.Top(0xBBBB, 5).front().user_id, 2U);
    EXPECT_EQ(store.Top(0xBBBB, 5).front().recorded_at, 1'700'000'100U);
}

TEST(LeaderboardTest, StoreFiltersByDateAndBestPerAccount) {
    ScoreStore store;
    constexpr std::uint64_t kHash = 0x99;
    constexpr std::uint64_t kOld = 1'000'000;
    constexpr std::uint64_t kRecent = 2'000'000;
    // Alice has three scores across two dates; Bob one.
    store.Submit(kHash, 50, "Alice", 1, kOld);
    store.Submit(kHash, 90, "Alice", 1, kRecent);
    store.Submit(kHash, 70, "Alice", 1, kRecent);
    store.Submit(kHash, 80, "Bob", 2, kRecent);

    // Unfiltered: every run, best first.
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

    client.client.SubmitScore(42);
    ASSERT_TRUE(WaitFor(
        [&client] {
            const Client::Snapshot snapshot = client.client.GetSnapshot();
            return snapshot.connected && snapshot.has_rank;
        },
        std::chrono::seconds(5)));
    EXPECT_EQ(client.client.GetSnapshot().last_best, 42);

    client.client.RequestTop(5);
    ASSERT_TRUE(WaitFor(
        [&client] { return !client.client.GetSnapshot().top.empty(); }, std::chrono::seconds(5)));
    ASSERT_EQ(client.client.GetSnapshot().top.front().score, 42);
    EXPECT_EQ(client.client.GetSnapshot().top.front().display_name, "Alice");
    // The server records who and when for every accepted score.
    EXPECT_NE(client.client.GetSnapshot().top.front().user_id, 0U);
    EXPECT_GT(client.client.GetSnapshot().top.front().recorded_at, 0U);

    // Hiding from the leaderboard stores no name for later submissions.
    SyncedSettings hidden{};
    hidden.show_on_leaderboard = false;
    const AccountResult updated = client.client.UpdateSettings("Alice Cooper", hidden).get();
    ASSERT_TRUE(updated.ok()) << updated.message;
    EXPECT_EQ(updated.display_name, "Alice Cooper");

    client.client.SubmitScore(7);
    // The board is only refreshed when asked, so request it again.
    client.client.RequestTop(5);
    ASSERT_TRUE(WaitFor(
        [&client] { return client.client.GetSnapshot().top.size() == 2; }, std::chrono::seconds(5)));
    EXPECT_EQ(client.client.GetSnapshot().top[1].score, 7);
    EXPECT_TRUE(client.client.GetSnapshot().top[1].display_name.empty());

    // Best-per-account collapses Alice's two runs into one row server-side.
    client.client.RequestTop(5, /*best_per_account=*/true, 0);
    ASSERT_TRUE(WaitFor(
        [&client] { return client.client.GetSnapshot().top.size() == 1; }, std::chrono::seconds(5)));
    EXPECT_EQ(client.client.GetSnapshot().top.front().score, 42);

    // A date window that starts after both runs is empty.
    const std::uint64_t far_future =
        client.client.GetSnapshot().top.front().recorded_at + 86'400;
    client.client.RequestTop(5, false, far_future);
    ASSERT_TRUE(WaitFor(
        [&client] { return client.client.GetSnapshot().top.empty(); }, std::chrono::seconds(5)));

    // A wrong token is refused.
    const AccountResult bad =
        client.client.Login("alice", std::string(kTokenHexLength, '0')).get();
    EXPECT_EQ(bad.status, SyncStatus::InvalidToken);
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

// Scores are persisted by the server, with user id and timestamp, not just
// held in memory.
TEST(LeaderboardTest, ServerPersistsScoresToDisk) {
    const std::filesystem::path scores =
        std::filesystem::temp_directory_path() / "vkengine_scores_test.txt";
    std::error_code ec;
    std::filesystem::remove(scores, ec);

    {
        ServerOptions options = TestServerOptions();
        options.store_path = scores.string();
        RunningServer server(std::move(options));

        RunningClient client(TestClientOptions(server.server));
        ASSERT_TRUE(client.client.Register("dave", "Dave").get().ok());
        client.client.SubmitScore(321);
        ASSERT_TRUE(WaitFor([&client] { return client.client.GetSnapshot().has_rank; },
                            std::chrono::seconds(5)));
    }

    std::ifstream stream(scores);
    ASSERT_TRUE(stream.good()) << "no score file at " << scores;
    const std::string contents{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
    EXPECT_NE(contents.find("321"), std::string::npos) << contents;
    EXPECT_NE(contents.find("Dave"), std::string::npos) << contents;
    // "<user id> <recorded at>" are present, so the date is saved too.
    EXPECT_NE(contents.find(' '), std::string::npos);

    std::filesystem::remove(scores, ec);
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
// connection is served on its own thread; before that, an idle client kept the
// single handler blocked in recv and every other handshake timed out.
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

#if !defined(_WIN32)
// A GUI process receives signals all the time; a blocking recv/send/select that
// returns EINTR has not failed and must be resumed. Without that, connections
// look randomly flaky in the game while tests stay green.
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

TEST(LeaderboardTest, UnauthenticatedSubmitIsRejected) {
    RunningServer server(TestServerOptions());

    ClientOptions options = TestClientOptions(server.server);
    // No server key: exercises the v1 PSK path, which cannot authenticate.
    options.server_public_key.reset();

    RunningClient client(std::move(options));
    client.client.SubmitScore(123);
    ASSERT_TRUE(WaitFor(
        [&client] {
            return client.client.GetSnapshot().status.find("authenticated") != std::string::npos;
        },
        std::chrono::seconds(5)));
    EXPECT_FALSE(client.client.GetSnapshot().has_rank);
}

} // namespace
