module;

#include <nlohmann/json.hpp>

module Examples.InfiniteRunner.Leaderboard.Store;

import std;

import Examples.InfiniteRunner.Leaderboard.Log;

namespace Examples::InfiniteRunner::Leaderboard {

namespace {

// Current on-disk schema. A file written by a newer schema is refused rather
// than silently downgraded and rewritten with data loss.
constexpr int kFormat = 2;

[[nodiscard]] bool AllDigits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

[[nodiscard]] std::optional<std::uint64_t> ParseHex64(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    try {
        return std::stoull(std::string{text}, nullptr, 16);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Legacy persistence line:
// "<hex hash> <score> <user id> <recorded at> <display name>". A line without
// the id/timestamp pair is pre-account and is dropped during migration.
[[nodiscard]] bool ParseLegacyLine(std::string_view line, std::uint64_t& hash,
                                   std::int32_t& score, std::uint64_t& user_id,
                                   std::uint64_t& recorded_at) {
    std::istringstream stream{std::string{line}};
    std::string hash_text;
    if (!(stream >> hash_text >> score)) {
        return false;
    }
    const std::optional<std::uint64_t> parsed_hash = ParseHex64(hash_text);
    if (!parsed_hash.has_value()) {
        return false;
    }
    hash = *parsed_hash;

    std::string rest;
    std::getline(stream, rest);
    std::istringstream fields{rest};
    std::string user_text;
    std::string time_text;
    if (!(fields >> user_text >> time_text) || !AllDigits(user_text) || !AllDigits(time_text)) {
        user_id = 0;
        recorded_at = 0;
        return true;
    }
    try {
        user_id = std::stoull(user_text);
        recorded_at = std::stoull(time_text);
    } catch (const std::exception&) {
        user_id = 0;
        recorded_at = 0;
    }
    return true;
}

} // namespace

bool ScoreStore::Better(const Row& a, const Row& b) {
    if (a.score != b.score) {
        return a.score > b.score;
    }
    if (a.recorded_at != b.recorded_at) {
        return a.recorded_at < b.recorded_at;
    }
    return a.seq < b.seq;
}

std::vector<const ScoreStore::Row*> ScoreStore::Ordered(const Board& board, bool best_per_account,
                                                        std::uint64_t since) {
    std::vector<const Row*> rows;
    for (const auto& [user, account_rows] : board.accounts) {
        (void)user;
        if (best_per_account) {
            for (const Row& row : account_rows) {
                if (since != 0 && row.recorded_at < since) {
                    continue;
                }
                rows.push_back(&row);
                break;
            }
        } else {
            for (const Row& row : account_rows) {
                if (since != 0 && row.recorded_at < since) {
                    continue;
                }
                rows.push_back(&row);
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row* a, const Row* b) { return Better(*a, *b); });
    return rows;
}

ScoreStore::RankInfo ScoreStore::RankOfBoard(const Board& board, std::uint64_t user_id) {
    RankInfo info{};
    const std::vector<const Row*> all = Ordered(board, /*best_per_account=*/false, 0);
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i]->user_id == user_id) {
            info.rank_runs = static_cast<std::int32_t>(i) + 1;
            break;
        }
    }
    info.best = all.empty() ? 0 : all.front()->score;

    const std::vector<const Row*> collapsed = Ordered(board, /*best_per_account=*/true, 0);
    for (std::size_t i = 0; i < collapsed.size(); ++i) {
        if (collapsed[i]->user_id == user_id) {
            info.rank_accounts = static_cast<std::int32_t>(i) + 1;
            break;
        }
    }
    return info;
}

ScoreStore::ScoreStore(ScoreStoreOptions options) : options_(std::move(options)) {
    std::error_code ec;
    if (!options_.directory.empty()) {
        std::filesystem::create_directories(options_.directory, ec);
        LoadDirectory();
    }
    if (!options_.legacy_file.empty() && std::filesystem::exists(options_.legacy_file, ec)) {
        LoadLegacy();
    }
}

ScoreStore::~ScoreStore() = default;

void ScoreStore::LoadDirectory() {
    std::error_code ec;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(options_.directory, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        LoadFile(entry.path());
    }
}

void ScoreStore::LoadFile(const std::filesystem::path& path) {
    const std::optional<std::uint64_t> hash = ParseHex64(path.stem().string());
    if (!hash.has_value()) {
        return;
    }
    std::ifstream stream(path);
    if (!stream) {
        return;
    }
    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception& error) {
        LogMessage(LogLevel::Error,
                   std::format("scores: cannot parse {} ({})", path.string(), error.what()));
        return;
    }
    const int format = document.value("format", 0);
    if (format > kFormat) {
        // A newer server owns this file. Refuse it loudly instead of rewriting
        // it with a schema this binary does not understand.
        LogMessage(LogLevel::Error,
                   std::format("scores: {} has format {} but this build understands {}; "
                               "refusing to load it",
                               path.string(), format, kFormat));
        return;
    }

