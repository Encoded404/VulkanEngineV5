module;

export module Examples.InfiniteRunner.Leaderboard.Store;

import std;

export namespace Examples::InfiniteRunner::Leaderboard {

// One ranked score row. Names are not stored: the display name is resolved
// from the account store at query time, so a rename touches no score data.
struct ScoreEntry {
    std::int32_t rank = 0;
    std::int32_t score = 0;
    std::uint64_t user_id = 0;
    std::uint64_t recorded_at = 0; // Unix seconds
};

// How a board query should be filtered.
struct TopOptions {
    // Keep only the best score per account, so one strong player cannot fill
    // the whole list with near-identical runs.
    bool best_per_account = false;
    // Only scores at or after this Unix time. 0 means no lower bound.
    std::uint64_t since = 0;
    // When non-zero, restrict the returned rows to this account. Ranks still
    // reflect the whole board, so a player sees their true position.
    std::uint64_t only_user = 0;
};

struct ScoreStoreOptions {
    // Directory holding one JSON board file per ruleset. Empty means in-memory
    // only (tests, ephemeral servers).
    std::filesystem::path directory{};
    // Optional legacy line-based file to import once, then rename to ".bak".
    std::filesystem::path legacy_file{};
    // Retained scores per account. Independent of the display count, so a
    // weaker player is never evicted by a flood of better players.
    std::size_t max_scores_per_account = 32;
};

// In-memory score boards, one per ruleset fingerprint, persisted as one JSON
// file per ruleset under ScoreStoreOptions::directory.
//
// Within an account, rows are ordered by best score, then oldest first, then
// insertion order. Ordering is total, so ranks are stable across restarts and
// equal scores keep the run that reached the score first.
class ScoreStore {
public:
    struct SubmitResult {
        std::int32_t rank_runs = 0;     // 1-based position of the account's best run
        std::int32_t rank_accounts = 0; // 1-based position among accounts
        std::int32_t best = 0;          // best score on the board
    };
    struct RankInfo {
        std::int32_t rank_runs = 0;
        std::int32_t rank_accounts = 0;
        std::int32_t best = 0;
    };

    explicit ScoreStore(ScoreStoreOptions options = {});
    ~ScoreStore();

    ScoreStore(const ScoreStore&) = delete;
    ScoreStore& operator=(const ScoreStore&) = delete;

    // Records a score and returns the account's fresh ranks. A score the account
    // already holds is discarded: the first run to reach a score is the one
    // kept.
    SubmitResult Submit(std::uint64_t config_hash, std::int32_t score, std::uint64_t user_id,
                        std::uint64_t recorded_at);

    // Best first, after applying `options`.
    [[nodiscard]] std::vector<ScoreEntry> Top(std::uint64_t config_hash, std::size_t count,
                                              const TopOptions& options = {}) const;
    [[nodiscard]] std::optional<RankInfo> RankOf(std::uint64_t config_hash,
                                                 std::uint64_t user_id) const;
    [[nodiscard]] std::size_t Size(std::uint64_t config_hash) const;

    // Writes every board modified since the last flush. Called on a timer and
    // on shutdown; a crash loses at most one flush interval.
    void Flush();

private:
    struct Row {
        std::int32_t score = 0;
        std::uint64_t user_id = 0;
        std::uint64_t recorded_at = 0;
        std::uint64_t seq = 0;
    };

    struct Board {
        std::map<std::uint64_t, std::vector<Row>> accounts;
        std::uint64_t next_seq = 1;
        bool dirty = false;
    };

    void LoadDirectory();
    void LoadLegacy();
    void LoadFile(const std::filesystem::path& path);
    void WriteBoard(std::uint64_t config_hash, const Board& board) const;

    // Total order: best score, then oldest first, then insertion order.
    [[nodiscard]] static bool Better(const Row& a, const Row& b);
    // Every row (or each account's best) at or after `since`, in rank order.
    [[nodiscard]] static std::vector<const Row*> Ordered(const Board& board, bool best_per_account,
                                                         std::uint64_t since);
    // Ranks of an account's best run. Caller holds mutex_.
    [[nodiscard]] static RankInfo RankOfBoard(const Board& board, std::uint64_t user_id);

    mutable std::mutex mutex_;
    ScoreStoreOptions options_;
    std::map<std::uint64_t, Board> boards_;
};

} // namespace Examples::InfiniteRunner::Leaderboard
