module;

export module Examples.InfiniteRunner.Leaderboard.Store;

import std;

export namespace Examples::InfiniteRunner::Leaderboard {

// One ranked score row.
struct ScoreEntry {
    std::int32_t rank = 0;
    std::int32_t score = 0;
    std::string display_name;
    // Server-assigned: which account recorded it, and when.
    std::uint64_t user_id = 0;
    std::uint64_t recorded_at = 0; // Unix seconds; 0 when unknown (legacy data)
};

// How a board query should be filtered.
struct TopOptions {
    // Keep only the best score per account, so one strong player cannot fill
    // the whole list with near-identical runs.
    bool best_per_account = false;
    // Only scores at or after this Unix time. 0 means no lower bound.
    std::uint64_t since = 0;
};

// In-memory score boards, one per ruleset fingerprint, with optional
// append-only persistence. Top-N is kept per board so memory is bounded.
class ScoreStore {
public:
    struct SubmitResult {
        std::int32_t rank = 0;  // 1-based position of this score
        std::int32_t total = 0; // number of scores on the board
        std::int32_t best = 0;  // current best score
    };

    explicit ScoreStore(std::filesystem::path persist_path = {});

    SubmitResult Submit(std::uint64_t config_hash, std::int32_t score, std::string_view display_name,
                        std::uint64_t user_id, std::uint64_t recorded_at);

    // Best first, after applying `options`.
    [[nodiscard]] std::vector<ScoreEntry> Top(std::uint64_t config_hash, std::size_t count,
                                              const TopOptions& options = {}) const;
    [[nodiscard]] std::size_t Size(std::uint64_t config_hash) const;

private:
    static constexpr std::size_t kMaxScoresPerBoard = 1000;

    struct Row {
        std::int32_t score = 0;
        std::string display_name;
        std::uint64_t user_id = 0;
        std::uint64_t recorded_at = 0;
    };

    void Load();
    void Append(std::uint64_t config_hash, const Row& row) const;

    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::vector<Row>> boards_;
    std::filesystem::path persist_path_;
};

} // namespace Examples::InfiniteRunner::Leaderboard