    Board board;
    board.next_seq = document.value("next_seq", 1ULL);
    if (document.contains("accounts") && document["accounts"].is_object()) {
        for (auto it = document["accounts"].begin(); it != document["accounts"].end(); ++it) {
            if (!it.value().is_array()) {
                continue;
            }
            const std::optional<std::uint64_t> user = ParseHex64(it.key());
            if (!user.has_value() || *user == 0) {
                continue;
            }
            std::vector<Row>& rows = board.accounts[*user];
            for (const nlohmann::json& row : it.value()) {
                if (!row.is_object()) {
                    continue;
                }
                Row parsed{};
                parsed.score = row.value("score", 0);
                parsed.recorded_at = row.value("at", 0ULL);
                parsed.seq = row.contains("seq") ? row.value("seq", 0ULL) : board.next_seq++;
                parsed.user_id = *user;
                rows.push_back(parsed);
            }
            std::sort(rows.begin(), rows.end(), &ScoreStore::Better);
            if (rows.size() > options_.max_scores_per_account) {
                rows.resize(options_.max_scores_per_account);
            }
        }
    }
    for (const auto& [user, rows] : board.accounts) {
        for (const Row& row : rows) {
            board.next_seq = std::max(board.next_seq, row.seq + 1);
        }
    }
    boards_[*hash] = std::move(board);
}

void ScoreStore::LoadLegacy() {
    std::ifstream stream(options_.legacy_file);
    if (!stream) {
        return;
    }
    std::size_t imported = 0;
    std::string line;
    while (std::getline(stream, line)) {
        std::uint64_t hash = 0;
        std::int32_t score = 0;
        std::uint64_t user_id = 0;
        std::uint64_t recorded_at = 0;
        if (!ParseLegacyLine(line, hash, score, user_id, recorded_at)) {
            continue;
        }
        // Anonymous (pre-account) rows have no owner and are dropped: the
        // server no longer serves anonymous play.
        if (user_id == 0) {
            continue;
        }
        Board& board = boards_[hash];
        std::vector<Row>& rows = board.accounts[user_id];
        const bool present =
            std::any_of(rows.begin(), rows.end(), [score](const Row& row) { return row.score == score; });
        if (present) {
            continue;
        }
        rows.push_back(Row{score, user_id, recorded_at, board.next_seq++});
        ++imported;
    }
    for (auto& [hash, board] : boards_) {
        (void)hash;
        for (auto& [user, rows] : board.accounts) {
            (void)user;
            std::sort(rows.begin(), rows.end(), &ScoreStore::Better);
            if (rows.size() > options_.max_scores_per_account) {
                rows.resize(options_.max_scores_per_account);
            }
        }
        board.dirty = true;
    }
    if (imported > 0) {
        LogMessage(LogLevel::Info,
                   std::format("scores: imported {} rows from the legacy store {}", imported,
                               options_.legacy_file.string()));
    }
    Flush();

    std::error_code ec;
    std::filesystem::path backup = options_.legacy_file;
    backup += ".bak";
    std::filesystem::rename(options_.legacy_file, backup, ec);
    if (!ec) {
        LogMessage(LogLevel::Info,
                   std::format("scores: legacy store archived as {}", backup.string()));
    }
}

