module;

module Examples.InfiniteRunner.Leaderboard.Store;

import std;

import Examples.InfiniteRunner.Leaderboard.Log;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

[[nodiscard]] bool AllDigits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// Persistence line: "<hex hash> <score> <user id> <recorded at> <display name>".
// A legacy line without the id/timestamp pair still parses; its date is then
// unknown (0), so date filters exclude it.
[[nodiscard]] bool ParseLine(std::string_view line, std::uint64_t& hash, std::int32_t& score,
                             std::uint64_t& user_id, std::uint64_t& recorded_at,
                             std::string& display_name) {
    std::istringstream stream{std::string{line}};
    std::string hash_text;
    if (!(stream >> hash_text >> score)) {
        return false;
    }
    try {
        hash = std::stoull(hash_text, nullptr, 16);
    } catch (const std::exception&) {
        return false;
    }

    std::string rest;
    std::getline(stream, rest);
    if (!rest.empty() && rest.front() == ' ') {
        rest.erase(rest.begin());
    }

    std::istringstream fields{rest};
    std::string user_text;
    std::string time_text;
    if ((fields >> user_text >> time_text) && AllDigits(user_text) && AllDigits(time_text)) {
        try {
            user_id = std::stoull(user_text);
            recorded_at = std::stoull(time_text);
        } catch (const std::exception&) {
            user_id = 0;
            recorded_at = 0;
        }
        std::getline(fields, rest);
        if (!rest.empty() && rest.front() == ' ') {
            rest.erase(rest.begin());
        }
        display_name = std::move(rest);
        return true;
    }

    user_id = 0;
    recorded_at = 0;
    display_name = std::move(rest);
    return true;
}

} // namespace

ScoreStore::ScoreStore(std::filesystem::path persist_path) : persist_path_(std::move(persist_path)) {
    if (persist_path_.empty()) {
        return;
    }
    // Opening a directory for append fails, and the store would silently keep
    // every score in memory only. Catch it loudly instead.
    std::error_code ec;
    if (std::filesystem::is_directory(persist_path_, ec)) {
        LogMessage(LogLevel::Error,
                   std::format("scores: {} is a directory, not a file; scores will not persist",
                               persist_path_.string()));
        persist_path_.clear();
        return;
    }
    Load();
}

void ScoreStore::Load() {
    std::ifstream stream(persist_path_);
    if (!stream) {
        return;
    }
    std::string line;
    while (std::getline(stream, line)) {
        std::uint64_t hash = 0;
        std::int32_t score = 0;
        std::uint64_t user_id = 0;
        std::uint64_t recorded_at = 0;
        std::string display_name;
        if (!ParseLine(line, hash, score, user_id, recorded_at, display_name)) {
            continue;
        }
        Row row;
        row.score = score;
        row.display_name = std::move(display_name);
        row.user_id = user_id;
        row.recorded_at = recorded_at;
        boards_[hash].push_back(std::move(row));
    }
    for (auto& [hash, board] : boards_) {
        std::stable_sort(board.begin(), board.end(), [](const Row& a, const Row& b) {
            return a.score > b.score;
        });
    }
}

void ScoreStore::Append(std::uint64_t config_hash, const Row& row) const {
    if (persist_path_.empty()) {
        return;
    }
    std::ofstream stream(persist_path_, std::ios::app);
    if (!stream) {
        return;
    }
    stream << std::hex << config_hash << std::dec << ' ' << row.score << ' ' << row.user_id << ' '
           << row.recorded_at << ' ' << row.display_name << '\n';
}

ScoreStore::SubmitResult ScoreStore::Submit(std::uint64_t config_hash, std::int32_t score,
                                            std::string_view display_name, std::uint64_t user_id,
                                            std::uint64_t recorded_at) {
    std::lock_guard lock(mutex_);
    std::vector<Row>& board = boards_[config_hash];
    board.push_back(Row{score, std::string{display_name}, user_id, recorded_at});
    std::stable_sort(board.begin(), board.end(), [](const Row& a, const Row& b) {
        return a.score > b.score;
    });
    if (board.size() > kMaxScoresPerBoard) {
        board.resize(kMaxScoresPerBoard);
    }

    // Rank by count of strictly better scores (ties share the best rank).
    const auto better = std::count_if(board.begin(), board.end(), [score](const Row& row) {
        return row.score > score;
    });

    SubmitResult result{};
    result.rank = static_cast<std::int32_t>(better) + 1;
    result.total = static_cast<std::int32_t>(board.size());
    result.best = board.front().score;
    Append(config_hash, Row{score, std::string{display_name}, user_id, recorded_at});
    return result;
}

std::vector<ScoreEntry> ScoreStore::Top(std::uint64_t config_hash, std::size_t count,
                                        const TopOptions& options) const {
    std::lock_guard lock(mutex_);
    std::vector<ScoreEntry> out;
    const auto it = boards_.find(config_hash);
    if (it == boards_.end()) {
        return out;
    }

    std::vector<const Row*> rows;
    rows.reserve(it->second.size());
    for (const Row& row : it->second) {
        if (options.since != 0 && row.recorded_at < options.since) {
            continue;
        }
        rows.push_back(&row);
    }

    if (options.best_per_account) {
        // Keep each account's best row; anonymous rows (user 0) are all kept
        // because they cannot be grouped.
        std::map<std::uint64_t, const Row*> best;
        std::vector<const Row*> anonymous;
        for (const Row* row : rows) {
            if (row->user_id == 0) {
                anonymous.push_back(row);
                continue;
            }
            const auto found = best.find(row->user_id);
            if (found == best.end() || row->score > found->second->score) {
                best[row->user_id] = row;
            }
        }
        rows.clear();
        rows.reserve(best.size() + anonymous.size());
        for (const auto& [user, row] : best) {
            rows.push_back(row);
        }
        rows.insert(rows.end(), anonymous.begin(), anonymous.end());
    }

    std::stable_sort(rows.begin(), rows.end(), [](const Row* a, const Row* b) {
        return a->score > b->score;
    });

    const std::size_t limit = std::min(count, rows.size());
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        out.push_back(ScoreEntry{static_cast<std::int32_t>(i) + 1, rows[i]->score,
                                 rows[i]->display_name, rows[i]->user_id, rows[i]->recorded_at});
    }
    return out;
}

std::size_t ScoreStore::Size(std::uint64_t config_hash) const {
    std::lock_guard lock(mutex_);
    const auto it = boards_.find(config_hash);
    return it == boards_.end() ? 0 : it->second.size();
}

} // namespace Examples::InfiniteRunner::Leaderboard