void ScoreStore::WriteBoard(std::uint64_t config_hash, const Board& board) const {
    if (options_.directory.empty()) {
        return;
    }
    nlohmann::json document;
    document["format"] = kFormat;
    document["next_seq"] = board.next_seq;
    nlohmann::json accounts = nlohmann::json::object();
    for (const auto& [user, rows] : board.accounts) {
        nlohmann::json entries = nlohmann::json::array();
        for (const Row& row : rows) {
            nlohmann::json entry;
            entry["score"] = row.score;
            entry["at"] = row.recorded_at;
            entry["seq"] = row.seq;
            entries.push_back(std::move(entry));
        }
        accounts[std::format("{:x}", user)] = std::move(entries);
    }
    document["accounts"] = std::move(accounts);

    const std::filesystem::path path =
        options_.directory / std::format("{:x}.json", config_hash);
    std::error_code ec;
    std::filesystem::path staging = path;
    staging += ".tmp";
    {
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        if (!out) {
            LogMessage(LogLevel::Error,
                       std::format("scores: cannot write {}", staging.string()));
            return;
        }
        out << document.dump();
        out.close();
        if (out.fail()) {
            LogMessage(LogLevel::Error,
                       std::format("scores: failed writing {}", staging.string()));
            std::filesystem::remove(staging, ec);
            return;
        }
    }
    std::filesystem::rename(staging, path, ec);
    if (ec) {
        LogMessage(LogLevel::Error, std::format("scores: cannot publish {}", path.string()));
        std::filesystem::remove(staging, ec);
    }
}

ScoreStore::SubmitResult ScoreStore::Submit(std::uint64_t config_hash, std::int32_t score,
                                            std::uint64_t user_id, std::uint64_t recorded_at) {
    std::lock_guard lock(mutex_);
    Board& board = boards_[config_hash];
    std::vector<Row>& rows = board.accounts[user_id];

    // Keep the first run to reach a score: a later equal score is discarded.
    const bool present =
        std::any_of(rows.begin(), rows.end(), [score](const Row& row) { return row.score == score; });
    if (!present) {
        rows.push_back(Row{score, user_id, recorded_at, board.next_seq++});
        std::sort(rows.begin(), rows.end(), &ScoreStore::Better);
        if (rows.size() > options_.max_scores_per_account) {
            rows.resize(options_.max_scores_per_account);
        }
        board.dirty = true;
    }

    const RankInfo info = RankOfBoard(board, user_id);
    return SubmitResult{info.rank_runs, info.rank_accounts, info.best};
}

std::vector<ScoreEntry> ScoreStore::Top(std::uint64_t config_hash, std::size_t count,
                                        const TopOptions& options) const {
    std::lock_guard lock(mutex_);
    std::vector<ScoreEntry> out;
    const auto it = boards_.find(config_hash);
    if (it == boards_.end()) {
        return out;
    }

    const std::vector<const Row*> rows =
        Ordered(it->second, options.best_per_account, options.since);
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (options.only_user != 0 && rows[i]->user_id != options.only_user) {
            continue;
        }
        out.push_back(ScoreEntry{static_cast<std::int32_t>(i) + 1, rows[i]->score, rows[i]->user_id,
                                 rows[i]->recorded_at});
        if (out.size() >= count) {
            break;
        }
    }
    return out;
}

std::optional<ScoreStore::RankInfo> ScoreStore::RankOf(std::uint64_t config_hash,
                                                       std::uint64_t user_id) const {
    std::lock_guard lock(mutex_);
    const auto it = boards_.find(config_hash);
    if (it == boards_.end()) {
        return RankInfo{};
    }
    return RankOfBoard(it->second, user_id);
}

std::size_t ScoreStore::Size(std::uint64_t config_hash) const {
    std::lock_guard lock(mutex_);
    const auto it = boards_.find(config_hash);
    if (it == boards_.end()) {
        return 0;
    }
    std::size_t total = 0;
    for (const auto& [user, rows] : it->second.accounts) {
        (void)user;
        total += rows.size();
    }
    return total;
}

void ScoreStore::Flush() {
    std::lock_guard lock(mutex_);
    for (auto& [hash, board] : boards_) {
        if (!board.dirty) {
            continue;
        }
        WriteBoard(hash, board);
        board.dirty = false;
    }
}

} // namespace Examples::InfiniteRunner::Leaderboard
